"""Training run configuration, loaded from YAML (see configs/).

A config may start with ``extends: <other>.yaml`` (relative to its own file): it is merged over that config, section by
section, so a curriculum stage lists only what differs from the base.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field, fields, is_dataclass
from pathlib import Path

import yaml

from .mappo.trainer import MappoConfig
from .stages import seed_chain

EXTENDS_KEY = "extends"
AUTO = "auto"

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
    runs_dir: str = "runs"  # the sim passes AnimusForge.OutputDir/runs
    layouts_dir: str = "layouts"  # the sim passes AnimusForge.OutputDir/layouts
    socket: str = "/tmp/animus-forge.sock"
    seed: int = 1

    total_env_steps: int = 5_000_000  # decisions x envs x agents
    rollout_length: int = 128
    log_every: int = 1  # updates
    checkpoint_every: int = 25  # updates
    keep_checkpoints: int = 5  # numbered checkpoint_*.pt files kept (latest.pt and best.pt always are); 0 = all

    train_device: str = AUTO  # "auto": cuda when torch sees a GPU (ROCm included), else cpu
    rollout_device: str = "cpu"  # one small forward pass per decision is faster on the CPU

    # Checkpoints to seed the networks from (see animus.bootstrap): the first candidate that exists. "auto" takes the
    # stage's seed chain from the sim's stage.json (the closest earlier stage that has been trained); a list names
    # them, with {runs_dir} and {run_name} filled in. A best.pt that does not exist falls back to the latest.pt beside
    # it. Empty = train from scratch.
    init_from: str | list[str] = AUTO

    mappo: MappoConfig = field(default_factory=MappoConfig)
    eval: EvalConfig = field(default_factory=EvalConfig)
    plateau: PlateauConfig = field(default_factory=PlateauConfig)

    def resolved_init_from(self, stage: dict | None) -> list[str]:
        if self.init_from == AUTO:
            return [str(Path(self.runs_dir) / name / "best.pt") for name in seed_chain(stage)]
        candidates = [self.init_from] if isinstance(self.init_from, str) else list(self.init_from or [])
        return [c.format(runs_dir=self.runs_dir, run_name=self.run_name) for c in candidates if c]

    def resolved_train_device(self) -> str:
        return resolve_device(self.train_device)

    def resolved_rollout_device(self) -> str:
        return resolve_device(self.rollout_device)

    @classmethod
    def load(cls, path: str | Path, overrides: list[str] | None = None) -> "TrainConfig":
        """Load YAML (following extends), then apply "key=value" overrides (dotted keys for sections, values parsed
        as YAML)."""
        raw = load_yaml(path)
        for override in overrides or ():
            apply_override(raw, override)
        return from_dict(cls, raw)

    def to_dict(self) -> dict:
        return asdict(self)


def resolve_device(name: str) -> str:
    if name != AUTO:
        return name

    import torch

    return "cuda" if torch.cuda.is_available() else "cpu"


def load_yaml(path: str | Path, seen: tuple[Path, ...] = ()) -> dict:
    """A config file with its extends chain merged in, base first."""
    path = Path(path).resolve()
    if path in seen:
        raise ValueError(f"config {path} extends itself")

    raw = yaml.safe_load(path.read_text()) or {}
    base = raw.pop(EXTENDS_KEY, None)
    if not base:
        return raw
    return merge(load_yaml(path.parent / base, (*seen, path)), raw)


def merge(base: dict, override: dict) -> dict:
    """`override` over `base`: sections merge key by key, anything else is replaced."""
    merged = dict(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(merged.get(key), dict):
            merged[key] = merge(merged[key], value)
        else:
            merged[key] = value
    return merged


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
