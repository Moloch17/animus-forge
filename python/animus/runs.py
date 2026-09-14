"""Run directories.

A learner always trains from scratch. When it starts, whatever its run directory holds from an earlier run is moved
to ``<runs_dir>/_archive/<run>-<time>/`` (nothing is deleted).
"""

from __future__ import annotations

import time
from pathlib import Path

ARCHIVE_DIR = "_archive"


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
