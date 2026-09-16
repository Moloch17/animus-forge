"""When a curriculum stage is finished, and what to do when it gets stuck.

The queue (AnimusForge.Queue) moves on to the next stage when the learner exits cleanly, so the learner decides:

- The stage is **done learning** when the evaluation score has converged (animus.evaluation.ConvergenceTracker).
- It is **good enough** when the best networks pass the stage target (animus.gates), checked on the evaluation
  that set the best and then again on held-out seeds (target.confirm_episodes).
- Converged and good enough: the learner exits 0 and the queue moves on.
- Converged below the target is a local optimum: the networks restart from best.pt with more exploration (a raised
  entropy bonus that decays back, fresh optimizers, optionally shrink and perturb) and the convergence test starts
  over, up to restarts.max_restarts times. After that, or when total_env_steps runs out below the target, the
  learner exits with EXIT_BELOW_TARGET and the queue halts on this stage.

With no target set, a converged stage moves on as before. The controller only decides; animus.train carries it out.
"""

from __future__ import annotations

import math

from collections.abc import Callable
from dataclasses import dataclass

from .config import TrainConfig
from .evaluation import ConvergenceTracker
from .gates import GateReport, check_gates

EXIT_BELOW_TARGET = 3

CONTINUE, ADVANCE, RESTART, HALT = "continue", "advance", "restart", "halt"


@dataclass
class Outcome:
    action: str  # CONTINUE, ADVANCE, RESTART or HALT
    reason: str = ""  # finished.json reason for ADVANCE and HALT
    gates: GateReport | None = None  # the report that decided it
    stage: str = ""  # which evaluation the report is on: "best" or "confirm"


# Scores the best networks on the held-out seeds: returns (learner summary, baseline summary or None).
Confirm = Callable[[], tuple[dict, dict | None]]


class StageController:
    def __init__(self, config: TrainConfig):
        self.config = config
        c = config.convergence
        evaluating = config.eval.every_env_steps > 0
        self.tracker = ConvergenceTracker(
            patience=c.patience if evaluating else 0,
            window=c.window,
            z=c.z,
            min_improvement=c.min_improvement,
            min_improvement_abs=c.min_improvement_abs,
        )
        self.restarts = 0
        self.restart_env_steps: int | None = None
        self.best_summary: dict | None = None
        self.baseline_summary: dict | None = None
        self.last_outcome: Outcome | None = None
        # What the entropy floor multiplies mappo.entropy_coef by; 1 until the floor has reason to raise it.
        self.entropy_scale = 1.0

    def observe(self, summary: dict, env_steps: int) -> bool:
        """Record a learner evaluation; True if its networks are the new best (save them to best.pt)."""
        improved = self.tracker.observe(summary["score"], env_steps, summary.get("stderr", 0.0))
        if improved:
            self.best_summary = summary
        return improved

    def entropy_coef(self, env_steps: int) -> float:
        base = self.config.mappo.entropy_coef * self.entropy_scale
        r = self.config.restarts
        if self.restart_env_steps is None or r.entropy_boost == 1.0:
            return base
        elapsed = max(0, env_steps - self.restart_env_steps)
        decay = 0.5 ** (elapsed / r.entropy_half_life_env_steps) if r.entropy_half_life_env_steps > 0 else 0.0
        return base * (1.0 + (r.entropy_boost - 1.0) * decay)

    def observe_entropy(self, entropy: float, allowed_actions: float) -> None:
        """Move the entropy floor after an update: `entropy` is the policy's, over `allowed_actions` legal ones.

        The ceiling a masked policy can reach is ln(allowed actions), so that -- not the padded action count --
        is what the target is a fraction of. The scale only ever sits between 1 and max_boost: below the target
        it climbs, above it falls back to the configured coefficient, so a policy that is converging on its own
        is never held open.
        """
        floor = self.config.entropy_floor
        if floor.fraction <= 0.0 or allowed_actions <= 1.0:
            return

        target = floor.fraction * math.log(allowed_actions)
        wanted = min(floor.max_boost, self.entropy_scale * 1.5) if entropy < target else 1.0
        self.entropy_scale += floor.rate * (wanted - self.entropy_scale)
        self.entropy_scale = min(max(self.entropy_scale, 1.0), max(1.0, floor.max_boost))

    def after_eval(self, env_steps: int, confirm: Confirm) -> Outcome:
        """Call after each training evaluation."""
        if not self.tracker.converged(env_steps, self.config.convergence.min_env_steps):
            return self._decide(Outcome(CONTINUE))
        return self._decide(self._judge(confirm, at_budget=False))

    def at_budget(self, confirm: Confirm) -> Outcome:
        """Call once total_env_steps is reached (after the final evaluation)."""
        return self._decide(self._judge(confirm, at_budget=True))

    def record_restart(self, env_steps: int) -> None:
        """Call once the networks have been restarted from best.pt."""
        self.restarts += 1
        self.restart_env_steps = env_steps
        self.tracker.reset_segment(env_steps)

    def state_dict(self) -> dict:
        """What a resumed run needs to carry on deciding (the convergence tracker is saved on its own)."""
        return {
            "restarts": self.restarts,
            "restart_env_steps": self.restart_env_steps,
            "best_summary": self.best_summary,
            "baseline_summary": self.baseline_summary,
        }

    def load_state_dict(self, state: dict | None) -> None:
        if not state:
            return
        self.restarts = state.get("restarts", 0)
        self.restart_env_steps = state.get("restart_env_steps")
        self.best_summary = state.get("best_summary")
        # The target's baseline gates need it even when the resumed run judges the stage before evaluating again.
        self.baseline_summary = state.get("baseline_summary")

    def _decide(self, outcome: Outcome) -> Outcome:
        self.last_outcome = outcome
        return outcome

    def _judge(self, confirm: Confirm, at_budget: bool) -> Outcome:
        done = "total_env_steps" if at_budget else "converged"
        target = self.config.target
        if not target.enabled:
            return Outcome(ADVANCE, done)

        report, stage = check_gates(self.best_summary, self.baseline_summary, target), "best"
        if report.passed and target.confirm_episodes > 0:
            summary, baseline = confirm()
            report, stage = check_gates(summary, baseline, target), "confirm"
        if report.passed:
            return Outcome(ADVANCE, done, report, stage)

        if not at_budget and self.restarts < self.config.restarts.max_restarts:
            return Outcome(RESTART, "below_target", report, stage)
        return Outcome(HALT, "budget_below_target" if at_budget else "below_target", report, stage)
