"""Run directories.

A learner always trains from scratch. When it starts, whatever its run directory holds from an earlier run is moved
to ``<runs_dir>/_archive/<run>-<time>/`` (nothing is deleted). While it trains, only the newest numbered checkpoints
are kept (latest.pt and best.pt are separate files and always stay).
"""

from __future__ import annotations

import time
from pathlib import Path

ARCHIVE_DIR = "_archive"
CHECKPOINT_GLOB = "checkpoint_*.pt"


def prune_checkpoints(run_dir: Path, keep: int) -> list[Path]:
    """Delete all but the newest `keep` numbered checkpoints (0 keeps them all); returns the deleted files."""
    if keep <= 0:
        return []

    # checkpoint_<update, zero-padded>.pt: name order is update order.
    checkpoints = sorted(run_dir.glob(CHECKPOINT_GLOB))
    removed = checkpoints[:-keep]
    for path in removed:
        path.unlink(missing_ok=True)
    return removed


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
