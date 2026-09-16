"""Training run configuration, loaded from YAML (see configs/).

A config may start with ``extends: <other>.yaml`` (relative to its own file): it is merged over that config, section by
section, so a curriculum stage lists only what differs from the base. Overlays (``--overlay``, e.g. configs/fast.yaml
for ``forge fast``) are merged over the whole result the same way, so one file changes every stage.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field, fields, is_dataclass
from pathlib import Path
from types import UnionType
from typing import Union, get_args, get_origin, get_type_hints

import yaml

from .mappo.trainer import MappoConfig
from .stages import merges, seed_chain

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
    # Seeded episodes per evaluation. The score's standard error falls with the square root of this, and that error
    # sets the convergence margin (a new best must clear it) and the per-layout gates' noise allowance, so too few
    # episodes hide real progress behind noise. An evaluation costs seconds against tens of minutes of training
    # between them: prefer more. Spread over the stage's layouts, each needs its own share (target.min_layout_episodes).
    episodes: int = 1024
    seed: int = 1000  # seed base: which characters and opponents
    deterministic: bool = True  # argmax actions instead of sampling
    baseline: str = ""  # sim scripted policy scored once per run on the same seeds ("greedy", "fight")
    # Self-play arenas: the baseline also plays the other side of every self-play episode when the learner is scored
    # (and against itself when the baseline is), so the score is the learner against a fixed opponent.
    opponent_baseline: bool = False
    report: tuple[str, ...] = REPORT_COLUMNS  # episode info columns printed per level band, when present


@dataclass
class ConvergenceConfig:
    """When the stage is done learning (animus.evaluation.ConvergenceTracker). Needs eval.every_env_steps."""

    patience: int = 0  # evaluations without a new best before converging; 0 = train to total_env_steps
    window: int = 4  # latest evaluations whose trend must be flat too
    z: float = 2.0  # a new best beats the best by this many standard errors ...
    min_improvement: float = 0.02  # ... or by this fraction of |best| ...
    min_improvement_abs: float = 0.01  # ... or by this much, whichever is largest
    min_env_steps: int = 0  # never converge before this many env steps (counted again after each restart)


@dataclass
class TargetConfig:
    """When the stage is good enough to move on (animus.gates). Unset gates are not checked; with none set, the
    stage moves on as soon as it converges."""

    # Overall score >= baseline + this x |baseline|, on the eval.baseline scripted policy's seeds: 0.2 = 20% better.
    min_over_baseline: float | None = None
    # Every class/role's score >= its baseline + this x |baseline|: a looser floor so no layout is left behind.
    min_layout_over_baseline: float | None = None
    # Layouts with fewer eval rows than this (one per seat with a character in each seeded episode) are too noisy to
    # gate. Named episodes for the configs' sake; in the party stage one episode gives up to four rows.
    min_layout_episodes: int = 16
    # Episode info means, e.g. {killed: {min: 0.9}, died: {max: 0.1}}.
    metrics: dict = field(default_factory=dict)
    # Per arena of a stage that mixes arenas (names from stage.json), the same gates on that arena's episodes only,
    # e.g. {duel: {min_over_baseline: 0.1}, pvp_scripted: {metrics: {won: {min: 0.5}}}}.
    arenas: dict = field(default_factory=dict)
    min_arena_episodes: int = 16  # arenas with fewer eval episodes than this are too noisy to gate
    # Score gates pass when the score is within this many standard errors of what they require (of the difference
    # between the two means, the learner's and the baseline's). Each layout is scored on its share of the episodes
    # only, so without an allowance a class/role that is truly level with its baseline fails about half the time.
    # 0 = compare the raw means.
    noise_z: float = 1.0
    # Before moving on, the best networks are scored again on seeds training never evaluated, and must pass again:
    # a best picked out of many evaluations is partly luck. 0 = trust the evaluation that set the best.
    confirm_episodes: int = 2048
    confirm_seed: int = 50000

    @property
    def enabled(self) -> bool:
        return (self.min_over_baseline is not None or self.min_layout_over_baseline is not None
                or bool(self.metrics) or bool(self.arenas))

    def arena_needs_baseline(self) -> bool:
        return any(isinstance(gates, dict) and gates.get("min_over_baseline") is not None
                   for gates in self.arenas.values())


@dataclass
class RestartConfig:
    """Escaping a local optimum: a stage that converges below its target restarts from its best networks with
    more exploration, up to max_restarts times; after that the learner exits with an error and the queue halts."""

    max_restarts: int = 2
    entropy_boost: float = 3.0  # mappo.entropy_coef x this right after a restart ...
    entropy_half_life_env_steps: int = 10_000_000  # ... decaying back to entropy_coef with this half-life
    reset_optimizers: bool = True  # fresh Adam state, so steps are full-sized again
    # Shrink and perturb: weights = shrink x best + perturb x freshly initialised weights. 1 and 0 = off.
    shrink: float = 1.0
    perturb: float = 0.0


@dataclass
class EntropyFloorConfig:
    """Keep exploration from collapsing, measured against how many actions were actually legal.

    A masked action space makes a flat entropy coefficient hard to reason about: the ceiling is ln(legal
    actions), which swings with level, cooldowns and the global cooldown, and is nothing like ln(the padded
    action count). This raises mappo.entropy_coef when the policy's entropy falls below `fraction` of that
    ceiling and lets it fall back to the configured value once it is above -- a floor, never a ceiling, so it
    cannot hold a converging policy stochastic.
    """

    fraction: float = 0.0  # of ln(allowed actions); 0 = off, the coefficient stays where it is configured
    max_boost: float = 4.0  # never raise the coefficient past this many times the configured one
    rate: float = 0.05  # how fast it moves per update, as a fraction of the distance


@dataclass
class DistillConfig:
    """Kickstarting a merge stage (animus.distill): on the decisions of each arena that has a teacher -- the parent
    stage whose model already plays it -- the policy loss gains coef x KL(teacher || policy), and coef decays with
    half_life_env_steps so PPO takes over."""

    # "" = off; "auto" = every arena of the stage that a parent (the extended stage, then the merges, in order) has,
    # taught by that parent's best.pt (else latest.pt); or {arena: checkpoint path} with {runs_dir} filled in.
    teachers: str | dict = ""
    coef: float = 1.0
    half_life_env_steps: int = 30_000_000
    min_coef: float = 0.0  # the coefficient never decays below this; below 1e-4 the term is not computed

    def coef_at(self, env_steps: int) -> float:
        decay = 0.5 ** (env_steps / self.half_life_env_steps) if self.half_life_env_steps > 0 else 1.0
        return max(self.min_coef, self.coef * decay)


@dataclass
class LayoutSamplingConfig:
    """Training episodes draw a class/role uniformly, so each layout gets its share of the data whatever it is
    worth. A stage is gated on its weakest layout, though, so the data is worth most where the score is furthest
    below the baseline. After every evaluation the learner sends the sim a weight per layout (protocol WEIGHTS) and
    training episodes draw layouts in proportion; evaluation stays uniform, whatever the weights are.

    Needs eval.every_env_steps and eval.baseline: the weights come from the gap to the baseline's per-layout score.
    """

    enabled: bool = False
    strength: float = 1.0  # e^(strength x gap in standard deviations of the gaps): 0 = uniform
    max_ratio: float = 3.0  # the heaviest layout draws at most this many times the lightest


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

    # Run the PPO update on a worker thread, so the sim collects the next rollout instead of waiting for it. The
    # rollout then acts on the weights of the update before last (the rollout networks are synced when the update is
    # joined, one rollout later), which is data one update staler than the strictly serial loop; its log_probs come
    # from the same weights, so the PPO ratio stays consistent. Update stats are logged one update late as well.
    overlap_updates: bool = False

    train_device: str = AUTO  # "auto": cuda when torch sees a GPU (ROCm included), else cpu
    rollout_device: str = "cpu"  # one small forward pass per decision is faster on the CPU
    # CPU threads torch may use; 0 = torch's own default (a thread per core). The learner shares the machine with the
    # sim's map update threads, so fewer can be faster overall (the sim's `forge bench` sweeps both).
    torch_threads: int = 0

    # Checkpoints to seed the networks from (see animus.bootstrap): the first candidate that exists. "auto" takes the
    # stage's seed chain from the sim's stage.json (the closest earlier stage that has been trained); a list names
    # them, with {runs_dir} and {run_name} filled in. A best.pt that does not exist falls back to the latest.pt beside
    # it. Empty = train from scratch.
    init_from: str | list[str] = AUTO
    # A merge stage's further parents (stage.json merges), seeding the blocks only they have after init_from: "auto"
    # takes each merged stage's best.pt (else latest.pt); a list names checkpoints; empty = none.
    merge_from: str | list[str] = AUTO

    mappo: MappoConfig = field(default_factory=MappoConfig)
    distill: DistillConfig = field(default_factory=DistillConfig)
    eval: EvalConfig = field(default_factory=EvalConfig)
    convergence: ConvergenceConfig = field(default_factory=ConvergenceConfig)
    target: TargetConfig = field(default_factory=TargetConfig)
    restarts: RestartConfig = field(default_factory=RestartConfig)
    layout_sampling: LayoutSamplingConfig = field(default_factory=LayoutSamplingConfig)
    entropy_floor: EntropyFloorConfig = field(default_factory=EntropyFloorConfig)

    def resolved_init_from(self, stage: dict | None) -> list[str]:
        if self.init_from == AUTO:
            return [str(Path(self.runs_dir) / name / "best.pt") for name in seed_chain(stage)]
        candidates = [self.init_from] if isinstance(self.init_from, str) else list(self.init_from or [])
        return [c.format(runs_dir=self.runs_dir, run_name=self.run_name) for c in candidates if c]

    def resolved_merge_from(self, stage: dict | None) -> list[str]:
        if self.merge_from == AUTO:
            return [str(Path(self.runs_dir) / name / "best.pt") for name in merges(stage)]
        candidates = [self.merge_from] if isinstance(self.merge_from, str) else list(self.merge_from or [])
        return [c.format(runs_dir=self.runs_dir, run_name=self.run_name) for c in candidates if c]

    def named_teachers(self) -> dict[str, str]:
        """distill.teachers as {arena: checkpoint path}, {runs_dir} filled in; {} when off or "auto"."""
        teachers = self.distill.teachers
        if isinstance(teachers, dict):
            return {arena: str(path).format(runs_dir=self.runs_dir, run_name=self.run_name)
                    for arena, path in teachers.items()}
        if teachers not in ("", AUTO):
            raise ValueError(f"distill.teachers: expected \"\", \"auto\" or {{arena: checkpoint}}, got {teachers!r}")
        return {}

    def resolved_train_device(self) -> str:
        return resolve_device(self.train_device)

    def resolved_rollout_device(self) -> str:
        return resolve_device(self.rollout_device)

    @classmethod
    def load(
        cls, path: str | Path, overrides: list[str] | None = None, overlays: list[str | Path] | None = None
    ) -> "TrainConfig":
        """Load YAML (following extends), merge each overlay file over it, then apply "key=value" overrides (dotted
        keys for sections, values parsed as YAML)."""
        raw = load_yaml(path)
        for overlay in overlays or ():
            raw = merge(raw, load_yaml(overlay))
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
    """`override` over `base`: sections merge key by key, anything else is replaced. An empty map replaces too, so
    `metrics: {}` in an overlay clears what the stage set."""
    merged = dict(base)
    for key, value in override.items():
        if isinstance(value, dict) and value and isinstance(merged.get(key), dict):
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


def _matches(value, hint) -> bool:
    """Whether a YAML value fits a field's type hint (ints fit floats; bools are never numbers)."""
    origin = get_origin(hint)
    if origin in (Union, UnionType):
        return any(_matches(value, arg) for arg in get_args(hint))
    if hint is type(None):
        return value is None
    if hint is float:
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if hint is int:
        return isinstance(value, int) and not isinstance(value, bool)
    if hint in (bool, str, dict):
        return isinstance(value, hint)
    if origin in (tuple, list):
        # YAML gives sequences as lists; elements are checked loosely (a tuple[int, ...] of numbers).
        if not isinstance(value, (list, tuple)):
            return False
        element = next((arg for arg in get_args(hint) if arg is not Ellipsis), None)
        return element is None or all(_matches(item, element) for item in value)
    if origin is dict:
        return isinstance(value, dict)
    return True


def from_dict(cls, raw: dict, prefix: str = ""):
    """Build dataclass `cls` from a dict, recursing into dataclass fields; unknown keys and values of the wrong type
    are an error (so `total_env_steps: 3e8`, which YAML reads as a string, fails at load rather than hours later)."""
    raw = dict(raw or {})
    by_name = {f.name: f for f in fields(cls)}
    unknown = sorted(f"{prefix}{k}" for k in raw if k not in by_name)
    if unknown:
        raise ValueError(f"unknown config keys: {unknown}")

    hints = get_type_hints(cls)
    kwargs = {}
    for name, value in raw.items():
        default = by_name[name].default_factory() if callable(by_name[name].default_factory) else by_name[name].default
        if is_dataclass(default):
            kwargs[name] = from_dict(type(default), value, f"{prefix}{name}.")
            continue
        if not _matches(value, hints[name]):
            raise ValueError(f"config key {prefix}{name}: expected {hints[name]}, got {type(value).__name__} {value!r}")
        if isinstance(default, tuple) and isinstance(value, list):
            kwargs[name] = tuple(value)
        elif hints[name] is float and isinstance(value, int):
            kwargs[name] = float(value)
        else:
            kwargs[name] = value
    return cls(**kwargs)
