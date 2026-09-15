"""Kickstarting a merge stage from the stages it joins (distill: in the config).

A merge stage mixes arenas that different parents already play: the extended stage's arenas and each merged stage's.
Seeding (animus.bootstrap) copies the trunk from one parent only, so on the other parents' arenas the seeded policy
starts off worse than those parents. Distillation pulls it back: on the decisions of an arena that has a teacher, the
policy loss gains coef x KL(teacher || policy), with coef decaying over training so PPO takes over (Schmitt et al.,
"Kickstarting Deep Reinforcement Learning", 2018).

A teacher is a parent's frozen actor. Its layouts, blocks and actions are matched to the stage's by name through both
stage.json files (block spans): the teacher sees the stage's observation columns of the blocks it has (the others
are zero), and the KL is over the actions both have and the stage's mask allows, each distribution renormalised
over that set. Each decision's arena comes from the critic state's arena one-hot (stage.json ``state``).
"""

from __future__ import annotations

from dataclasses import dataclass

import torch

from .mappo.networks import MASKED_LOGIT, LayoutActor
from .stages import Span, arena_names, arena_state_span, block_spans


@dataclass
class LayoutMap:
    """How one of the stage's layouts reads and writes a teacher layout."""

    teacher_layout: int
    obs_student: torch.Tensor  # stage observation columns ...
    obs_teacher: torch.Tensor  # ... and where they go in the teacher's
    actions_student: torch.Tensor  # stage actions ...
    actions_teacher: torch.Tensor  # ... and the teacher's same actions


@dataclass
class Teacher:
    name: str  # the checkpoint's scenario
    actor: LayoutActor
    obs_dim: int  # the teacher's padded observation width
    num_actions: int  # the teacher's padded action count
    layouts: dict[int, LayoutMap]  # the stage's layout index -> its map; layouts the teacher lacks are absent


def _index_pairs(student: dict[str, tuple[Span, Span]] | None, teacher: dict[str, tuple[Span, Span]] | None,
                 student_dims: tuple[int, int], teacher_dims: tuple[int, int]):
    """(obs student, obs teacher, actions student, actions teacher) index lists for the blocks both have with equal
    sizes; without block spans, the common prefix."""
    if student is None or teacher is None:
        obs = list(range(min(student_dims[0], teacher_dims[0])))
        actions = list(range(min(student_dims[1], teacher_dims[1])))
        return obs, obs, actions, actions

    obs_s, obs_t, act_s, act_t = [], [], [], []
    for block, ((s_obs, s_count), (s_act, s_act_count)) in student.items():
        if block not in teacher:
            continue
        (t_obs, t_count), (t_act, t_act_count) = teacher[block]
        if (s_count, s_act_count) != (t_count, t_act_count):
            continue
        obs_s += range(s_obs, s_obs + s_count)
        obs_t += range(t_obs, t_obs + t_count)
        act_s += range(s_act, s_act + s_act_count)
        act_t += range(t_act, t_act + t_act_count)
    return obs_s, obs_t, act_s, act_t


def build_teacher(checkpoint: dict, spec, stage: dict | None, device: torch.device) -> Teacher:
    """A frozen actor from `checkpoint`, mapped onto the stage's layouts (spec.layouts, stage.json `stage`)."""
    t_spec = checkpoint["spec"]
    t_layouts = [(entry["obs_dim"], entry["num_actions"]) for entry in t_spec["layouts"]]
    hidden = list(checkpoint["config"]["mappo"]["hidden"])
    actor = LayoutActor(t_layouts, hidden)
    actor.load_state_dict(checkpoint["trainer"]["actor"])
    actor.to(device).eval()
    for param in actor.parameters():
        param.requires_grad_(False)

    t_names = [entry["name"] for entry in t_spec["layouts"]]
    t_stage = checkpoint.get("stage")
    layouts = {}
    for index, layout in enumerate(spec.layouts):
        if layout.name not in t_names:
            continue
        t_index = t_names.index(layout.name)
        obs_s, obs_t, act_s, act_t = _index_pairs(
            block_spans(stage, layout.name), block_spans(t_stage, layout.name),
            (layout.obs_dim, layout.num_actions), t_layouts[t_index])
        if not act_s:
            continue

        def tensor(values):
            return torch.as_tensor(values, dtype=torch.long, device=device)

        layouts[index] = LayoutMap(t_index, tensor(obs_s), tensor(obs_t), tensor(act_s), tensor(act_t))

    return Teacher(
        name=t_spec.get("scenario", "?"),
        actor=actor,
        obs_dim=max(obs for obs, _ in t_layouts),
        num_actions=max(actions for _, actions in t_layouts),
        layouts=layouts,
    )


def auto_teachers(stage: dict | None, parents: list[dict]) -> dict[str, dict]:
    """distill.teachers: auto -- every arena of the stage goes to the first parent checkpoint (in order) whose own
    stage.json has an arena of that name."""
    chosen = {}
    for arena in arena_names(stage):
        for parent in parents:
            if arena in arena_names(parent.get("stage")):
                chosen[arena] = parent
                break
    return chosen


class Distiller:
    """The auxiliary loss for MappoTrainer.update: KL(teacher || policy) on the decisions of taught arenas."""

    def __init__(self, stage: dict | None, teachers: dict[str, Teacher]):
        span = arena_state_span(stage)
        if span is None:
            raise ValueError("distillation needs the stage.json arena state span (a sim that writes stage.json 2)")
        names = arena_names(stage)
        unknown = sorted(set(teachers) - set(names))
        if unknown:
            raise ValueError(f"distill.teachers: the stage has no arena {', '.join(unknown)} ({', '.join(names)})")
        self.arena_first, self.arena_count = span
        self.teachers = {names.index(arena): teacher for arena, teacher in teachers.items()}
        self.coef = 0.0

    def arenas(self, state: torch.Tensor) -> torch.Tensor:
        """Each row's arena index from the critic state one-hot; -1 when none is set."""
        onehot = state[..., self.arena_first : self.arena_first + self.arena_count]
        value, index = onehot.max(dim=-1)
        return torch.where(value > 0.5, index, torch.full_like(index, -1))

    def kl(self, obs: torch.Tensor, state: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
           log_probs: torch.Tensor) -> tuple[torch.Tensor, int]:
        """Summed KL(teacher || policy) over the taught rows, and how many rows that is. `log_probs` [n, N] are the
        policy's normalised log-probabilities (Categorical.logits) for the rows."""
        arena = self.arenas(state)
        total = log_probs.new_zeros(())
        rows_taught = 0

        for arena_index, teacher in self.teachers.items():
            in_arena = arena == arena_index
            if not in_arena.any():
                continue
            for layout_index in torch.unique(layout[in_arena]).tolist():
                mapping = teacher.layouts.get(int(layout_index))
                if mapping is None:
                    continue
                rows = torch.nonzero(in_arena & (layout == layout_index), as_tuple=True)[0]

                allowed = mask[rows][:, mapping.actions_student].bool()
                taught = allowed.any(dim=-1)
                if not taught.any():
                    continue

                with torch.no_grad():
                    t_obs = obs.new_zeros(len(rows), teacher.obs_dim)
                    t_obs[:, mapping.obs_teacher] = obs[rows][:, mapping.obs_student]
                    t_mask = torch.zeros(len(rows), teacher.num_actions, dtype=torch.bool, device=obs.device)
                    t_mask[:, mapping.actions_teacher] = allowed
                    t_layout = torch.full((len(rows),), mapping.teacher_layout, dtype=torch.long, device=obs.device)
                    t_logits = teacher.actor(t_obs, t_layout, t_mask).logits[:, mapping.actions_teacher]
                    t_logp = torch.log_softmax(t_logits.masked_fill(~allowed, MASKED_LOGIT), dim=-1)

                s_logits = log_probs[rows][:, mapping.actions_student]
                s_logp = torch.log_softmax(s_logits.masked_fill(~allowed, MASKED_LOGIT), dim=-1)
                kl = (t_logp.exp() * (t_logp - s_logp)).masked_fill(~allowed, 0.0).sum(dim=-1)
                total = total + kl[taught].sum()
                rows_taught += int(taught.sum())

        return total, rows_taught

    def __call__(self, data: dict, idx: torch.Tensor, dist) -> tuple[torch.Tensor, dict[str, float]] | None:
        """MappoTrainer.update's auxiliary hook: coef x mean KL on the minibatch's taught rows."""
        if self.coef < 1e-4:
            return None
        total, rows = self.kl(data["obs"][idx], data["state"][idx], data["layout"][idx], data["mask"][idx],
                              dist.logits)
        if rows == 0:
            return None
        mean = total / rows
        return self.coef * mean, {"distill_kl": float(mean.detach()), "distill_rows": float(rows)}
