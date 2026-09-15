"""Seed a curriculum stage's networks from the stage it extends.

Networks are layout-aware (animus.mappo.networks): a per-layout input adapter, a shared trunk and (actor) a
per-layout action head. Layouts are matched by name (the class/role). A stage keeps some of its base's blocks, may
drop others and adds its own (the curriculum is a tree), so a layout is seeded block by block:

- Input adapters (actor and critic): each kept block's feature columns move to where the block sits now; new blocks'
  columns start at zero, so the seeded policy initially ignores them; dropped blocks' columns are left behind.
- Actor heads: each kept block's action rows move the same way; new actions keep their small initial weights.
- Trunk: copied (the hidden sizes must match).
- A layout the checkpoint does not have keeps its fresh adapter and head, and still gets the copied trunk.
- Critic state encoder and value head: kept freshly initialised, and the value normaliser is not copied, because
  the later stage's global state and reward differ.

Block positions come from the stages' stage.json (``layouts``), which every checkpoint carries. A checkpoint or stage
without them (older runs, standalone scenarios) is seeded as a prefix: the earlier layout's columns and rows first.
"""

from __future__ import annotations

import torch

from .stages import Span, block_spans


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


def _common_blocks(old: dict[str, tuple[Span, Span]], new: dict[str, tuple[Span, Span]], name: str):
    """(old spans, new spans) of every block both layouts have, sizes checked."""
    common = []
    for block, (new_obs, new_actions) in new.items():
        if block not in old:
            continue
        old_obs, old_actions = old[block]
        if old_obs[1] != new_obs[1] or old_actions[1] != new_actions[1]:
            raise ValueError(f"{name}: block {block} is {old_obs[1]} features and {old_actions[1]} actions in the "
                             f"checkpoint, {new_obs[1]} and {new_actions[1]} now")
        common.append(((old_obs, old_actions), (new_obs, new_actions)))
    return common


def _seed_adapter_blocks(new: dict, old: dict, prefix: str, common) -> None:
    new_w, old_w = new[f"{prefix}.weight"], old[f"{prefix}.weight"]
    if new_w.shape[0] != old_w.shape[0]:
        raise ValueError(f"{prefix}: width {old_w.shape[0]} in the checkpoint, {new_w.shape[0]} now")
    new_w.zero_()
    for ((old_first, count), _), ((new_first, _), _) in common:
        new_w[:, new_first : new_first + count] = old_w[:, old_first : old_first + count]
    new[f"{prefix}.bias"].copy_(old[f"{prefix}.bias"])


def _seed_head_blocks(new: dict, old: dict, prefix: str, common) -> None:
    new_w, old_w = new[f"{prefix}.weight"], old[f"{prefix}.weight"]
    if new_w.shape[1] != old_w.shape[1]:
        raise ValueError(f"{prefix}: {tuple(old_w.shape)} does not fit in {tuple(new_w.shape)}")
    new_b, old_b = new[f"{prefix}.bias"], old[f"{prefix}.bias"]
    for (_, (old_first, count)), (_, (new_first, _)) in common:
        new_w[new_first : new_first + count] = old_w[old_first : old_first + count]
        new_b[new_first : new_first + count] = old_b[old_first : old_first + count]


def _seed_trunk(new: dict, old: dict) -> None:
    for key, tensor in new.items():
        if key.startswith("trunk."):
            if key not in old or old[key].shape != tensor.shape:
                raise ValueError(f"{key}: the trunk in the checkpoint does not match (hidden sizes must be equal)")
            tensor.copy_(old[key])


def seed_trainer(trainer, checkpoint: dict, spec, stage: dict | None = None) -> list[str]:
    """Seed a fresh MappoTrainer for `spec` (whose stage.json is `stage`) from an earlier stage's checkpoint; returns
    the layouts seeded."""
    old_names = [layout["name"] for layout in checkpoint["spec"].get("layouts", ())]
    old_stage = checkpoint.get("stage")
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
        adapters = [(network, {
            f"adapters.{index}.weight": old_network[f"adapters.{old_index}.weight"],
            f"adapters.{index}.bias": old_network[f"adapters.{old_index}.bias"],
        }) for network, old_network in ((actor, old["actor"]), (critic, old["critic"]))]
        head = {
            f"heads.{index}.weight": old["actor"][f"heads.{old_index}.weight"],
            f"heads.{index}.bias": old["actor"][f"heads.{old_index}.bias"],
        }

        old_blocks = block_spans(old_stage, layout.name)
        new_blocks = block_spans(stage, layout.name)
        if old_blocks is not None and new_blocks is not None:
            common = _common_blocks(old_blocks, new_blocks, layout.name)
            for network, remapped in adapters:
                _seed_adapter_blocks(network, remapped, f"adapters.{index}", common)
            _seed_head_blocks(actor, head, f"heads.{index}", common)
        else:
            for network, remapped in adapters:
                _seed_adapter(network, remapped, f"adapters.{index}")
            _seed_head(actor, head, f"heads.{index}")

    trainer.actor.load_state_dict(actor)
    trainer.critic.load_state_dict(critic)
    trainer._sync_rollout()
    return seeded
