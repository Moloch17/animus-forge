"""Seeded evaluation and convergence detection.

An evaluation switches the sim to seeded episodes (protocol MODE): episode seed index i builds the same
characters and opponents every time, so two checkpoints -- or a checkpoint and a scripted baseline -- are
scored on exactly the same situations. Combat rolls (crits, misses, creature choices during the fight) stay
random, so the scores are averages over the seeds, not replays.

The score is the mean episode return over every agent of every seeded episode: the scenario's own reward summed
over each episode, so it measures what training optimises and is comparable between checkpoints of one scenario
(not between scenarios). Summaries also break it down by level band, by layout (class/role) and, for a stage that
mixes arenas, by arena, each with the standard error of its score: combat rolls make two evaluations of the same
networks differ, and the convergence test only counts an improvement that stands out from that noise.

Self-play arenas can be scored against a scripted opponent: with `opponents` the sim plays the other side of each
self-play episode with that policy (protocol MODE_FLAG_SCRIPTED_OPPONENTS), and the opponent seats' rows (episode
info opponent_seat) are left out of the result -- of the learner's evaluation and of the baseline's, which then is the
baseline against itself.
"""

from __future__ import annotations

import math
import time
from dataclasses import dataclass, field

import numpy as np

from . import protocol as p

LEVEL_BANDS = ((1, 20), (21, 40), (41, 60), (61, 80))

# How a character's talents were spent, by the episode info column "talent_plan" (SeatCharacter::TalentPlan).
# Scored as its own group so a run shows whether the policy plays a build it was not handed the recipe for.
TALENT_PLANS = ("standard", "noisy", "random")


@dataclass
class EvalResult:
    policy: str  # "learner" or the baseline's name
    returns: np.ndarray  # [n] episode returns, one per agent of each seeded episode, in (seed, agent) order
    infos: np.ndarray  # [n, K] episode info, same order
    info_names: tuple[str, ...]
    layouts: tuple[str, ...] = ()  # [n] each agent's layout name
    seeds: tuple[int, ...] = ()  # [n] the seed index of each row's episode, for the per-episode log
    arenas: tuple[str, ...] = ()  # the stage's arena names, indexed by the episode info column "arena"
    seconds: float = 0.0
    decisions: int = 0

    @property
    def episodes(self) -> int:
        return len(self.returns)

    @property
    def score(self) -> float:
        return float(self.returns.mean()) if len(self.returns) else float("nan")

    @property
    def stderr(self) -> float:
        return standard_error(self.returns)

    def episodes_log(self, columns: tuple[str, ...]) -> list[dict]:
        """One row per scored episode: its seed, layout, return and `columns` of its episode info.

        The summaries average these away, and an average cannot say whether a class/role is a little worse
        everywhere or fine except for a handful of episodes it never finishes -- which is what a per-layout gate
        actually turns on. Written to eval_episodes.jsonl by the training run.
        """
        present = [c for c in columns if c in self.info_names]
        rows = []
        for index in range(self.episodes):
            row = {
                "policy": self.policy,
                "seed": int(self.seeds[index]) if index < len(self.seeds) else -1,
                "layout": self.layouts[index] if index < len(self.layouts) else "",
                "return": float(self.returns[index]),
            }
            for name in present:
                row[name] = float(self.infos[index, self.info_names.index(name)])
            rows.append(row)
        return rows

    def column(self, name: str) -> np.ndarray | None:
        return self.infos[:, self.info_names.index(name)] if name in self.info_names else None

    def summary(self, columns: tuple[str, ...]) -> dict:
        """Score and means of `columns`: overall, per level band, per layout, per arena and per talent build."""
        present = [c for c in columns if c in self.info_names]

        def means(rows: np.ndarray) -> dict:
            out = {
                "episodes": int(rows.sum()),
                "score": float(self.returns[rows].mean()) if rows.any() else None,
                "stderr": standard_error(self.returns[rows]),
            }
            for name in present:
                values = self.column(name)[rows]
                out[name] = float(values.mean()) if len(values) else None
            return out

        everything = np.ones(self.episodes, dtype=bool)
        result = {"policy": self.policy, **means(everything), "bands": {}, "layouts": {}, "arenas": {}, "builds": {}}
        levels = self.column("level")
        if levels is not None:
            for low, high in LEVEL_BANDS:
                rows = (levels >= low) & (levels <= high)
                if rows.any():
                    result["bands"][f"{low}-{high}"] = means(rows)
        if len(set(self.layouts)) > 1:
            names = np.array(self.layouts)
            for layout in sorted(set(self.layouts)):
                result["layouts"][layout] = means(names == layout)
        arenas = self.column("arena")
        if len(self.arenas) > 1 and arenas is not None:
            for index, arena in enumerate(self.arenas):
                rows = arenas == index
                if rows.any():
                    result["arenas"][arena] = means(rows)
        plans = self.column("talent_plan")
        if plans is not None and len(set(plans.tolist())) > 1:
            for index, plan in enumerate(TALENT_PLANS):
                rows = plans == index
                if rows.any():
                    result["builds"][plan] = means(rows)
        return result


def layout_weights(summary: dict, baseline: dict | None, strength: float, max_ratio: float) -> dict[str, float]:
    """How often training episodes should draw each layout, from the gap to the baseline's score for that layout.

    A stage is gated on its weakest class/role, so an episode of a layout that trails its baseline is worth more
    than one of a layout that is already clear of it. The gaps are measured in their own standard deviations, so
    the weights do not depend on the size of the scenario's rewards, and the spread is capped: the heaviest layout
    draws at most `max_ratio` times the lightest, whatever the scores are. Weights average 1 (the even draw).
    """
    rows = summary.get("layouts", {})
    names = [name for name, row in rows.items() if row.get("score") is not None]
    if not names or strength <= 0.0 or max_ratio <= 1.0:
        return {name: 1.0 for name in rows}

    base = (baseline or {}).get("layouts", {})
    gaps = np.array([float(base.get(name, {}).get("score") or 0.0) - float(rows[name]["score"]) for name in names])
    spread = float(gaps.std())
    if spread <= 1e-9:
        return {name: 1.0 for name in names}

    weights = np.exp(strength * np.clip((gaps - gaps.mean()) / spread, -3.0, 3.0))
    # Cap the spread, then centre on 1 so the total number of episodes is unchanged.
    limit = math.sqrt(max_ratio)
    weights = np.clip(weights / float(np.exp(np.log(weights).mean())), 1.0 / limit, limit)
    weights /= float(weights.mean())
    return {name: float(weight) for name, weight in zip(names, weights)}


def standard_error(values: np.ndarray) -> float:
    """Standard error of the mean; 0 with fewer than two values."""
    return float(np.std(values, ddof=1) / math.sqrt(len(values))) if len(values) > 1 else 0.0


def run_evaluation(env, spec, choose_actions, episodes: int, seed: int, baseline: str = "",
                   max_decisions: int | None = None, opponents: str = "",
                   arenas: tuple[str, ...] = ()) -> tuple[EvalResult, p.Step]:
    """Run seeded episodes 0..episodes-1 and return their results and the fresh training STEP after them.

    choose_actions(step) -> [E, A] actions; ignored by the sim when `baseline` names a scripted policy. `opponents`
    names a scripted policy for the opponent seats of self-play episodes (the learner plays the rest, or `baseline`
    everything); their rows are left out. `arenas` are the stage's arena names, for the per-arena summary.
    """
    started = time.perf_counter()
    envs, agents = spec.num_envs, spec.agents_per_env
    names = [layout.name for layout in spec.layouts]

    if max_decisions is None:
        per_episode = max(1, spec.episode_seconds * 1000 // max(1, spec.decision_ms))
        # Seeds go out as envs reset, so the last one can start up to one episode after the others.
        max_decisions = per_episode * (-(-episodes // envs) + 2)

    if baseline:
        step = env.set_mode(True, seed, episodes, baseline)
    else:
        step = env.set_mode(True, seed, episodes, opponents, opponents_only=bool(opponents))
    running = np.zeros((envs, agents), dtype=np.float64)
    finished: dict[int, list[tuple[float, np.ndarray, str]]] = {}
    info_names = list(spec.episode_info_names)
    # A party seat left empty for an episode reports present = 0: it is not an episode of any class/role.
    present = info_names.index("present") if "present" in info_names else None
    # Against a scripted opponent its seats are not the learner's (nor, for the baseline, the seat being scored).
    opponent_seat = info_names.index("opponent_seat") if opponents and "opponent_seat" in info_names else None
    decisions = 0

    while len(finished) < episodes and decisions < max_decisions:
        actions = np.zeros((envs, agents), dtype=np.int32) if baseline else choose_actions(step)
        # The episode's layouts: after a done, the next STEP already carries the new episode's.
        layout = step.layout
        step = env.step(actions)
        decisions += 1

        running += step.reward
        for e in np.flatnonzero(step.done):
            index = int(step.episode_seed[e])
            if index != p.NO_EPISODE_SEED and index < episodes and index not in finished:
                finished[index] = [
                    (float(running[e, a]), step.episode_info[e, a].copy(), names[int(layout[e, a])])
                    for a in range(agents)
                    if (present is None or step.episode_info[e, a, present] > 0.0)
                    and (opponent_seat is None or step.episode_info[e, a, opponent_seat] <= 0.0)
                ]
            running[e] = 0.0

    if len(finished) < episodes:
        print(f"Evaluation stopped after {decisions} decisions with {len(finished)} of {episodes} episodes", flush=True)

    rows = [row for index in sorted(finished) for row in finished[index]]
    seeds = [index for index in sorted(finished) for _ in finished[index]]
    result = EvalResult(
        policy=baseline or "learner",
        returns=np.array([row[0] for row in rows], dtype=np.float64),
        infos=np.array([row[1] for row in rows], dtype=np.float32).reshape(len(rows), spec.episode_info_dim),
        info_names=tuple(spec.episode_info_names),
        layouts=tuple(row[2] for row in rows),
        seeds=tuple(seeds),
        arenas=tuple(arenas),
        seconds=time.perf_counter() - started,
        decisions=decisions,
    )

    training_step = env.set_mode(False)
    return result, training_step


@dataclass
class ConvergenceTracker:
    """Best evaluation score so far, and whether the score has stopped improving.

    An evaluation is a new best only if it beats the best by the margin: the larger of min_improvement_abs,
    min_improvement x |best| and z standard errors of the difference between the two scores, so a lucky evaluation
    within the noise does not count. The run has converged once `patience` evaluations in a row set no new best
    and the trend of the last `window` scores, projected `patience` evaluations ahead, would not reach the margin
    either (a slow climb hidden by the noise is still a climb).

    A restart (animus.stage) starts a new segment: the counters and the trend start over, the best is kept.
    """

    patience: int = 0
    window: int = 4
    z: float = 2.0
    min_improvement: float = 0.02
    min_improvement_abs: float = 0.01
    best: float | None = None
    best_stderr: float = 0.0
    best_env_steps: int = 0
    evals_since_best: int = 0
    last_margin: float = 0.0  # the margin the latest evaluation had to beat
    segment_index: int = 0  # history index where the current segment starts
    segment_env_steps: int = 0
    history: list[tuple[int, float, float]] = field(default_factory=list)  # (env steps, score, stderr)

    def margin(self, stderr: float = 0.0) -> float:
        if self.best is None:
            return 0.0
        noise = self.z * math.sqrt(stderr ** 2 + self.best_stderr ** 2)
        return max(self.min_improvement_abs, self.min_improvement * abs(self.best), noise)

    def observe(self, score: float, env_steps: int, stderr: float = 0.0) -> bool:
        """Record an evaluation; True if it is a new best."""
        self.history.append((env_steps, score, stderr))
        self.last_margin = self.margin(stderr)
        if self.best is None or score > self.best + self.last_margin:
            self.best = score
            self.best_stderr = stderr
            self.best_env_steps = env_steps
            self.evals_since_best = 0
            return True
        self.evals_since_best += 1
        return False

    def projected_gain(self) -> float | None:
        """Score gain over the next `patience` evaluations on the current segment's trend; None below 3 points."""
        points = self.history[self.segment_index:][-max(3, self.window):]
        if len(points) < 3:
            return None
        steps = np.array([point[0] for point in points], dtype=np.float64)
        scores = np.array([point[1] for point in points], dtype=np.float64)
        if np.ptp(steps) == 0:
            return None
        slope = np.polyfit(steps, scores, 1)[0]
        spacing = np.ptp(steps) / (len(points) - 1)
        return float(slope * spacing * self.patience)

    def converged(self, env_steps: int, min_env_steps: int = 0) -> bool:
        if self.patience <= 0 or self.evals_since_best < self.patience:
            return False
        if env_steps - self.segment_env_steps < min_env_steps:
            return False
        gain = self.projected_gain()
        if gain is None:
            return True
        recent = self.history[self.segment_index:][-max(3, self.window):]
        stderr = float(np.mean([point[2] for point in recent]))
        return gain <= self.margin(stderr)

    def reset_segment(self, env_steps: int) -> None:
        """Start a new segment (after a restart): counters and trend start over, the best score is kept."""
        self.segment_index = len(self.history)
        self.segment_env_steps = env_steps
        self.evals_since_best = 0

    def state_dict(self) -> dict:
        return {
            "best": self.best,
            "best_stderr": self.best_stderr,
            "best_env_steps": self.best_env_steps,
            "evals_since_best": self.evals_since_best,
            "segment_index": self.segment_index,
            "segment_env_steps": self.segment_env_steps,
            "history": list(self.history),
        }

    def load_state_dict(self, state: dict | None) -> None:
        if not state:
            return
        self.best = state.get("best")
        self.best_stderr = state.get("best_stderr", 0.0)
        self.best_env_steps = state.get("best_env_steps", 0)
        self.evals_since_best = state.get("evals_since_best", 0)
        self.segment_index = state.get("segment_index", 0)
        self.segment_env_steps = state.get("segment_env_steps", 0)
        self.history = [tuple(h) for h in state.get("history", [])]


def format_summary(summary: dict, baseline: dict | None, columns: tuple[str, ...]) -> str:
    """Multi-line table: overall, per level band, layout, arena and talent build, learner next to baseline."""

    def cell(row: dict | None, name: str) -> str:
        value = None if row is None else row.get(name)
        return f"{value:10.2f}" if isinstance(value, (int, float)) else f"{'-':>10}"

    names = ["score", *[c for c in columns if c in summary]]
    rows = [("all", summary, baseline)]
    for group in ("bands", "layouts", "arenas", "builds"):
        for key, row in summary.get(group, {}).items():
            rows.append((key, row, (baseline or {}).get(group, {}).get(key)))

    width = max(7, *(len(key) for key, _, _ in rows))
    # "rows": one per agent of each seeded episode (a party episode is up to four), not episodes.
    lines = [f"  {'group':>{width}} {'rows':>4}  " + "  ".join(f"{n[:21]:>21}" for n in names)]
    for key, row, base in rows:
        cells = "  ".join(f"{cell(row, n)}/{cell(base, n).strip():>10}" for n in names)
        lines.append(f"  {key:>{width}} {row['episodes']:>4}  {cells}")
    return "\n".join(lines)
