"""The manual's tables against the files they describe.

Every stage budget in the manual was between 1x and 7x the configured value before this existed -- stage15_party
was documented at 600M against a configured 120M, stage23_crossroads at 1B against 150M. Numbers copied by hand
into prose drift silently and nobody notices until someone plans a run from them, so the table is checked instead
of trusted.
"""

import re
from pathlib import Path

import pytest

from animus.config import TrainConfig

CONFIGS = Path(__file__).resolve().parents[1] / "configs"
MANUAL = Path(__file__).resolve().parents[2] / "docs" / "manual" / "04-curriculum.md"

# `| `stage5_duel` | 300M | 10M | 2048 | 30M |` -- the table pairs two stages per row, so each line yields two.
ROW = re.compile(
    r"\|\s*`(?P<name>\w+)`\s*\|\s*(?P<budget>[\d.]+)M\s*\|\s*(?P<every>[\d.]+)M\s*\|"
    r"\s*(?P<episodes>\d+)\s*\|\s*(?P<min>[\d.]+)M\s*(?=\|)")


def documented() -> dict[str, dict]:
    out = {}
    for line in MANUAL.read_text().splitlines():
        for m in ROW.finditer(line):
            out[m.group("name")] = {
                "total_env_steps": int(float(m.group("budget")) * 1e6),
                "every_env_steps": int(float(m.group("every")) * 1e6),
                "episodes": int(m.group("episodes")),
                "min_env_steps": int(float(m.group("min")) * 1e6),
            }
    return out


def configured(name: str) -> dict:
    config = TrainConfig.load(CONFIGS / f"{name}.yaml")
    return {
        "total_env_steps": config.total_env_steps,
        "every_env_steps": config.eval.every_env_steps,
        "episodes": config.eval.episodes,
        "min_env_steps": config.convergence.min_env_steps,
    }


def test_the_table_covers_every_stage_config():
    """A stage added without a row is the way the table goes stale next."""
    on_disk = {p.stem for p in CONFIGS.glob("*.yaml")} - {"fast"}
    assert on_disk - set(documented()) == set(), "these configs have no row in the manual's budget table"


@pytest.mark.parametrize("name", sorted(documented()))
def test_the_manual_matches_the_config(name):
    assert documented()[name] == configured(name), f"04-curriculum.md disagrees with configs/{name}.yaml"


def test_the_queue_total_is_what_the_manual_says():
    """The manual states the whole queue in one number, which is the one a person plans a run from."""
    rows = documented()
    # The mix_duel_pvp pilot is not in the default queue (StageDefinition::InDefaultQueue is false): it is trained
    # by name. stage1b_indoor joined the queue after stage1_move on 2026-09-23.
    outside = {"mix_duel_pvp"}
    queue = sum(v["total_env_steps"] for k, v in rows.items() if k not in outside)
    assert queue == 2_130_000_000, f"the queue is {queue/1e6:.0f}M; the manual says 2,130M"
    assert sum(v["total_env_steps"] for v in rows.values()) == 2_190_000_000


# --------------------------------------------------------------------------- 8.2 tuning defaults

REFERENCE = Path(__file__).resolve().parents[2] / "docs" / "manual" / "08-reference.md"
CONF_DIST = Path(__file__).resolve().parents[2] / "conf" / "mod_animus_forge.conf.dist"


def _template_defaults() -> dict[str, str]:
    text = CONF_DIST.read_text()
    return {m.group(1): m.group(2).strip().strip('"')
            for m in re.finditer(r"^AnimusForge\.Curriculum\.([\w.]+)\s*=\s*(.+?)\s*$", text, re.M)}


def _quick_reference() -> dict[str, str]:
    """Section 8.2's two-pairs-per-row table of the tuning values worth knowing."""
    text = REFERENCE.read_text()
    section = text.split("## 8.2 Curriculum tuning defaults", 1)[1].split("## 8.3", 1)[0]
    return {m.group(1): m.group(2).strip().strip('`"')
            for m in re.finditer(r"`([A-Z][\w.]*)`\s*\|\s*([^|]+?)\s*(?=\||$)", section, re.M)}


def test_the_quick_reference_only_lists_keys_that_exist():
    """test_conf_covers_tuning checks the template against the sim. This checks the manual against the template,
    which closes the loop: a key renamed in the code fails there, and a stale row here fails now."""
    unknown = sorted(set(_quick_reference()) - set(_template_defaults()))
    assert unknown == [], f"08-reference.md lists tuning keys the template does not define: {unknown}"


def test_the_quick_reference_defaults_are_the_templates():
    """Five of these were wrong when the table was last checked by hand -- Duel.Stall and Pulls.Stall at 0.05
    against a configured 0.08, both PreparationRefundMaxMs at 30000 against 15000, Travel.FastArrive at 3.0
    against 6.0. Numbers transcribed into prose drift; this is why the table is checked."""
    template = _template_defaults()
    wrong = {k: (v, template[k]) for k, v in _quick_reference().items() if k in template and v != template[k]}
    assert wrong == {}, f"08-reference.md disagrees with the template (manual, template): {wrong}"
