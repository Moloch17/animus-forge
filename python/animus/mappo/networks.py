"""Actor and centralized critic for MAPPO.

Both networks take a one-hot agent id so a single set of weights serves every agent (parameter
sharing), while agents still receive their own observations and choose independently.
"""

from __future__ import annotations

import math

import torch
from torch import nn
from torch.distributions import Categorical

MASKED_LOGIT = -1e9


def _mlp(in_dim: int, hidden: list[int], out_dim: int, out_gain: float) -> nn.Sequential:
    layers: list[nn.Module] = []
    last = in_dim
    for width in hidden:
        linear = nn.Linear(last, width)
        nn.init.orthogonal_(linear.weight, gain=math.sqrt(2))
        nn.init.zeros_(linear.bias)
        layers += [linear, nn.Tanh()]
        last = width
    head = nn.Linear(last, out_dim)
    nn.init.orthogonal_(head.weight, gain=out_gain)
    nn.init.zeros_(head.bias)
    layers.append(head)
    return nn.Sequential(*layers)


def one_hot_agents(agent_id: torch.Tensor, num_agents: int) -> torch.Tensor:
    return nn.functional.one_hot(agent_id.long(), num_agents).to(torch.float32)


def masked_distribution(logits: torch.Tensor, mask: torch.Tensor) -> Categorical:
    """Categorical over allowed actions only. A row with nothing allowed falls back to action 0."""
    mask = mask.bool()
    empty = ~mask.any(dim=-1, keepdim=True)
    if empty.any():
        fallback = torch.zeros_like(mask)
        fallback[..., 0] = True
        mask = torch.where(empty, fallback, mask)
    return Categorical(logits=logits.masked_fill(~mask, MASKED_LOGIT))


class Actor(nn.Module):
    def __init__(self, obs_dim: int, num_actions: int, num_agents: int, hidden: list[int]):
        super().__init__()
        self.num_agents = num_agents
        self.net = _mlp(obs_dim + num_agents, hidden, num_actions, out_gain=0.01)

    def forward(self, obs: torch.Tensor, agent_id: torch.Tensor, mask: torch.Tensor) -> Categorical:
        logits = self.net(torch.cat([obs, one_hot_agents(agent_id, self.num_agents)], dim=-1))
        return masked_distribution(logits, mask)


class Critic(nn.Module):
    """V(global state, agent). Outputs a normalised value when ValueNorm is in use."""

    def __init__(self, state_dim: int, num_agents: int, hidden: list[int]):
        super().__init__()
        self.num_agents = num_agents
        self.net = _mlp(state_dim + num_agents, hidden, 1, out_gain=1.0)

    def forward(self, state: torch.Tensor, agent_id: torch.Tensor) -> torch.Tensor:
        return self.net(torch.cat([state, one_hot_agents(agent_id, self.num_agents)], dim=-1)).squeeze(-1)
