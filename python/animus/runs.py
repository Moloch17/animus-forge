"""Run directories.

A learner trains from scratch by default. When it starts, whatever its run directory holds from an earlier run is
moved to ``<runs_dir>/_archive/<run>-<time>/`` (nothing is deleted).

``--resume`` (the sim's ``forge resume``) is the one exception: it continues the run in place from its
``latest.pt``, provided the scenario still has the same shapes (see ``resume_mismatch``).
"""

from __future__ import annotations

import time
from pathlib import Path

ARCHIVE_DIR = "_archive"
LATEST_CHECKPOINT = "latest.pt"
FINISHED_FILE = "finished.json"

# The spec fields the networks' shapes depend on. The env count, decision interval and episode length may change
# between a run and its resume.
RESUME_SPEC_KEYS = ("scenario", "agents_per_env", "obs_dim", "state_dim", "num_actions", "layouts")


def archive_run(run_dir: Path) -> Path | None:
    """Move an earlier run out of ``run_dir`` and leave it empty; returns where it went, if there was one."""
    archived = None
    if run_dir.exists() and any(run_dir.iterdir()):
        archive = run_dir.parent / ARCHIVE_DIR
        archive.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        archived = archive / f"{run_dir.name}-{stamp}"
        suffix = 1
        while archived.exists():
            archived = archive / f"{run_dir.name}-{stamp}-{suffix}"
            suffix += 1
        run_dir.rename(archived)

    run_dir.mkdir(parents=True, exist_ok=True)
    return archived


def resume_checkpoint_path(run_dir: Path) -> Path:
    """The checkpoint a resume continues from; raises FileNotFoundError when the run has none."""
    path = run_dir / LATEST_CHECKPOINT
    if not path.exists():
        raise FileNotFoundError(f"cannot resume {run_dir.name}: {path} does not exist")
    return path


def _normalise(value):
    if isinstance(value, dict):
        return {k: _normalise(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_normalise(v) for v in value]
    return value


def resume_mismatch(checkpoint_spec: dict, spec: dict) -> list[str]:
    """Spec fields that differ between a checkpoint and the sim now (empty = the run can resume)."""
    return [
        key for key in RESUME_SPEC_KEYS
        if _normalise(checkpoint_spec.get(key)) != _normalise(spec.get(key))
    ]
