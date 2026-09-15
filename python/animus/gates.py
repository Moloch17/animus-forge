"""Stage targets: whether an evaluation summary is good enough to move on to the next curriculum stage.

Scores are only comparable within one scenario, so the score gates are relative to the scripted baseline scored on
the same seeds: "score >= baseline + ratio x |baseline|" reads as "ratio better than the baseline" whatever the sign
of the scenario's reward. The overall gate keeps the average up; the per-layout floor keeps a class/role from
hiding behind it, because the next stage seeds every layout from these networks. Metric gates check episode info
means directly (killed, died, ...), which reward shaping cannot game.
"""

from __future__ import annotations

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
        required = required_score(baseline["score"], target.min_over_baseline)
        report.check("score", summary["score"], required, summary["score"] >= required)

    if target.min_layout_over_baseline is not None:
        for name, row in summary.get("layouts", {}).items():
            base = baseline.get("layouts", {}).get(name)
            if row.get("score") is None or base is None or base.get("score") is None:
                report.skipped.append(f"{name}: no baseline")
            elif row["episodes"] < target.min_layout_episodes:
                report.skipped.append(f"{name}: {row['episodes']} episodes")
            else:
                required = required_score(base["score"], target.min_layout_over_baseline)
                report.check(f"{name} score", row["score"], required, row["score"] >= required)

    for name, bounds in target.metrics.items():
        value = summary.get(name)
        if value is None:
            report.fail(f"{name}: not in the evaluation summary")
            continue
        if "min" in bounds:
            report.check(f"{name} (min)", value, bounds["min"], value >= bounds["min"])
        if "max" in bounds:
            report.check(f"{name} (max)", value, bounds["max"], value <= bounds["max"])

    return report


def validate_target(config: TrainConfig, info_names: tuple[str, ...] | list[str]) -> None:
    """Fail at startup, not hours later at the end of the stage, when the target cannot be checked."""
    target = config.target
    errors = []
    if target.enabled and config.eval.every_env_steps <= 0:
        errors.append("target gates need eval.every_env_steps")
    if (target.min_over_baseline is not None or target.min_layout_over_baseline is not None) \
            and not config.eval.baseline:
        errors.append("target.min_over_baseline / min_layout_over_baseline need eval.baseline")
    for name, bounds in target.metrics.items():
        if name not in info_names:
            errors.append(f"target.metrics.{name}: the scenario has no such episode info ({', '.join(info_names)})")
        if not isinstance(bounds, dict) or not bounds or set(bounds) - {"min", "max"} \
                or not all(isinstance(v, (int, float)) for v in bounds.values()):
            errors.append(f"target.metrics.{name}: expected {{min: number}} and/or {{max: number}}, got {bounds!r}")
    if target.confirm_episodes < 0:
        errors.append("target.confirm_episodes must be >= 0")
    if config.restarts.max_restarts < 0:
        errors.append("restarts.max_restarts must be >= 0")
    if errors:
        raise ValueError("invalid stage target: " + "; ".join(errors))
