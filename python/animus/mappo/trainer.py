"""MAPPO update: PPO-clip actor, clipped value loss on a normalised centralized critic."""

from __future__ import annotations

import copy
import time
from dataclasses import dataclass

import numpy as np
import torch
from torch import nn

from .buffer import RolloutBuffer
from .networks import LayoutActor, LayoutCritic, skip_distribution_checks
from .valuenorm import ValueNorm


@dataclass
class MappoConfig:
    hidden: tuple[int, ...] = (128, 128)
    # gamma and gae_lambda are per reference_decision_ms of game time, so a horizon means the same number of seconds
    # at any AnimusForge.DecisionMs (per_decision converts them).
    gamma: float = 0.99
    gae_lambda: float = 0.95
    reference_decision_ms: int = 100
    clip: float = 0.2
    value_clip: float = 0.2
    entropy_coef: float = 0.01
    value_coef: float = 1.0
    actor_lr: float = 5e-4
    critic_lr: float = 5e-4
    epochs: int = 5
    minibatches: int = 4
    max_grad_norm: float = 0.5
    use_value_norm: bool = True
    # How fast the value normaliser follows the returns, as the weight the running stats keep per update. The
    # returns drift as the policy improves, so stats that never follow leave the critic fitting a target measured
    # on a scale it has outgrown: 0.99 halves the old stats every ~69 updates, 0.99999 every ~69,000.
    value_norm_beta: float = 0.99
    # Normalise advantages within each layout rather than over the whole rollout. One mean and one standard
    # deviation across 18 class/roles with different reward scales lets the largest of them set the gradient of
    # the trunk they share. Groups smaller than this fall back to the rollout's own statistics.
    per_layout_advantages: bool = True
    min_layout_rows: int = 32
    # Stop an update early once its epochs have moved the policy this far in KL (0 = never). PPO's clipping
    # bounds each step, not the sum of a rollout's epochs.
    target_kl: float = 0.0


#: An epoch may exceed the target this far before the update stops: the measure is noisy over one epoch.
EPOCH_KL_TOLERANCE = 1.5


def per_decision(config: MappoConfig, decision_ms: int) -> tuple[float, float]:
    """(gamma, gae_lambda) for one decision of decision_ms: the configured per-reference values, compounded."""
    exponent = max(1, decision_ms) / max(1, config.reference_decision_ms)
    return config.gamma**exponent, config.gae_lambda**exponent


def horizon_seconds(discount: float, decision_ms: int) -> float:
    """The effective horizon 1 / (1 - discount), in seconds of game time."""
    return float("inf") if discount >= 1.0 else decision_ms / 1000.0 / (1.0 - discount)


class MappoTrainer:
    """Owns the networks. Rollouts run on `rollout_device` (a CPU copy is usually fastest for small
    MLPs at batch sizes of a few hundred); updates run on `train_device`."""

    def __init__(
        self,
        layouts: list[tuple[int, int]],
        state_dim: int,
        config: MappoConfig,
        train_device: str = "cpu",
        rollout_device: str = "cpu",
    ):
        """layouts: (obs dim, action count) per agent layout, in the sim's layout order."""
        skip_distribution_checks()
        self.config = config
        self.layouts = list(layouts)
        self.state_dim = state_dim
        self.train_device = torch.device(train_device)
        self.rollout_device = torch.device(rollout_device)
        # Starts at config.entropy_coef; the stage controller raises it after a restart (animus.stage).
        self.entropy_coef = config.entropy_coef

        hidden = list(config.hidden)
        self.actor = LayoutActor(self.layouts, hidden).to(self.train_device)
        self.critic = LayoutCritic(state_dim, self.layouts, hidden).to(self.train_device)
        self.value_norm = (ValueNorm(beta=config.value_norm_beta).to(self.train_device)
                           if config.use_value_norm else None)

        self.reset_optimizers()

        self._rollout_actor = copy.deepcopy(self.actor).to(self.rollout_device)
        self._rollout_critic = copy.deepcopy(self.critic).to(self.rollout_device)
        self._rollout_value_norm = (
            copy.deepcopy(self.value_norm).to(self.rollout_device) if self.value_norm is not None else None
        )

        # (trained tensor, rollout tensor) for every parameter and buffer the rollout networks mirror, paired
        # once here so a sync is a copy rather than a state dict.
        self._rollout_pairs = self._pair_tensors()

    def _pair_tensors(self) -> list[tuple[torch.Tensor, torch.Tensor]]:
        """Every tensor a rollout copy mirrors, next to the trained tensor it comes from."""
        trained = [self.actor, self.critic]
        rollout = [self._rollout_actor, self._rollout_critic]
        if self.value_norm is not None:
            trained.append(self.value_norm)
            rollout.append(self._rollout_value_norm)

        pairs = []
        for source, destination in zip(trained, rollout):
            for name, tensor in list(source.named_parameters()) + list(source.named_buffers()):
                pairs.append((tensor, dict(list(destination.named_parameters())
                                           + list(destination.named_buffers()))[name]))
        return pairs

    def reset_optimizers(self) -> None:
        """Fresh Adam state: after a restart the step sizes are no longer shrunk by the old gradient history."""
        self.actor_opt = torch.optim.Adam(self.actor.parameters(), lr=self.config.actor_lr, eps=1e-5)
        self.critic_opt = torch.optim.Adam(self.critic.parameters(), lr=self.config.critic_lr, eps=1e-5)

    @torch.no_grad()
    def shrink_perturb(self, shrink: float, perturb: float) -> None:
        """weights = shrink x weights + perturb x freshly initialised weights (Ash & Adams, 2020)."""
        hidden = list(self.config.hidden)
        fresh = (LayoutActor(self.layouts, hidden), LayoutCritic(self.state_dim, self.layouts, hidden))
        for network, init in zip((self.actor, self.critic), fresh):
            for param, init_param in zip(network.parameters(), init.to(self.train_device).parameters()):
                param.mul_(shrink).add_(init_param, alpha=perturb)
        self._sync_rollout()

    # ------------------------------------------------------------------ rollout

    def sync_rollout(self) -> None:
        """Copy the trained weights to the rollout networks. `update(sync=False)` leaves this to the caller, which
        overlapping an update with the next rollout needs: the rollout reads these copies while the update runs."""
        self._sync_rollout()

    @torch.no_grad()
    def _sync_rollout(self) -> None:
        # Copy tensor by tensor into the networks that are already there. Building a state dict and loading it
        # allocates a host copy of every parameter and buffer of both networks after every update, which with a
        # GPU is the whole model over the bus; the rollout copies only ever need the values.
        for source, destination in self._rollout_pairs:
            destination.copy_(source)

    def _tensor(self, array: np.ndarray, dtype=None) -> torch.Tensor:
        return torch.as_tensor(array, device=self.rollout_device, dtype=dtype)

    @torch.no_grad()
    def act(
        self, obs: np.ndarray, mask: np.ndarray, layout: np.ndarray, deterministic: bool = False
    ) -> tuple[np.ndarray, np.ndarray]:
        """obs [E, A, O], mask [E, A, N], layout [E, A] -> actions [E, A], log_probs [E, A].

        The rows go in flat and the answers are reshaped, so the actor builds one distribution per decision: a
        [E, A] batch would make it build a second one over the reshaped logits.
        """
        envs, agents = layout.shape
        rows = envs * agents
        dist = self._rollout_actor(
            self._tensor(obs).reshape(rows, -1),
            self._tensor(layout, torch.long).reshape(rows),
            self._tensor(mask).reshape(rows, -1),
        )
        # argmax over the logits is the argmax over the probabilities, without materialising them.
        actions = dist.logits.argmax(dim=-1) if deterministic else dist.sample()
        log_probs = dist.log_prob(actions)
        return (actions.reshape(envs, agents).cpu().numpy(), log_probs.reshape(envs, agents).cpu().numpy())

    @torch.no_grad()
    def value(self, state: np.ndarray, obs: np.ndarray, layout: np.ndarray) -> np.ndarray:
        """state [E, S], obs [E, A, O], layout [E, A] -> denormalised V per agent [E, A]."""
        envs, agents = layout.shape
        state_t = self._tensor(state)[:, None, :].expand(envs, agents, state.shape[-1])
        values = self._rollout_critic(state_t, self._tensor(obs), self._tensor(layout, torch.long))
        if self._rollout_value_norm is not None:
            values = self._rollout_value_norm.denormalize(values)
        return values.cpu().numpy()

    # ------------------------------------------------------------------ update

    def update(self, buffer: RolloutBuffer, auxiliary=None, sync: bool = True) -> dict[str, float]:
        """One PPO update over the rollout. `auxiliary(data, idx, dist)` may add a loss to each minibatch's actor
        loss: it returns (loss, {stat: value}) or None (see animus.distill)."""
        cfg = self.config
        started = time.perf_counter()
        stats = {"policy_loss": 0.0, "value_loss": 0.0, "entropy": 0.0, "clip_frac": 0.0, "approx_kl": 0.0,
                 "actor_grad_norm": 0.0, "critic_grad_norm": 0.0}
        data = {k: torch.as_tensor(v, device=self.train_device) for k, v in buffer.flat().items()}
        if data["actions"].shape[0] == 0:
            return stats  # no seat had a character this rollout: nothing to learn from

        data["advantages"] = self._normalise_advantages(data["advantages"], data["layout"])

        if self.value_norm is not None:
            self.value_norm.update(data["returns"])
            data["returns_target"] = self.value_norm.normalize(data["returns"])
            data["old_values"] = self.value_norm.normalize(data["values"])
        else:
            data["returns_target"] = data["returns"]
            data["old_values"] = data["values"]

        # How much of the returns' spread the critic already accounts for, on the values it produced during the
        # rollout. value_loss is reported in normalised space and shrinks with the normaliser, so it cannot say
        # whether the critic actually fits; this can. 0 = no better than predicting the mean, 1 = perfect.
        returns, values = data["returns"], data["values"]
        variance = returns.var()
        explained = 1.0 - (returns - values).var() / variance if float(variance) > 0.0 else torch.zeros(())

        samples = data["actions"].shape[0]
        # Even splits: `samples // minibatches` with a fixed stride leaves a remainder minibatch, which is a full
        # optimizer step on a fragment of the rollout.
        splits = max(1, min(cfg.minibatches, samples))
        auxiliary_stats: dict[str, float] = {}
        auxiliary_updates = 0
        updates = 0

        # Summed on the device and read once at the end: a .item() per statistic per minibatch is a pipeline stall
        # per statistic per minibatch.
        totals = {name: torch.zeros((), device=self.train_device) for name in stats}
        epochs_run = 0

        for _ in range(cfg.epochs):
            epoch_kl = torch.zeros((), device=self.train_device)
            epoch_updates = 0
            order = torch.randperm(samples, device=self.train_device)
            for idx in torch.tensor_split(order, splits):
                obs, layout = data["obs"][idx], data["layout"][idx]

                dist = self.actor(obs, layout, data["mask"][idx])
                log_probs = dist.log_prob(data["actions"][idx])
                log_ratio = log_probs - data["log_probs"][idx]
                ratio = log_ratio.exp()

                adv = data["advantages"][idx]
                policy_loss = -torch.min(ratio * adv, ratio.clamp(1 - cfg.clip, 1 + cfg.clip) * adv).mean()
                entropy = dist.entropy().mean()

                actor_loss = policy_loss - self.entropy_coef * entropy
                if auxiliary is not None and (extra := auxiliary(data, idx, dist)) is not None:
                    loss, extra_stats = extra
                    actor_loss = actor_loss + loss
                    for key, value in extra_stats.items():
                        auxiliary_stats[key] = auxiliary_stats.get(key, 0.0) + value
                    auxiliary_updates += 1

                self.actor_opt.zero_grad()
                actor_loss.backward()
                actor_grad = nn.utils.clip_grad_norm_(self.actor.parameters(), cfg.max_grad_norm)
                self.actor_opt.step()

                values = self.critic(data["state"][idx], obs, layout)
                old_values = data["old_values"][idx]
                target = data["returns_target"][idx]
                clipped = old_values + (values - old_values).clamp(-cfg.value_clip, cfg.value_clip)
                value_loss = torch.max((values - target) ** 2, (clipped - target) ** 2).mean()

                self.critic_opt.zero_grad()
                (cfg.value_coef * value_loss).backward()
                critic_grad = nn.utils.clip_grad_norm_(self.critic.parameters(), cfg.max_grad_norm)
                self.critic_opt.step()

                with torch.no_grad():
                    totals["policy_loss"] += policy_loss.detach()
                    totals["value_loss"] += value_loss.detach()
                    totals["entropy"] += entropy.detach()
                    totals["clip_frac"] += ((ratio - 1).abs() > cfg.clip).float().mean()
                    totals["approx_kl"] += ((ratio - 1) - log_ratio).mean()
                    totals["actor_grad_norm"] += actor_grad
                    totals["critic_grad_norm"] += critic_grad
                    epoch_kl += ((ratio - 1) - log_ratio).mean()
                updates += 1
                epoch_updates += 1

            epochs_run += 1
            # One read per epoch, not per minibatch: enough to stop before the next epoch pulls the policy
            # further from the rollout that justified it.
            if cfg.target_kl > 0.0 and epoch_updates \
                    and float(epoch_kl) / epoch_updates > EPOCH_KL_TOLERANCE * cfg.target_kl:
                break

        if sync:
            self._sync_rollout()
        stats = {name: float(total) for name, total in totals.items()}
        result = {k: v / max(1, updates) for k, v in stats.items()}
        result["explained_variance"] = float(explained)
        result["epochs_run"] = float(epochs_run)
        result.update({k: v / auxiliary_updates for k, v in auxiliary_stats.items()})
        # What the update itself cost. With overlap_updates the run's own timer measures the wait for this to
        # finish, not the work, so without this the work is invisible.
        result["update_compute_seconds"] = time.perf_counter() - started
        return result

    def _normalise_advantages(self, advantages: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """Centre and scale the advantages, within each layout when there are enough rows of it.

        The layouts of one rollout have their own return scales (a duel against a creature and a healer keeping a
        party alive are not the same numbers), and they share a trunk: one global scale lets the widest-spread
        layout speak loudest for weights every layout uses."""
        normalised = (advantages - advantages.mean()) / (advantages.std() + 1e-8)
        if not self.config.per_layout_advantages or len(self.layouts) < 2:
            return normalised

        for index in torch.unique(layout).tolist():
            rows = layout == int(index)
            if int(rows.sum()) < self.config.min_layout_rows:
                continue  # too few to measure a spread with; the rollout's own is the better estimate

            group = advantages[rows]
            normalised[rows] = (group - group.mean()) / (group.std() + 1e-8)

        return normalised

    # ------------------------------------------------------------------ checkpoints

    def state_dict(self) -> dict:
        return {
            "actor": self.actor.state_dict(),
            "critic": self.critic.state_dict(),
            "value_norm": self.value_norm.state_dict() if self.value_norm is not None else None,
            "actor_opt": self.actor_opt.state_dict(),
            "critic_opt": self.critic_opt.state_dict(),
        }

    def load_state_dict(self, state: dict, load_optimizers: bool = True) -> None:
        self.actor.load_state_dict(state["actor"])
        self.critic.load_state_dict(state["critic"])
        if self.value_norm is not None and state.get("value_norm") is not None:
            self.value_norm.load_state_dict(state["value_norm"])
        if load_optimizers:
            self.actor_opt.load_state_dict(state["actor_opt"])
            self.critic_opt.load_state_dict(state["critic_opt"])
        self._sync_rollout()
