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

    def add_decision(self, obs, state, mask, layout, actions, log_probs, values) -> None:
        """Record what the policy saw and did at step `cursor`."""
        t = self.cursor
        self.obs[t] = obs
        self.state[t] = state
        self.mask[t] = mask
        self.layout[t] = layout
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
        """Every per-agent sample flattened to [T*E*A, ...]. State is repeated per agent."""
        steps, envs, agents = self.actions.shape
        n = steps * envs * agents
        # Broadcast views are read-only; materialise them so torch gets writable arrays.
        state = np.broadcast_to(self.state[:, :, None, :], (steps, envs, agents, self.state.shape[-1])).copy()
        return {
            "obs": self.obs.reshape(n, -1),
            "state": state.reshape(n, -1),
            "layout": self.layout.reshape(n),
            "mask": self.mask.reshape(n, -1),
            "actions": self.actions.reshape(n),
            "log_probs": self.log_probs.reshape(n),
            "values": self.values.reshape(n),
            "advantages": self.advantages.reshape(n),
            "returns": self.returns.reshape(n),
        }
