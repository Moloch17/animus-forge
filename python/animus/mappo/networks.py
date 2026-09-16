"""Layout-aware actor and centralized critic for MAPPO.

One set of weights serves every agent of every layout (parameter sharing). A layout is one kind of agent --
a class/role, say -- with its own observation features and actions. Each network has:

- an input adapter per layout: Linear(layout obs dim -> width), reading only that layout's features;
- a shared trunk: every hidden layer after the first, the same for all layouts;
- (actor) an action head per layout: Linear(width -> layout action count).

So what is learned about moving, threat, healing or interrupts is shared through the trunk, while every layout
keeps its exact observation and action spaces. For a single layout this is exactly the previous plain MLP, and
adapter + trunk + head of one layout is a plain MLP too (see animus.export).

The critic adds the global state: a state encoder Linear(state dim -> width) is summed with the agent's layout
adapter over its own observation, so each agent's value sees the whole env and its own situation
("agent-specific global state").

Agents of a layout are identified by their features, not by a seat index: the same network plays any seat.
"""

from __future__ import annotations

import math
from collections.abc import Sequence

import torch
from torch import nn
from torch.distributions import Categorical

MASKED_LOGIT = -1e9


def _linear(in_dim: int, out_dim: int, gain: float) -> nn.Linear:
    linear = nn.Linear(in_dim, out_dim)
    nn.init.orthogonal_(linear.weight, gain=gain)
    nn.init.zeros_(linear.bias)
    return linear


def masked_distribution(logits: torch.Tensor, mask: torch.Tensor) -> Categorical:
    """Categorical over allowed actions only. A row with nothing allowed falls back to action 0."""
    mask = mask.bool()
    empty = ~mask.any(dim=-1, keepdim=True)
    if empty.any():
        fallback = torch.zeros_like(mask)
        fallback[..., 0] = True
        mask = torch.where(empty, fallback, mask)
    return Categorical(logits=logits.masked_fill(~mask, MASKED_LOGIT))


class RunningNorm(nn.Module):
    """Per-feature mean and variance of the observations the policy has been trained on.

    The features arrive on wildly different scales -- a level in 1..80, yards, fractions of health, gear
    ratings -- and meet `tanh` as the first nonlinearity, which saturates on anything far from zero. Centring
    and scaling them is what lets the first layer see all of them at once.

    It is an affine map with no clipping, so `animus.export` can fold it into the adapter that follows and the
    exported model stays an ordinary MLP. The statistics are buffers: they travel in the checkpoint, and a
    stage that seeds from another carries them across for the blocks it keeps (animus.bootstrap).
    """

    def __init__(self, dim: int, epsilon: float = 1e-5):
        super().__init__()
        self.epsilon = epsilon
        self.register_buffer("mean", torch.zeros(dim))
        self.register_buffer("var", torch.ones(dim))
        self.register_buffer("count", torch.zeros(()))

    @torch.no_grad()
    def update(self, rows: torch.Tensor) -> None:
        """Fold a batch of observations into the statistics (Chan's parallel variance)."""
        if rows.shape[0] == 0:
            return

        batch_count = torch.tensor(float(rows.shape[0]), device=self.count.device)
        batch_mean, batch_var = rows.mean(dim=0), rows.var(dim=0, unbiased=False)
        total = self.count + batch_count
        delta = batch_mean - self.mean
        self.mean += delta * (batch_count / total)
        self.var.copy_((self.var * self.count + batch_var * batch_count
                        + delta.pow(2) * (self.count * batch_count / total)) / total)
        self.count.copy_(total)

    def forward(self, rows: torch.Tensor) -> torch.Tensor:
        if float(self.count) == 0.0:
            return rows  # nothing seen yet: the raw features are the best estimate of themselves

        return (rows - self.mean) / torch.sqrt(self.var + self.epsilon)

    @torch.no_grad()
    def scale(self) -> tuple[torch.Tensor, torch.Tensor]:
        """(mean, standard deviation) as the fold in `animus.export` needs them; identity before any update."""
        if float(self.count) == 0.0:
            return torch.zeros_like(self.mean), torch.ones_like(self.var)

        return self.mean.clone(), torch.sqrt(self.var + self.epsilon)


class _Trunk(nn.Module):
    """tanh, then Linear + tanh for every hidden layer after the first."""

    def __init__(self, hidden: Sequence[int]):
        super().__init__()
        self.layers = nn.ModuleList(_linear(a, b, math.sqrt(2)) for a, b in zip(hidden, hidden[1:]))

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = torch.tanh(x)
        for layer in self.layers:
            x = torch.tanh(layer(x))
        return x


def skip_distribution_checks() -> None:
    """Stop torch validating every Categorical it builds and every sample it scores.

    The checks (finite logits, a sample inside the support) run on every rollout decision and every minibatch of
    every epoch, and the logits here are built by this module, not by a user. Call it once at start-up.
    """
    torch.distributions.Distribution.set_default_validate_args(False)


@torch.no_grad()
def update_norms(norms: nn.ModuleList, obs: torch.Tensor, layout: torch.Tensor, obs_dims) -> None:
    """Fold a rollout's observations into each layout's statistics, each from its own rows and own features."""
    for index, rows in _per_layout(layout, len(norms)):
        norms[index].update(obs[rows, : obs_dims[index]])


def _per_layout(layout: torch.Tensor, count: int) -> list[tuple[int, torch.Tensor]]:
    """(layout, row indices) for every layout present in the batch."""
    if count == 1:
        return [(0, torch.arange(layout.shape[0], device=layout.device))]
    layout = layout.long()
    present = torch.unique(layout).tolist()
    return [(int(index), torch.nonzero(layout == index, as_tuple=True)[0]) for index in present]


class LayoutActor(nn.Module):
    def __init__(self, layouts: Sequence[tuple[int, int]], hidden: Sequence[int]):
        """layouts: (obs dim, action count) per layout; hidden: widths, the first being the adapters' output."""
        super().__init__()
        if not hidden:
            raise ValueError("the actor needs at least one hidden layer")
        self.obs_dims = [obs for obs, _ in layouts]
        self.action_counts = [actions for _, actions in layouts]
        self.max_actions = max(self.action_counts)
        self.norms = nn.ModuleList(RunningNorm(obs) for obs, _ in layouts)
        self.adapters = nn.ModuleList(_linear(obs, hidden[0], math.sqrt(2)) for obs, _ in layouts)
        self.trunk = _Trunk(hidden)
        self.heads = nn.ModuleList(_linear(hidden[-1], actions, 0.01) for _, actions in layouts)

    def forward(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor) -> Categorical:
        """obs [..., O], layout [...], mask [..., N] (padded) -> distribution over N actions."""
        lead = obs.shape[:-1]
        obs, layout, mask = obs.reshape(-1, obs.shape[-1]), layout.reshape(-1), mask.reshape(-1, mask.shape[-1])
        groups = _per_layout(layout, len(self.adapters))

        width = self.adapters[0].out_features
        hidden = obs.new_zeros(obs.shape[0], width)
        for index, rows in groups:
            hidden[rows] = self.adapters[index](self.norms[index](obs[rows, : self.obs_dims[index]]))
        hidden = self.trunk(hidden)

        logits = obs.new_full((obs.shape[0], mask.shape[-1]), MASKED_LOGIT)
        for index, rows in groups:
            logits[rows, : self.action_counts[index]] = self.heads[index](hidden[rows])

        dist = masked_distribution(logits, mask)
        if len(lead) != 1:
            dist = Categorical(logits=dist.logits.reshape(*lead, -1))
        return dist


class LayoutCritic(nn.Module):
    """V(global state, agent's own observation). Outputs a normalised value when ValueNorm is in use."""

    def __init__(self, state_dim: int, layouts: Sequence[tuple[int, int]], hidden: Sequence[int]):
        super().__init__()
        if not hidden:
            raise ValueError("the critic needs at least one hidden layer")
        self.obs_dims = [obs for obs, _ in layouts]
        self.state_norm = RunningNorm(state_dim)
        self.state_encoder = _linear(state_dim, hidden[0], math.sqrt(2))
        self.norms = nn.ModuleList(RunningNorm(obs) for obs, _ in layouts)
        self.adapters = nn.ModuleList(_linear(obs, hidden[0], math.sqrt(2)) for obs, _ in layouts)
        self.trunk = _Trunk(hidden)
        self.head = _linear(hidden[-1], 1, 1.0)

    def forward(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """state [..., S], obs [..., O], layout [...] -> value [...]."""
        lead = obs.shape[:-1]
        state, obs, layout = state.reshape(-1, state.shape[-1]), obs.reshape(-1, obs.shape[-1]), layout.reshape(-1)

        hidden = self.state_encoder(self.state_norm(state))
        own = torch.zeros_like(hidden)
        for index, rows in _per_layout(layout, len(self.adapters)):
            own[rows] = self.adapters[index](self.norms[index](obs[rows, : self.obs_dims[index]]))
        return self.head(self.trunk(hidden + own)).reshape(lead)
