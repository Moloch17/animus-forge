"""Training run configuration, loaded from YAML (see configs/)."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field, fields
from pathlib import Path

import yaml

from .mappo.trainer import MappoConfig


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

    mappo: MappoConfig = field(default_factory=MappoConfig)

    @classmethod
    def load(cls, path: str | Path) -> "TrainConfig":
        raw = yaml.safe_load(Path(path).read_text()) or {}
        mappo_raw = raw.pop("mappo", {}) or {}

        known = {f.name for f in fields(cls)}
        unknown = set(raw) - known
        mappo_known = {f.name for f in fields(MappoConfig)}
        unknown |= {f"mappo.{k}" for k in set(mappo_raw) - mappo_known}
        if unknown:
            raise ValueError(f"unknown config keys: {sorted(unknown)}")

        if "hidden" in mappo_raw:
            mappo_raw["hidden"] = tuple(mappo_raw["hidden"])
        return cls(**raw, mappo=MappoConfig(**mappo_raw))

    def to_dict(self) -> dict:
        return asdict(self)
