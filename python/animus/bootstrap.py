"""Seed a curriculum stage's networks from the previous stage's checkpoint.

Networks are layout-aware (animus.mappo.networks): a per-layout input adapter, a shared trunk and (actor) a
per-layout action head. A later stage keeps each layout's earlier observation and action layout as a prefix and
appends its own features and actions, and layouts are matched by name (the class/role), so:

- Input adapters (actor and critic): the earlier layout's feature columns keep their positions; the new feature
  columns start at zero, so the seeded policy initially ignores them.
- Trunk: copied (the hidden sizes must match).
- Actor heads: the earlier actions' rows are copied; new actions keep their small initial weights.
- A layout the checkpoint does not have keeps its fresh adapter and head, and still gets the copied trunk.
- Critic state encoder and value head: kept freshly initialised, and the value normaliser is not copied, because
  the later stage's global state and reward differ.
"""

from __future__ import annotations

import torch


def _seed_adapter(new: dict, old: dict, prefix: str) -> None:
    new_w, old_w = new[f"{prefix}.weight"], old[f"{prefix}.weight"]
    if new_w.shape[0] != old_w.shape[0]:
        raise ValueError(f"{prefix}: width {old_w.shape[0]} in the checkpoint, {new_w.shape[0]} now")
    if old_w.shape[1] > new_w.shape[1]:
        raise ValueError(f"{prefix}: {old_w.shape[1]} inputs in the checkpoint do not fit in {new_w.shape[1]}")
    new_w.zero_()
    new_w[:, : old_w.shape[1]] = old_w
    new[f"{prefix}.bias"].copy_(old[f"{prefix}.bias"])


def _seed_head(new: dict, old: dict, prefix: str) -> None:
    new_w, old_w = new[f"{prefix}.weight"], old[f"{prefix}.weight"]
    rows = old_w.shape[0]
    if new_w.shape[0] < rows or new_w.shape[1] != old_w.shape[1]:
        raise ValueError(f"{prefix}: {tuple(old_w.shape)} does not fit in {tuple(new_w.shape)}")
    new_w[:rows] = old_w
    new[f"{prefix}.bias"][:rows] = old[f"{prefix}.bias"]


def _seed_trunk(new: dict, old: dict) -> None:
    for key, tensor in new.items():
        if key.startswith("trunk."):
            if key not in old or old[key].shape != tensor.shape:
                raise ValueError(f"{key}: the trunk in the checkpoint does not match (hidden sizes must be equal)")
            tensor.copy_(old[key])


def seed_trainer(trainer, checkpoint: dict, spec) -> list[str]:
    """Seed a fresh MappoTrainer for `spec` from an earlier stage's checkpoint; returns the layouts seeded."""
    old_names = [layout["name"] for layout in checkpoint["spec"].get("layouts", ())]
    old = checkpoint["trainer"]

    actor = {key: tensor.clone() for key, tensor in trainer.actor.state_dict().items()}
    critic = {key: tensor.clone() for key, tensor in trainer.critic.state_dict().items()}

    _seed_trunk(actor, old["actor"])
    _seed_trunk(critic, old["critic"])

    seeded = []
    for index, layout in enumerate(spec.layouts):
        if layout.name not in old_names:
            continue
        old_index = old_names.index(layout.name)
        seeded.append(layout.name)

        # Checkpoint keys use the checkpoint's own layout order.
        for network, old_network in ((actor, old["actor"]), (critic, old["critic"])):
            remapped = {
                f"adapters.{index}.weight": old_network[f"adapters.{old_index}.weight"],
                f"adapters.{index}.bias": old_network[f"adapters.{old_index}.bias"],
            }
            _seed_adapter(network, remapped, f"adapters.{index}")

        _seed_head(actor, {
            f"heads.{index}.weight": old["actor"][f"heads.{old_index}.weight"],
            f"heads.{index}.bias": old["actor"][f"heads.{old_index}.bias"],
        }, f"heads.{index}")

    trainer.actor.load_state_dict(actor)
    trainer.critic.load_state_dict(critic)
    trainer._sync_rollout()
    return seeded
