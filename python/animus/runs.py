"""Run directories: clean runs.

A clean run is named by an id (AnimusForge.Learner.CleanRun). The first time a run directory is used under a
new id, whatever it holds from earlier runs is moved to ``<runs_dir>/_archive/`` and training starts from
scratch; the fresh directory records the id, so restarting the server under the same id resumes the clean run
instead of starting it over again.
"""

from __future__ import annotations

import time
from pathlib import Path

ARCHIVE_DIR = "_archive"
CLEAN_RUN_MARKER = "clean_run_id"


def start_clean_run(run_dir: Path, clean_run_id: str) -> Path | None:
    """Prepare ``run_dir`` for clean run ``clean_run_id``; returns where an earlier run was archived, if any."""
    marker = run_dir / CLEAN_RUN_MARKER
    if marker.exists() and marker.read_text().strip() == clean_run_id:
        return None

    archived = None
    if run_dir.exists() and any(run_dir.iterdir()):
        archive = run_dir.parent / ARCHIVE_DIR
        archive.mkdir(parents=True, exist_ok=True)
        archived = archive / f"{run_dir.name}-{time.strftime('%Y%m%d-%H%M%S')}"
        suffix = 1
        while archived.exists():
            archived = archive / f"{run_dir.name}-{time.strftime('%Y%m%d-%H%M%S')}-{suffix}"
            suffix += 1
        run_dir.rename(archived)

    run_dir.mkdir(parents=True, exist_ok=True)
    marker.write_text(clean_run_id + "\n")
    return archived
