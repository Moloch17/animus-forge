"""Stage targets: whether an evaluation summary is good enough to move on to the next curriculum stage.

Scores are only comparable within one scenario, so the score gates are relative to the scripted baseline scored on
the same seeds: "score >= baseline + ratio x |baseline|" reads as "ratio better than the baseline" whatever the sign
of the scenario's reward. The overall gate keeps the average up; the per-layout floor keeps a class/role from
hiding behind it, because the next stage seeds every layout from these networks. Score gates allow for evaluation
noise (target.noise_z standard errors of the difference), because a class/role holds only its share of the seeded
episodes: without it a layout that is genuinely level with the baseline fails about half the time. Metric gates check
episode info
means directly (killed, died, ...), which reward shaping cannot game. A stage that mixes arenas can gate each arena
on its own episodes (target.arenas), so one situation cannot hide behind the others either.
"""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass, field

from .config import TargetConfig, TrainConfig


@dataclass
class GateReport:
    passed: bool = True
    failures: list[str] = field(default_factory=list)
    checks: list[dict] = field(default_factory=list)  # {gate, value, required, passed}
    skipped: list[str] = field(default_factory=list)

    def check(self, gate: str, value: float, required: float, passed: bool) -> None:
        self.checks.append({"gate": gate, "value": value, "required": required, "passed": passed})
        if not passed:
            self.passed = False
            self.failures.append(f"{gate} {value:.4g} (needs {required:.4g})")

    def fail(self, reason: str) -> None:
        self.passed = False
        self.failures.append(reason)

    def to_dict(self) -> dict:
        return asdict(self)


def required_score(baseline: float, ratio: float) -> float:
    return baseline + ratio * abs(baseline)


def noise_allowance(row: dict, base: dict, z: float) -> float:
    """How far below the required score still counts as passing: `z` standard errors of the difference between the
    two scores. Both are means over a sample of seeded episodes, so both carry noise."""
    if z <= 0.0:
        return 0.0
    stderr = float(row.get("stderr") or 0.0)
    base_stderr = float(base.get("stderr") or 0.0)
    return z * math.sqrt(stderr ** 2 + base_stderr ** 2)


def _check_score(report: GateReport, gate: str, row: dict, base: dict, ratio: float, z: float) -> None:
    """A score gate: the required score, less what evaluation noise can explain."""
    required = required_score(base["score"], ratio)
    report.check(gate, row["score"], required, row["score"] >= required - noise_allowance(row, base, z))


def check_gates(summary: dict | None, baseline: dict | None, target: TargetConfig) -> GateReport:
    """Check `summary` (EvalResult.summary) against the target; `baseline` is the baseline's summary, same seeds."""
    report = GateReport()
    if summary is None or summary.get("score") is None:
        report.fail("no evaluation to judge")
        return report

    if target.min_over_baseline is not None or target.min_layout_over_baseline is not None:
        if baseline is None or baseline.get("score") is None:
            report.fail("no baseline score to compare with")
            return report

    if target.min_over_baseline is not None:
        _check_score(report, "score", summary, baseline, target.min_over_baseline, target.noise_z)

    if target.min_layout_over_baseline is not None:
        for name, row in summary.get("layouts", {}).items():
            base = baseline.get("layouts", {}).get(name)
            if row.get("score") is None or base is None or base.get("score") is None:
                report.skipped.append(f"{name}: no baseline")
            elif row["episodes"] < target.min_layout_episodes:
                report.skipped.append(f"{name}: {row['episodes']} episodes")
            else:
                _check_score(report, f"{name} score", row, base, target.min_layout_over_baseline, target.noise_z)

    _check_metrics(report, summary, target.metrics, "")

    for arena, gates in target.arenas.items():
        row = summary.get("arenas", {}).get(arena)
        if row is None or row.get("score") is None:
            report.fail(f"arena {arena}: not in the evaluation summary")
            continue
        if row["episodes"] < target.min_arena_episodes:
            report.skipped.append(f"arena {arena}: {row['episodes']} episodes")
            continue

        ratio = gates.get("min_over_baseline")
        if ratio is not None:
            base = (baseline or {}).get("arenas", {}).get(arena)
            if base is None or base.get("score") is None:
                report.fail(f"arena {arena}: no baseline score to compare with")
            else:
                _check_score(report, f"arena {arena} score", row, base, ratio, target.noise_z)

        _check_metrics(report, row, gates.get("metrics", {}), f"arena {arena} ")

    return report


def _check_metrics(report: GateReport, row: dict, metrics: dict, prefix: str) -> None:
    for name, bounds in metrics.items():
        value = row.get(name)
        if value is None:
            report.fail(f"{prefix}{name}: not in the evaluation summary")
            continue
        if "min" in bounds:
            report.check(f"{prefix}{name} (min)", value, bounds["min"], value >= bounds["min"])
        if "max" in bounds:
            report.check(f"{prefix}{name} (max)", value, bounds["max"], value <= bounds["max"])


def _metric_errors(prefix: str, metrics, info_names) -> list[str]:
    if not isinstance(metrics, dict):
        return [f"{prefix}: expected {{name: {{min/max: number}}}}, got {metrics!r}"]
    errors = []
    for name, bounds in metrics.items():
        if name not in info_names:
            errors.append(f"{prefix}.{name}: the scenario has no such episode info ({', '.join(info_names)})")
        if not isinstance(bounds, dict) or not bounds or set(bounds) - {"min", "max"} \
                or not all(isinstance(v, (int, float)) for v in bounds.values()):
            errors.append(f"{prefix}.{name}: expected {{min: number}} and/or {{max: number}}, got {bounds!r}")
    return errors


def validate_target(config: TrainConfig, info_names: tuple[str, ...] | list[str],
                    arena_names: tuple[str, ...] | list[str] | None = None) -> None:
    """Fail at startup, not hours later at the end of the stage, when the target cannot be checked. `arena_names`
    are the stage's arenas (stage.json); None skips checking arena names."""
    target = config.target
    errors = []
    if target.enabled and config.eval.every_env_steps <= 0:
        errors.append("target gates need eval.every_env_steps")
    if (target.min_over_baseline is not None or target.min_layout_over_baseline is not None
            or target.arena_needs_baseline()) and not config.eval.baseline:
        errors.append("target.min_over_baseline / min_layout_over_baseline need eval.baseline")
    if config.eval.opponent_baseline and not config.eval.baseline:
        errors.append("eval.opponent_baseline needs eval.baseline")
    errors += _metric_errors("target.metrics", target.metrics, info_names)
    for arena, gates in target.arenas.items():
        prefix = f"target.arenas.{arena}"
        if arena_names is not None and arena not in arena_names:
            errors.append(f"{prefix}: the stage has no such arena ({', '.join(arena_names) or 'none listed'})")
        if not isinstance(gates, dict) or set(gates) - {"min_over_baseline", "metrics"}:
            errors.append(f"{prefix}: expected min_over_baseline and/or metrics, got {gates!r}")
            continue
        if gates.get("min_over_baseline") is not None and not isinstance(gates["min_over_baseline"], (int, float)):
            errors.append(f"{prefix}.min_over_baseline: expected a number")
        errors += _metric_errors(f"{prefix}.metrics", gates.get("metrics", {}), info_names)
    if target.min_arena_episodes < 0:
        errors.append("target.min_arena_episodes must be >= 0")
    if target.noise_z < 0:
        errors.append("target.noise_z must be >= 0")
    if target.confirm_episodes < 0:
        errors.append("target.confirm_episodes must be >= 0")
    if config.restarts.max_restarts < 0:
        errors.append("restarts.max_restarts must be >= 0")
    if errors:
        raise ValueError("invalid stage target: " + "; ".join(errors))
