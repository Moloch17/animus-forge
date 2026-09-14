"""MAPPO update: PPO-clip actor, clipped value loss on a normalised centralized critic."""

from __future__ import annotations

import copy
from dataclasses import dataclass

import numpy as np
import torch
from torch import nn

from .buffer import RolloutBuffer
from .networks import Actor, Critic
from .valuenorm import ValueNorm


@dataclass
class MappoConfig:
    hidden: tuple[int, ...] = (128, 128)
    gamma: float = 0.99
    gae_lambda: float = 0.95
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


class MappoTrainer:
    """Owns the networks. Rollouts run on `rollout_device` (a CPU copy is usually fastest for small
    MLPs at batch sizes of a few hundred); updates run on `train_device`."""

    def __init__(
        self,
        obs_dim: int,
        state_dim: int,
        num_actions: int,
        num_agents: int,
        config: MappoConfig,
        train_device: str = "cpu",
        rollout_device: str = "cpu",
    ):
        self.config = config
        self.num_agents = num_agents
        self.train_device = torch.device(train_device)
        self.rollout_device = torch.device(rollout_device)

        hidden = list(config.hidden)
        self.actor = Actor(obs_dim, num_actions, num_agents, hidden).to(self.train_device)
        self.critic = Critic(state_dim, num_agents, hidden).to(self.train_device)
        self.value_norm = ValueNorm().to(self.train_device) if config.use_value_norm else None

        self.actor_opt = torch.optim.Adam(self.actor.parameters(), lr=config.actor_lr, eps=1e-5)
        self.critic_opt = torch.optim.Adam(self.critic.parameters(), lr=config.critic_lr, eps=1e-5)

        self._rollout_actor = copy.deepcopy(self.actor).to(self.rollout_device)
        self._rollout_critic = copy.deepcopy(self.critic).to(self.rollout_device)
        self._rollout_value_norm = (
            copy.deepcopy(self.value_norm).to(self.rollout_device) if self.value_norm is not None else None
        )

    # ------------------------------------------------------------------ rollout

    def _sync_rollout(self) -> None:
        self._rollout_actor.load_state_dict(self.actor.state_dict())
        self._rollout_critic.load_state_dict(self.critic.state_dict())
        if self.value_norm is not None:
            self._rollout_value_norm.load_state_dict(self.value_norm.state_dict())

    def _agent_ids(self, envs: int) -> torch.Tensor:
        return torch.arange(self.num_agents, device=self.rollout_device).expand(envs, self.num_agents)

    @torch.no_grad()
    def act(self, obs: np.ndarray, mask: np.ndarray, deterministic: bool = False) -> tuple[np.ndarray, np.ndarray]:
        """obs [E, A, O], mask [E, A, N] -> actions [E, A], log_probs [E, A]."""
        obs_t = torch.as_tensor(obs, device=self.rollout_device)
        mask_t = torch.as_tensor(mask, device=self.rollout_device)
        dist = self._rollout_actor(obs_t, self._agent_ids(obs.shape[0]), mask_t)
        actions = dist.probs.argmax(dim=-1) if deterministic else dist.sample()
        return actions.cpu().numpy(), dist.log_prob(actions).cpu().numpy()

    @torch.no_grad()
    def value(self, state: np.ndarray) -> np.ndarray:
        """state [E, S] -> denormalised V per agent [E, A]."""
        envs = state.shape[0]
        state_t = torch.as_tensor(state, device=self.rollout_device)
        state_t = state_t[:, None, :].expand(envs, self.num_agents, state.shape[-1])
        values = self._rollout_critic(state_t, self._agent_ids(envs))
        if self._rollout_value_norm is not None:
            values = self._rollout_value_norm.denormalize(values)
        return values.cpu().numpy()

    # ------------------------------------------------------------------ update

    def update(self, buffer: RolloutBuffer) -> dict[str, float]:
        cfg = self.config
        data = {k: torch.as_tensor(v, device=self.train_device) for k, v in buffer.flat().items()}

        advantages = data["advantages"]
        data["advantages"] = (advantages - advantages.mean()) / (advantages.std() + 1e-8)

        if self.value_norm is not None:
            self.value_norm.update(data["returns"])
            data["returns_target"] = self.value_norm.normalize(data["returns"])
            data["old_values"] = self.value_norm.normalize(data["values"])
        else:
            data["returns_target"] = data["returns"]
            data["old_values"] = data["values"]

        samples = data["actions"].shape[0]
        batch = max(1, samples // cfg.minibatches)
        stats = {"policy_loss": 0.0, "value_loss": 0.0, "entropy": 0.0, "clip_frac": 0.0, "approx_kl": 0.0}
        updates = 0

        for _ in range(cfg.epochs):
            order = torch.randperm(samples, device=self.train_device)
            for start in range(0, samples, batch):
                idx = order[start : start + batch]

                dist = self.actor(data["obs"][idx], data["agent_id"][idx], data["mask"][idx])
                log_probs = dist.log_prob(data["actions"][idx])
                log_ratio = log_probs - data["log_probs"][idx]
                ratio = log_ratio.exp()

                adv = data["advantages"][idx]
                policy_loss = -torch.min(ratio * adv, ratio.clamp(1 - cfg.clip, 1 + cfg.clip) * adv).mean()
                entropy = dist.entropy().mean()

                self.actor_opt.zero_grad()
                (policy_loss - cfg.entropy_coef * entropy).backward()
                nn.utils.clip_grad_norm_(self.actor.parameters(), cfg.max_grad_norm)
                self.actor_opt.step()

                values = self.critic(data["state"][idx], data["agent_id"][idx])
                old_values = data["old_values"][idx]
                target = data["returns_target"][idx]
                clipped = old_values + (values - old_values).clamp(-cfg.value_clip, cfg.value_clip)
                value_loss = torch.max((values - target) ** 2, (clipped - target) ** 2).mean()

                self.critic_opt.zero_grad()
                (cfg.value_coef * value_loss).backward()
                nn.utils.clip_grad_norm_(self.critic.parameters(), cfg.max_grad_norm)
                self.critic_opt.step()

                with torch.no_grad():
                    stats["policy_loss"] += policy_loss.item()
                    stats["value_loss"] += value_loss.item()
                    stats["entropy"] += entropy.item()
                    stats["clip_frac"] += ((ratio - 1).abs() > cfg.clip).float().mean().item()
                    stats["approx_kl"] += ((ratio - 1) - log_ratio).mean().item()
                updates += 1

        self._sync_rollout()
        return {k: v / max(1, updates) for k, v in stats.items()}

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
