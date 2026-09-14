"""Seed a curriculum stage's networks from the previous stage's checkpoint.

A later stage (e.g. `warrior_dps_duel`) keeps the earlier stage's (`warrior_dps`) observation and
action layouts as a prefix and appends its own features and actions. Its networks therefore contain
the earlier ones: this copies every weight that has a counterpart and leaves the rest as initialised.

- First layers (actor and critic): the earlier stage's input columns keep their positions, the
  one-hot agent columns move to the end of the wider input, and the new feature columns start at
  zero so the seeded policy initially ignores them.
- Hidden layers: copied (the hidden sizes must match).
- Actor output: the earlier actions' rows are copied; new actions keep their small initial weights.
- Critic output: kept freshly initialised, and the value normaliser is not copied, because the
  later stage's reward has a different scale.
"""

from __future__ import annotations

import re

import torch

_LINEAR_KEY = re.compile(r"^net\.(\d+)\.(weight|bias)$")


def _linear_indices(state: dict[str, torch.Tensor]) -> list[int]:
    return sorted({int(m.group(1)) for key in state if (m := _LINEAR_KEY.match(key))})


def seed_network(
    new_state: dict[str, torch.Tensor],
    old_state: dict[str, torch.Tensor],
    old_obs_dim: int,
    new_obs_dim: int,
    num_agents: int,
    copy_head: bool,
) -> dict[str, torch.Tensor]:
    """Return new_state with old_state's weights copied in (see the module docstring)."""
    new_layers = _linear_indices(new_state)
    old_layers = _linear_indices(old_state)
    if len(new_layers) != len(old_layers):
        raise ValueError(f"{len(old_layers)} layers in the checkpoint, {len(new_layers)} in the new network")

    seeded = {key: tensor.clone() for key, tensor in new_state.items()}
    for position, (new_index, old_index) in enumerate(zip(new_layers, old_layers)):
        new_w, old_w = seeded[f"net.{new_index}.weight"], old_state[f"net.{old_index}.weight"]
        new_b, old_b = seeded[f"net.{new_index}.bias"], old_state[f"net.{old_index}.bias"]
        first, last = position == 0, position == len(new_layers) - 1

        if first:
            if old_w.shape[1] != old_obs_dim + num_agents or new_w.shape[1] != new_obs_dim + num_agents:
                raise ValueError("first layer inputs do not match the observation sizes")
            if new_w.shape[0] != old_w.shape[0]:
                raise ValueError(f"hidden size {old_w.shape[0]} in the checkpoint, {new_w.shape[0]} now")
            new_w.zero_()
            new_w[:, :old_obs_dim] = old_w[:, :old_obs_dim]
            new_w[:, new_obs_dim:] = old_w[:, old_obs_dim:]
            new_b.copy_(old_b)
        elif last:
            if not copy_head:
                continue
            rows = old_w.shape[0]
            if new_w.shape[0] < rows or new_w.shape[1] != old_w.shape[1]:
                raise ValueError(f"output layer {tuple(old_w.shape)} does not fit in {tuple(new_w.shape)}")
            new_w[:rows] = old_w
            new_b[:rows] = old_b
        else:
            if new_w.shape != old_w.shape:
                raise ValueError(f"hidden layer {tuple(old_w.shape)} in the checkpoint, {tuple(new_w.shape)} now")
            new_w.copy_(old_w)
            new_b.copy_(old_b)

    return seeded


def seed_trainer(trainer, checkpoint: dict, spec) -> None:
    """Seed a fresh MappoTrainer for `spec` from an earlier stage's checkpoint."""
    old_spec = checkpoint["spec"]
    if old_spec["agents_per_env"] != spec.agents_per_env:
        raise ValueError("the checkpoint has a different number of agents per env")
    if old_spec["obs_dim"] > spec.obs_dim or old_spec["num_actions"] > spec.num_actions:
        raise ValueError(
            f"checkpoint obs {old_spec['obs_dim']} / actions {old_spec['num_actions']} do not fit in "
            f"obs {spec.obs_dim} / actions {spec.num_actions}"
        )

    old = checkpoint["trainer"]
    actor = seed_network(
        trainer.actor.state_dict(), old["actor"], old_spec["obs_dim"], spec.obs_dim, spec.agents_per_env, True
    )
    critic = seed_network(
        trainer.critic.state_dict(), old["critic"], old_spec["state_dim"], spec.state_dim, spec.agents_per_env, False
    )
    trainer.actor.load_state_dict(actor)
    trainer.critic.load_state_dict(critic)
    trainer._sync_rollout()
