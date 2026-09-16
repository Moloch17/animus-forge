"""The curriculum's standard talent builds, checked as data: every spec must reach its own last row.

These live in animus-lib (`Character/SpecBuilds.cpp`) and are spent by `TalentBuilder::Standard`, which places
them in list order under the game's own rules. The rules that matter here are arithmetic:

- a level 80 character has 71 talent points, and
- a tree's last row (row 10 in Wrath) needs 50 points already spent in that tree.

So a build whose own tree holds 50 or fewer points cannot take the ability its spec is built around -- Titan's
Grip, Shadow Dance, Starfall, Riptide. A cap that held the spec tree to 51 points did exactly that to all 31
builds, because list order spends the 51st point long before the last row unlocks.
"""

import re
from pathlib import Path

import pytest

LEVEL_80_POINTS = 71
LAST_ROW_NEEDS = 50  # 5 points per row, rows 0-10

RELATIVE = Path("src/Scenario/Curriculum/Character/SpecBuilds.cpp")


def spec_builds_path() -> Path | None:
    """animus-lib beside this checkout, wherever the two were cloned (or mounted, under test)."""
    for base in Path(__file__).resolve().parents:
        for candidate in (base / "mod-animus-lib" / RELATIVE, base / "modules/mod-animus-lib" / RELATIVE):
            if candidate.exists():
                return candidate
    return None

BUILD = re.compile(r'\{\s*CLASS_(\w+),\s*"(\w+)",\s*\{(.*?)\n\s*\},', re.S)
PICK = re.compile(r'\{\s*(\d+),\s*"([^"]+)",\s*(\d+)\s*\}')


def specs():
    path = spec_builds_path()
    if path is None:
        pytest.skip("animus-lib is not beside this checkout")

    source = path.read_text()
    found = []
    for match in BUILD.finditer(source):
        player_class, spec, body = match.group(1).lower(), match.group(2), match.group(3)
        trees: dict[int, int] = {}
        for tab, _name, ranks in PICK.findall(body):
            trees[int(tab)] = trees.get(int(tab), 0) + int(ranks)
        found.append((f"{player_class}/{spec}", trees))

    assert found, "no spec builds parsed: has SpecBuilds.cpp changed shape?"
    return found


def test_every_spec_spends_all_seventy_one_points():
    for name, trees in specs():
        assert sum(trees.values()) == LEVEL_80_POINTS, f"{name} spends {sum(trees.values())}"


def test_every_spec_reaches_its_own_last_row():
    """The main tree must hold more than the 50 points the last row needs, or the capstone is unreachable."""
    for name, trees in specs():
        main = max(trees.values())
        assert main > LAST_ROW_NEEDS, (
            f"{name} puts only {main} points in its own tree; the last row needs {LAST_ROW_NEEDS} spent before "
            f"it can be taken, so the spec's defining talent would never be picked")


def test_no_support_tree_rivals_the_spec_tree():
    """A support tree may take a real share of the points -- three-tree builds are ordinary in Wrath, 57/9/5 as
    readily as 53/18 -- but none of them may grow into a second main tree, which no player's build has."""
    for name, trees in specs():
        main = max(trees.values())
        support = sorted((points for points in trees.values() if points), reverse=True)[1:]
        assert all(points < main for points in support), f"{name} has two trees of {main}"
        assert all(points <= 20 for points in support), f"{name} puts {max(support)} points in a support tree"
