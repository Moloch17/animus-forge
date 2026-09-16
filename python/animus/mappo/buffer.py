"""Rollout storage and GAE for auto-resetting vectorised envs.

Everything is numpy on the CPU: at [T, E, A] = [128, 64, 1] the whole rollout is a few MB, and GAE
is a single backwards loop over T.
"""

from __future__ import annotations

import numpy as np


def compute_gae(
    rewards: np.ndarray,
    values: np.ndarray,
    dones: np.ndarray,
    terminated: np.ndarray,
    final_values: np.ndarray,
    last_values: np.ndarray,
    gamma: float,
    gae_lambda: float,
) -> tuple[np.ndarray, np.ndarray]:
    """Generalised advantage estimation with correct time-limit handling.

    Shapes: rewards, values, final_values [T, E, A]; dones, terminated [T, E] (broadcast over
    agents); last_values [E, A] = V of the observation that follows the final step.

    For step t the successor value is:
      - values[t + 1] (or last_values) when the episode continued,
      - final_values[t] (V of the ended episode's last state) when it was truncated,
      - 0 when it terminated.
    The advantage recursion never crosses an episode boundary.

    Returns (advantages, returns), both [T, E, A].
    """
    steps = rewards.shape[0]
    advantages = np.zeros_like(rewards, dtype=np.float32)
    done = dones[..., None].astype(np.float32)
    terminal = terminated[..., None].astype(np.float32)

    gae = np.zeros_like(last_values, dtype=np.float32)
    for t in reversed(range(steps)):
        continued_value = last_values if t == steps - 1 else values[t + 1]
        next_value = (1.0 - done[t]) * continued_value + done[t] * (1.0 - terminal[t]) * final_values[t]
        delta = rewards[t] + gamma * next_value - values[t]
        gae = delta + gamma * gae_lambda * (1.0 - done[t]) * gae
        advantages[t] = gae

    return advantages, advantages + values


class RolloutBuffer:
    def __init__(self, steps: int, envs: int, agents: int, obs_dim: int, state_dim: int, num_actions: int):
        self.steps = steps
        shape = (steps, envs, agents)
        self.obs = np.zeros((*shape, obs_dim), dtype=np.float32)
        self.state = np.zeros((steps, envs, state_dim), dtype=np.float32)
        self.mask = np.zeros((*shape, num_actions), dtype=bool)
        self.layout = np.zeros(shape, dtype=np.int64)
        self.valid = np.ones(shape, dtype=bool)  # False for a seat without a character: not a sample
        self.actions = np.zeros(shape, dtype=np.int64)
        self.log_probs = np.zeros(shape, dtype=np.float32)
        self.values = np.zeros(shape, dtype=np.float32)  # denormalised
        self.rewards = np.zeros(shape, dtype=np.float32)
        self.dones = np.zeros((steps, envs), dtype=bool)
        self.terminated = np.zeros((steps, envs), dtype=bool)
        self.final_values = np.zeros(shape, dtype=np.float32)  # denormalised, valid where done
        self.advantages = np.zeros(shape, dtype=np.float32)
        self.returns = np.zeros(shape, dtype=np.float32)
        self.cursor = 0

    def add_decision(self, obs, state, mask, layout, actions, log_probs, values, present=None) -> None:
        """Record what the policy saw and did at step `cursor`; `present` [E, A] marks the agents with a character
        (default: all)."""
        t = self.cursor
        self.obs[t] = obs
        self.state[t] = state
        self.mask[t] = mask
        self.layout[t] = layout
        self.valid[t] = True if present is None else present
        self.actions[t] = actions
        self.log_probs[t] = log_probs
        self.values[t] = values

    def add_outcome(self, rewards, dones, terminated, final_values) -> None:
        """Record the result of the step-`cursor` actions and advance."""
        t = self.cursor
        self.rewards[t] = rewards
        self.dones[t] = dones
        self.terminated[t] = terminated
        self.final_values[t] = final_values
        self.cursor += 1

    @property
    def full(self) -> bool:
        return self.cursor >= self.steps

    def finish(self, last_values: np.ndarray, gamma: float, gae_lambda: float) -> None:
        self.advantages, self.returns = compute_gae(
            self.rewards,
            self.values,
            self.dones,
            self.terminated,
            self.final_values,
            last_values,
            gamma,
            gae_lambda,
        )

    def reset(self) -> None:
        self.cursor = 0

    def flat(self) -> dict[str, np.ndarray]:
        """Every valid per-agent sample flattened to [n, ...] (n = valid rows of T*E*A). State is repeated per agent.

        Seats without a character (``valid`` False) are left out: they only have the no-op and earn nothing, so as
        samples they would only dilute the advantages, the entropy and the value targets."""
        steps, envs, agents = self.actions.shape
        keep = self.valid.reshape(-1)
        # Boolean indexing copies, so torch gets writable arrays.
        state = np.broadcast_to(self.state[:, :, None, :], (steps, envs, agents, self.state.shape[-1]))
        return {
            "obs": self.obs.reshape(-1, self.obs.shape[-1])[keep],
            "state": state.reshape(-1, self.state.shape[-1])[keep],
            "layout": self.layout.reshape(-1)[keep],
            "mask": self.mask.reshape(-1, self.mask.shape[-1])[keep],
            "actions": self.actions.reshape(-1)[keep],
            "log_probs": self.log_probs.reshape(-1)[keep],
            "values": self.values.reshape(-1)[keep],
            "advantages": self.advantages.reshape(-1)[keep],
            "returns": self.returns.reshape(-1)[keep],
        }

    def mean_allowed_actions(self) -> float:
        """Mean legal actions per decision over the valid samples (0 when there are none).

        Entropy only means something against this: a policy over 6 legal actions and one over 60 have very
        different ceilings, and the masked action space here swings with level, cooldowns and the global cooldown.
        """
        if not self.valid.any():
            return 0.0

        return float(self.mask[self.valid].sum(axis=-1).mean())

    def mean_reward(self) -> float:
        """Mean reward per decision over the valid samples (0 when there are none)."""
        return float(self.rewards[self.valid].mean()) if self.valid.any() else 0.0
