"""Stage targets: whether an evaluation summary is good enough to move on to the next curriculum stage.

Scores are only comparable within one scenario, so the score gates are relative to the scripted baseline scored on
the same seeds: "score >= baseline + ratio x |baseline|" reads as "ratio better than the baseline" whatever the sign
of the scenario's reward. The overall gate keeps the average up; the per-layout floor keeps a class/build from
hiding behind it, because the next stage seeds every layout from these networks. Score gates allow for evaluation
noise (target.noise_z standard errors of the difference), because a class/build holds only its share of the seeded
episodes: without it a layout that is genuinely level with the baseline fails about half the time. Metric gates check
episode info
means directly (killed, died, ...), which reward shaping cannot game. A stage that mixes arenas can gate each arena
on its own episodes (target.arenas), so one situation cannot hide behind the others either.

A gate relative to the baseline is only as demanding as the baseline is good, and where the scripted policy is
hopeless it demands nothing: stage1_duel passed warlock_dps against a required score of -2.34 while it killed 65%
of the time and spent a quarter of its episodes in a cast/stop loop. target.metrics and target.layout_metrics are
the absolute floors that no baseline can lower, and they read derived summary fields (livelocked) as well as
episode info.
"""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass, field
from statistics import NormalDist

from .config import TargetConfig, TrainConfig
from .evaluation import DERIVED_METRICS


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

    # The absolute floors are judged on the base tiers' episodes when the stage names them (a summary without
    # tiers above them is all base).
    floors, scope = summary, ""
    if target.base_difficulty is not None and str(target.base_difficulty) in summary.get("up_to", {}):
        floors, scope = summary["up_to"][str(target.base_difficulty)], f"tiers 0-{target.base_difficulty} "

    # Absolute per-layout bounds, which no baseline can lower. Checked whatever min_layout_over_baseline is set to.
    if target.layout_metrics:
        for name, row in floors.get("layouts", {}).items():
            if row["episodes"] < target.min_layout_episodes:
                report.skipped.append(f"{name}: {row['episodes']} episodes")
            else:
                _check_metrics(report, row, target.layout_metrics, f"{scope}{name} ")

    for spec, bounds in target.spec_metrics.items():
        row = floors.get("specs", {}).get(spec)
        if row is None or not row.get("episodes"):
            report.skipped.append(f"build {spec}: no episodes")
        elif row["episodes"] < target.min_layout_episodes:
            report.skipped.append(f"build {spec}: {row['episodes']} episodes")
        else:
            _check_metrics(report, row, bounds, f"{scope}build {spec} ")

    _check_metrics(report, floors, target.metrics, scope)

    for arena, gates in target.arenas.items():
        _check_group(report, f"arena {arena}", summary.get("arenas", {}).get(arena),
                     (baseline or {}).get("arenas", {}).get(arena), gates, target)
    for tier, gates in target.difficulties.items():
        # A stage without difficulty tiers (no creature duel, or one tier) is all tier 0: a stage extending one
        # that gates tier 0 keeps the same bar.
        row = summary.get("difficulties", {}).get(tier)
        base = (baseline or {}).get("difficulties", {}).get(tier)
        if not summary.get("difficulties") and tier == "0":
            row, base = summary, baseline
        _check_group(report, f"tier {tier}", row, base, gates, target)

    return report


def _check_group(report: GateReport, label: str, row: dict | None, base: dict | None, gates: dict,
                 target: TargetConfig) -> None:
    """An arena's or a difficulty tier's gates, on its own rows of the summary."""
    if row is None or row.get("score") is None:
        report.fail(f"{label}: not in the evaluation summary")
        return
    if row["episodes"] < target.min_arena_episodes:
        report.skipped.append(f"{label}: {row['episodes']} episodes")
        return

    ratio = gates.get("min_over_baseline")
    if ratio is not None:
        if base is None or base.get("score") is None:
            report.fail(f"{label}: no baseline score to compare with")
        else:
            _check_score(report, f"{label} score", row, base, ratio, target.noise_z)

    _check_metrics(report, row, gates.get("metrics", {}), f"{label} ")


def wilson_bound(share: float, episodes: int, confidence: float, lower: bool) -> float:
    """The one-sided Wilson bound on a share of `episodes` at `confidence`: the lowest (or highest) true rate the
    observed share is still consistent with. No episodes bound nothing: 0 below, 1 above."""
    if episodes <= 0:
        return 0.0 if lower else 1.0
    z = NormalDist().inv_cdf(confidence)
    n = float(episodes)
    centre = share + z * z / (2.0 * n)
    spread = z * math.sqrt(share * (1.0 - share) / n + z * z / (4.0 * n * n))
    bound = (centre - spread if lower else centre + spread) / (1.0 + z * z / n)
    return min(1.0, max(0.0, bound))


def _check_metrics(report: GateReport, row: dict, metrics: dict, prefix: str) -> None:
    """Bounds on summary means. With `confidence` a share (killed, clean_kill, livelocked) is judged by its Wilson
    bound over the row's episodes rather than its raw mean: a minimum must hold for the lowest rate the episodes are
    consistent with and a maximum for the highest, so a class/build cannot pass on a lucky handful of episodes."""
    for name, bounds in metrics.items():
        value = row.get(name)
        if value is None:
            report.fail(f"{prefix}{name}: not in the evaluation summary")
            continue
        confidence = bounds.get("confidence")
        episodes = int(row.get("episodes") or 0)
        for kind, lower in (("min", True), ("max", False)):
            if kind not in bounds:
                continue
            judged, label = value, f"{prefix}{name} ({kind})"
            if confidence is not None:
                judged = wilson_bound(float(value), episodes, float(confidence), lower)
                label = f"{prefix}{name} ({kind}, {confidence:.0%} {'lower' if lower else 'upper'} bound)"
            passed = judged >= bounds[kind] if lower else judged <= bounds[kind]
            report.check(label, judged, bounds[kind], passed)


def _metric_errors(prefix: str, metrics, info_names) -> list[str]:
    if not isinstance(metrics, dict):
        return [f"{prefix}: expected {{name: {{min/max: number}}}}, got {metrics!r}"]
    errors = []
    for name, bounds in metrics.items():
        if name not in info_names:
            errors.append(f"{prefix}.{name}: the scenario has no such episode info ({', '.join(info_names)})")
        if not isinstance(bounds, dict) or not ({"min", "max"} & set(bounds)) \
                or set(bounds) - {"min", "max", "confidence"} \
                or not all(isinstance(v, (int, float)) for v in bounds.values()):
            errors.append(f"{prefix}.{name}: expected {{min: number}} and/or {{max: number}}, optionally with "
                          f"{{confidence: number}}, got {bounds!r}")
        elif "confidence" in bounds and not 0.0 < bounds["confidence"] < 1.0:
            errors.append(f"{prefix}.{name}.confidence: must be between 0 and 1, got {bounds['confidence']!r}")
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
    # Derived summary fields (livelocked) are gateable by name although they are not episode info.
    names = (*info_names, *DERIVED_METRICS)
    errors += _metric_errors("target.metrics", target.metrics, names)
    errors += _metric_errors("target.layout_metrics", target.layout_metrics, names)
    if not isinstance(target.spec_metrics, dict):
        errors.append(f"target.spec_metrics: expected {{spec: {{name: bounds}}}}, got {target.spec_metrics!r}")
    else:
        # A build name cannot be checked against a fixed list the way a role could -- the names are the classes'
        # own, and which of them a run has depends on AnimusForge.Classes. A name nothing plays is reported as
        # skipped for want of episodes rather than refused at startup.
        for spec, bounds in target.spec_metrics.items():
            errors += _metric_errors(f"target.spec_metrics.{spec}", bounds, names)
    for arena, gates in target.arenas.items():
        prefix = f"target.arenas.{arena}"
        if arena_names is not None and arena not in arena_names:
            errors.append(f"{prefix}: the stage has no such arena ({', '.join(arena_names) or 'none listed'})")
        if not isinstance(gates, dict) or set(gates) - {"min_over_baseline", "metrics"}:
            errors.append(f"{prefix}: expected min_over_baseline and/or metrics, got {gates!r}")
            continue
        if gates.get("min_over_baseline") is not None and not isinstance(gates["min_over_baseline"], (int, float)):
            errors.append(f"{prefix}.min_over_baseline: expected a number")
        errors += _metric_errors(f"{prefix}.metrics", gates.get("metrics", {}), names)
    for tier, gates in target.difficulties.items():
        prefix = f"target.difficulties.{tier}"
        if not str(tier).isdigit():
            errors.append(f"{prefix}: a tier is a whole number")
        if not isinstance(gates, dict) or set(gates) - {"min_over_baseline", "metrics"}:
            errors.append(f"{prefix}: expected min_over_baseline and/or metrics, got {gates!r}")
            continue
        errors += _metric_errors(f"{prefix}.metrics", gates.get("metrics", {}), names)
    sampling = config.layout_sampling
    if sampling.enabled and sampling.metric and sampling.metric not in names:
        errors.append(f"layout_sampling.metric: {sampling.metric} is neither episode info nor a derived field")
    if not 0.0 <= sampling.replay_fraction <= 1.0:
        errors.append("layout_sampling.replay_fraction must be between 0 and 1")
    if sampling.enabled and sampling.replay_fraction > 0.0 and not sampling.metric:
        errors.append("layout_sampling.replay_fraction needs layout_sampling.metric to tell lost episodes")
    if target.min_arena_episodes < 0:
        errors.append("target.min_arena_episodes must be >= 0")
    if target.noise_z < 0:
        errors.append("target.noise_z must be >= 0")
    base = target.base_difficulty
    if base is not None and (isinstance(base, bool) or not isinstance(base, int) or base < 0):
        errors.append("target.base_difficulty must be a whole number >= 0")
    if target.confirm_episodes < 0:
        errors.append("target.confirm_episodes must be >= 0")
    if config.restarts.max_restarts < 0:
        errors.append("restarts.max_restarts must be >= 0")
    if errors:
        raise ValueError("invalid stage target: " + "; ".join(errors))
