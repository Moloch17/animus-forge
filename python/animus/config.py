"""Training run configuration, loaded from YAML (see configs/)."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field, fields, is_dataclass
from pathlib import Path

import yaml

from .mappo.trainer import MappoConfig

REPORT_COLUMNS = (
    "dps", "killed", "died", "deaths", "time_to_kill", "damage_taken", "kills", "pulls_cleared", "wipes",
    "owner_deaths", "owner_healing", "casts_completed", "casts_cancelled", "cancelled_stopped", "cancelled_moved",
    "cancelled_target", "cancelled_other", "cast_seconds_wasted", "consumables_used", "self_resurrections", "revives",
)


@dataclass
class EvalConfig:
    """Seeded evaluation (see animus.evaluation): the same characters and opponents every time."""

    every_env_steps: int = 0  # evaluate after this many training env steps; 0 = never
    at_start: bool = True  # also evaluate the starting (seeded or fresh) networks before training
    episodes: int = 128  # seeded episodes per evaluation
    seed: int = 1000  # seed base: which characters and opponents
    deterministic: bool = True  # argmax actions instead of sampling
    baseline: str = ""  # sim scripted policy scored once per run on the same seeds ("greedy", "fight")
    report: tuple[str, ...] = REPORT_COLUMNS  # episode info columns printed per level band, when present


@dataclass
class PlateauConfig:
    """Stop training once evaluation stops improving. Needs eval.every_env_steps."""

    patience: int = 0  # evaluations without improvement before stopping; 0 = train to total_env_steps
    min_improvement: float = 0.02  # an improvement beats the best score by this fraction of |best| ...
    min_improvement_abs: float = 0.01  # ... or by this much, whichever is larger
    min_env_steps: int = 0  # never stop before this many env steps


@dataclass
class TrainConfig:
    run_name: str = "run"
    runs_dir: str = "runs"
    socket: str = "/tmp/animus-forge.sock"
    seed: int = 1

    total_env_steps: int = 5_000_000  # decisions x envs x agents
    rollout_length: int = 128
    log_every: int = 1  # updates
    checkpoint_every: int = 25  # updates

    train_device: str = "cpu"
    rollout_device: str = "cpu"

    # A run seeds its networks from an earlier-stage checkpoint (see animus.bootstrap): the first of these
    # candidates that exists, so a stage still seeds from the closest earlier stage when the ones in between were
    # skipped. "{run_name}" is replaced by this run's name. A best.pt that does not exist falls back to the latest.pt
    # beside it.
    init_from: str | list[str] = ""

    def resolved_init_from(self) -> list[str]:
        candidates = [self.init_from] if isinstance(self.init_from, str) else list(self.init_from or [])
        return [c.format(run_name=self.run_name) for c in candidates if c]

    mappo: MappoConfig = field(default_factory=MappoConfig)
    eval: EvalConfig = field(default_factory=EvalConfig)
    plateau: PlateauConfig = field(default_factory=PlateauConfig)

    @classmethod
    def load(cls, path: str | Path, overrides: list[str] | None = None) -> "TrainConfig":
        """Load YAML, then apply "key=value" overrides (dotted keys for sections, values parsed as YAML)."""
        raw = yaml.safe_load(Path(path).read_text()) or {}
        for override in overrides or ():
            apply_override(raw, override)
        return from_dict(cls, raw)

    def to_dict(self) -> dict:
        return asdict(self)


def apply_override(raw: dict, override: str) -> None:
    key, sep, value = override.partition("=")
    if not sep or not key:
        raise ValueError(f"override '{override}' is not key=value")

    *sections, name = key.split(".")
    target = raw
    for section in sections:
        target = target.setdefault(section, {})
        if not isinstance(target, dict):
            raise ValueError(f"override '{override}': {section} is not a section")
    target[name] = yaml.safe_load(value)


def from_dict(cls, raw: dict, prefix: str = ""):
    """Build dataclass `cls` from a dict, recursing into dataclass fields; unknown keys are an error."""
    raw = dict(raw or {})
    by_name = {f.name: f for f in fields(cls)}
    unknown = sorted(f"{prefix}{k}" for k in raw if k not in by_name)
    if unknown:
        raise ValueError(f"unknown config keys: {unknown}")

    kwargs = {}
    for name, value in raw.items():
        default = by_name[name].default_factory() if callable(by_name[name].default_factory) else by_name[name].default
        if is_dataclass(default):
            kwargs[name] = from_dict(type(default), value, f"{prefix}{name}.")
        elif isinstance(default, tuple) and isinstance(value, list):
            kwargs[name] = tuple(value)
        else:
            kwargs[name] = value
    return cls(**kwargs)
