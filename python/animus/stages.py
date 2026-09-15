"""The sim's stage descriptions.

When the worldserver builds a class/role stage it writes ``<layouts_dir>/<scenario>/stage.json`` beside the stage's
layout manifests: the stage's blocks, the stages it seeds from (``seed_chain``, closest first), the model name of
every layout, its episode info columns and the effective tuning. The learner copies it into the run directory, seeds
from it, and export names models from it.
"""

from __future__ import annotations

import json
from pathlib import Path

STAGE_FILE = "stage.json"


def stage_dir(layouts_dir: str | Path, scenario: str) -> Path:
    return Path(layouts_dir) / scenario


def load_stage(layouts_dir: str | Path, scenario: str) -> dict | None:
    """The scenario's stage.json, or None for a scenario without one (not a class/role stage, or not built yet)."""
    path = stage_dir(layouts_dir, scenario) / STAGE_FILE
    return json.loads(path.read_text()) if path.is_file() else None


def seed_chain(stage: dict | None) -> list[str]:
    """The stages a run of `stage` seeds from, closest first."""
    return list(stage.get("seed_chain", ())) if stage else []


Span = tuple[int, int]  # (first, count)


def block_spans(stage: dict | None, layout: str) -> dict[str, tuple[Span, Span]] | None:
    """Block name -> (observation span, action span) of `layout` in `stage`, or None when the stage has no spans for
    it (a stage.json from before block spans, or no stage.json)."""
    entry = (stage or {}).get("layouts", {}).get(layout)
    if not entry:
        return None
    return {block["name"]: (tuple(block["obs"]), tuple(block["actions"])) for block in entry["blocks"]}


def model_names(stage: dict | None) -> dict[str, str]:
    """Layout name -> model name (warrior_dps -> warrior_dps_duel)."""
    return dict(stage.get("models", {})) if stage else {}
