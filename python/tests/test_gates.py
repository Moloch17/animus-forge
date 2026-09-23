"""Stage target gates and their startup validation."""

import re
from pathlib import Path

import pytest

from animus.config import TargetConfig, TrainConfig
from animus.evaluation import DERIVED_METRICS
from animus.gates import check_gates, required_score, validate_target


def summary(score, layouts=None, **metrics):
    return {"score": score, "episodes": 100, "layouts": layouts or {}, **metrics}


def layout(score, episodes=32, stderr=0.0):
    return {"score": score, "episodes": episodes, "stderr": stderr}


def test_required_score_is_sign_safe():
    assert required_score(10.0, 0.2) == pytest.approx(12.0)
    assert required_score(-10.0, 0.2) == pytest.approx(-8.0)  # 20% better than a negative baseline is higher
    assert required_score(-10.0, 0.0) == pytest.approx(-10.0)


def test_overall_baseline_gate():
    target = TargetConfig(min_over_baseline=0.2)
    assert check_gates(summary(12.5), summary(10.0), target).passed
    report = check_gates(summary(11.0), summary(10.0), target)
    assert not report.passed and report.failures == ["score 11 (needs 12)"]
    assert check_gates(summary(-7.0), summary(-10.0), target).passed
    assert not check_gates(summary(12.5), None, target).passed  # no baseline to compare with


def test_layout_floor_skips_thin_layouts():
    target = TargetConfig(min_layout_over_baseline=0.0, min_layout_episodes=16)
    learner = summary(5.0, {"warrior_dps": layout(9.0), "priest_heal": layout(2.0), "mage_dps": layout(0.0, 4)})
    base = summary(4.0, {"warrior_dps": layout(6.0), "priest_heal": layout(3.0), "mage_dps": layout(5.0)})
    report = check_gates(learner, base, target)
    assert not report.passed
    assert report.failures == ["priest_heal score 2 (needs 3)"]
    assert report.skipped == ["mage_dps: 4 episodes"]

    learner["layouts"]["priest_heal"] = layout(3.5)
    assert check_gates(learner, base, target).passed


def test_metric_gates():
    target = TargetConfig(metrics={"killed": {"min": 0.9}, "died": {"max": 0.1}})
    assert check_gates(summary(1.0, killed=0.95, died=0.05), None, target).passed
    report = check_gates(summary(1.0, killed=0.8, died=0.2), None, target)
    assert len(report.failures) == 2
    assert not check_gates(summary(1.0, killed=0.95), None, target).passed  # died missing
    assert not check_gates(None, None, target).passed


def test_arena_gates():
    target = TargetConfig(arenas={"duel": {"min_over_baseline": 0.1}, "pvp": {"metrics": {"won": {"min": 0.5}}}},
                          min_arena_episodes=16)
    learner = summary(5.0, arenas={"duel": {"score": 12.0, "episodes": 40}, "pvp": {"score": 1.0, "episodes": 40,
                                                                                    "won": 0.6}})
    base = summary(4.0, arenas={"duel": {"score": 10.0, "episodes": 40}, "pvp": {"score": 0.5, "episodes": 40}})
    assert check_gates(learner, base, target).passed

    learner["arenas"]["duel"]["score"] = 10.5
    learner["arenas"]["pvp"]["won"] = 0.4
    report = check_gates(learner, base, target)
    assert report.failures == ["arena duel score 10.5 (needs 11)", "arena pvp won (min) 0.4 (needs 0.5)"]

    learner["arenas"]["duel"]["episodes"] = 8
    report = check_gates(learner, base, target)
    assert report.skipped == ["arena duel: 8 episodes"]

    del learner["arenas"]["pvp"]
    assert "arena pvp: not in the evaluation summary" in check_gates(learner, base, target).failures
    assert "arena duel: no baseline score to compare with" not in check_gates(learner, None, target).failures


def test_difficulty_gates():
    """Tier keys come as YAML numbers; a stage without tiers is all tier 0."""
    target = TargetConfig(difficulties={0: {"metrics": {"clean_kill": {"min": 0.9}}}}, min_arena_episodes=16)
    assert list(target.difficulties) == ["0"]
    learner = summary(5.0, difficulties={"0": {"score": 12.0, "episodes": 40, "clean_kill": 0.95},
                                         "3": {"score": 2.0, "episodes": 40, "clean_kill": 0.4}})
    assert check_gates(learner, None, target).passed

    learner["difficulties"]["0"]["clean_kill"] = 0.8
    assert check_gates(learner, None, target).failures == ["tier 0 clean_kill (min) 0.8 (needs 0.9)"]

    flat = summary(5.0, clean_kill=0.95)
    assert check_gates(flat, None, target).passed
    assert not check_gates(summary(5.0, clean_kill=0.8), None, target).passed
    missing = TargetConfig(difficulties={2: {"metrics": {"clean_kill": {"min": 0.9}}}})
    assert "tier 2: not in the evaluation summary" in check_gates(flat, None, missing).failures


def test_base_difficulty_judges_the_floors_on_the_easier_tiers():
    """metrics and layout_metrics on the up_to group; a summary without one is judged whole."""
    target = TargetConfig(metrics={"clean_kill": {"min": 0.85}}, layout_metrics={"clean_kill": {"min": 0.75}},
                          base_difficulty=2)
    base = {"score": 9.0, "episodes": 60, "clean_kill": 0.9,
            "layouts": {"rogue_dps": {"score": 9.0, "episodes": 30, "clean_kill": 0.8}}}
    learner = summary(7.0, layouts={"rogue_dps": {"score": 6.0, "episodes": 60, "clean_kill": 0.6}}, clean_kill=0.7,
                      up_to={"2": base})
    assert check_gates(learner, None, target).passed  # the elite tiers above 2 pull the whole summary down

    base["layouts"]["rogue_dps"]["clean_kill"] = 0.7
    assert check_gates(learner, None, target).failures == ["tiers 0-2 rogue_dps clean_kill (min) 0.7 (needs 0.75)"]

    flat = summary(7.0, layouts={"rogue_dps": {"score": 6.0, "episodes": 60, "clean_kill": 0.9}}, clean_kill=0.9)
    assert check_gates(flat, None, target).passed


def test_spec_metrics_judge_each_build_on_its_own_episodes():
    target = TargetConfig(spec_metrics={"restoration": {"owner_heal_share": {"min": 0.3}},
                                        "feral_bear": {"threat_share": {"min": 0.5}}})
    learner = summary(5.0, specs={"restoration": {"score": 4.0, "episodes": 40, "owner_heal_share": 0.4},
                                  "feral_bear": {"score": 6.0, "episodes": 40, "threat_share": 0.3},
                                  "feral_cat": {"score": 5.0, "episodes": 40, "threat_share": 0.1}})
    assert check_gates(learner, None, target).failures == ["build feral_bear threat_share (min) 0.3 (needs 0.5)"]
    assert "build restoration: no episodes" in check_gates(summary(5.0), None, target).skipped


def test_validate_spec_metrics_checks_the_metric_not_the_build_name():
    """A build name cannot be validated against a fixed list: the names are the classes' own, and which of them a
    run has depends on AnimusForge.Classes. The metric still is."""
    config = TrainConfig()
    config.eval.every_env_steps = 10
    config.target.spec_metrics = {"restoration": {"not_a_column": {"min": 0.3}}}
    with pytest.raises(ValueError, match="not_a_column"):
        validate_target(config, ("owner_heal_share",))

    config.target.spec_metrics = {"a_build_nothing_plays": {"owner_heal_share": {"min": 0.3}}}
    validate_target(config, ("owner_heal_share",))


def test_validate_base_difficulty():
    config = TrainConfig()
    config.eval.every_env_steps = 10
    config.target.base_difficulty = -1
    with pytest.raises(ValueError, match="base_difficulty"):
        validate_target(config, ())


def test_validate_arena_targets():
    config = TrainConfig()
    config.eval.every_env_steps = 10
    config.target.arenas = {"duel": {"min_over_baseline": 0.1}}
    with pytest.raises(ValueError, match="need eval.baseline"):
        validate_target(config, ("won",), ("duel", "pvp"))
    config.eval.baseline = "fight"
    validate_target(config, ("won",), ("duel", "pvp"))
    validate_target(config, ("won",))  # no stage.json: arena names are not checked
    with pytest.raises(ValueError, match="no such arena"):
        validate_target(config, ("won",), ("pvp",))

    # (`lost` was the example here until it became a derived metric the validator allows by name.)
    config.target.arenas = {"pvp": {"metrics": {"no_such_column": {"min": 0.1}}}}
    with pytest.raises(ValueError, match="no such episode info"):
        validate_target(config, ("won",), ("pvp",))
    config.target.arenas = {"pvp": {"min_over": 0.1}}
    with pytest.raises(ValueError, match="expected min_over_baseline"):
        validate_target(config, ("won",), ("pvp",))

    config.target.arenas = {}
    config.eval.baseline = ""
    config.eval.opponent_baseline = True
    with pytest.raises(ValueError, match="opponent_baseline needs eval.baseline"):
        validate_target(config, ("won",))


def test_validate_target():
    config = TrainConfig()
    validate_target(config, ("dps",))  # no target: nothing to check

    config.target.metrics = {"killed": {"min": 0.9}}
    with pytest.raises(ValueError, match="eval.every_env_steps"):
        validate_target(config, ("killed",))
    config.eval.every_env_steps = 10
    validate_target(config, ("killed",))
    with pytest.raises(ValueError, match="no such episode info"):
        validate_target(config, ("dps",))

    config.target.metrics = {"killed": {"at_least": 0.9}}
    with pytest.raises(ValueError, match="expected"):
        validate_target(config, ("killed",))

    config.target.metrics = {}
    config.target.min_layout_over_baseline = 0.0
    with pytest.raises(ValueError, match="need eval.baseline"):
        validate_target(config, ("killed",))
    config.eval.baseline = "fight"
    validate_target(config, ("killed",))


def sim_episode_info() -> tuple[str, ...]:
    """Every episode info column the sim can emit, read from the bundled animus-lib.

    This used to be `tuple(config.target.metrics)` -- the config's own top-level metric names -- which made the
    check very nearly tautological: a name was validated against itself, and any name under spec_metrics,
    layout_metrics or arenas failed simply for not being repeated at the top level. At run time
    validate_target is handed the scenario's real column list, so the honest stand-in here is that list, taken
    from the source that registers it. A config that gates on something the sim never emits now fails here
    instead of five hours into a queue."""
    root = Path(__file__).resolve().parents[2] / "animus-lib" / "src" / "Scenario" / "Curriculum"
    names: set[str] = set()
    for source in root.rglob("*.cpp"):
        names |= set(re.findall(r'Add\("([a-z0-9_]+)"', source.read_text()))
    assert names, f"no episode info registrations found under {root}"
    return tuple(names | set(DERIVED_METRICS))


def sim_stage_arenas() -> dict[str, tuple[str, ...]]:
    """Each stage's arena names, read from the curriculum that defines them.

    validate_target only checks `target.arenas` when it is given the stage's arena list, and the shipped-config
    test used to call it without one. An arena gate naming an arena the stage does not have therefore passed the
    suite and failed at startup instead -- stage3_travel inherited `open`, `broken` and `water` from stage1_move
    and took the queue down with it. Parsed from the definitions rather than from stage.json, which only exists
    for stages that have already been run."""
    source = (Path(__file__).resolve().parents[2] / "animus-lib" / "src" / "Scenario" / "Curriculum"
              / "Stages" / "Stages.cpp").read_text()
    stages: dict[str, tuple[str, ...]] = {}
    for body in re.findall(r"stages\.push_back\(\{(.*?)\n        \}\);", source, re.S):
        name = re.search(r'\.Name = "([^"]+)"', body)
        if not name:
            continue
        stages[name.group(1)] = tuple(re.findall(r'\{\s*\.Name = "([a-z0-9_]+)"', body[name.end():]))
    assert stages, "no stage definitions found"
    return stages


def sim_stage_columns() -> dict[str, set[str]]:
    """Per stage, the episode info columns it can actually emit.

    An encounter is built only when an arena asks for it (StageScenario's constructor), and it registers its
    columns only when it is built: a stage whose arenas are all Opposition::Pulls has no OwnerEncounter and so
    never emits owner_deaths. Gates accumulate down the config chain, so a stage that drops a capability keeps
    its parent's gate on it -- the raid stages inherited owner_deaths from the party line and would have failed
    it as "not in the evaluation summary" every evaluation, which under until_passed halts the queue for good.
    Checking names against the union of every column any stage can emit does not catch that; this does."""
    root = Path(__file__).resolve().parents[2] / "animus-lib" / "src" / "Scenario" / "Curriculum"

    def columns(relative: str) -> set[str]:
        path = root / relative
        return set(re.findall(r'Add\("([a-z0-9_]+)"', path.read_text())) if path.exists() else set()

    per_encounter = {name: columns(f"Encounters/{cls}.cpp") for name, cls in (
        ("opponent", "OpponentEncounter"), ("owner", "OwnerEncounter"), ("party", "PartyEncounter"),
        ("pulls", "PullsEncounter"), ("creature", "CreatureEncounter"), ("hazards", "HazardEncounter"),
        ("ambush", "AmbushEncounter"), ("travel", "TravelEncounter"), ("flag", "FlagEncounter"),
        ("director", "DirectorEncounter"))}
    always = columns("StageScenario.cpp") | set(DERIVED_METRICS)
    for reward in (root / "Rewards").glob("*.cpp"):
        always |= set(re.findall(r'Add\("([a-z0-9_]+)"', reward.read_text()))

    source = (root / "Stages" / "Stages.cpp").read_text()
    out: dict[str, set[str]] = {}
    for body in re.findall(r"stages\.push_back\(\{(.*?)\n        \}\);", source, re.S):
        name = re.search(r'\.Name = "([^"]+)"', body)
        if not name:
            continue
        active: set[str] = set()
        for arena in re.findall(r'\{\s*\.Name = "[a-z0-9_]+"(.*?)\}', body[name.end():], re.S):
            against = (re.search(r"\.Against = Opposition::(\w+)", arena) or [None, "Creature"])[1]
            if against in ("ScriptedPlayer", "MirrorSeat", "Flag"):
                active.add("opponent")
            for opposition, encounter in (("Pulls", "pulls"), ("Creature", "creature"), ("Hazards", "hazards"),
                                          ("Travel", "travel"), ("Flag", "flag")):
                if against == opposition:
                    active.add(encounter)
            if ".Owner = true" in arena:
                active.add("owner")
            if ".PartyGroup = true" in arena:
                active.add("party")
            if ".Directed = true" in arena:
                active.add("director")
            if re.search(r"\.Ambushers = [1-9]", arena):
                active.add("ambush")
        out[name.group(1)] = set(always).union(*(per_encounter[e] for e in active)) if active else set(always)
    assert out, "no stage definitions found"
    return out


def gated_names(target) -> dict[str, str]:
    """Every metric name a target gates on, and where it is gated."""
    found: dict[str, str] = {}
    for name in target.metrics or {}:
        found.setdefault(name, "target.metrics")
    for name in target.layout_metrics or {}:
        found.setdefault(name, "target.layout_metrics")
    for spec, bounds in (target.spec_metrics or {}).items():
        for name in bounds or {}:
            found.setdefault(name, f"target.spec_metrics.{spec}")
    for arena, gates in (target.arenas or {}).items():
        for name in (gates or {}).get("metrics") or {}:
            found.setdefault(name, f"target.arenas.{arena}")
    for tier, gates in (target.difficulties or {}).items():
        for name in (gates or {}).get("metrics") or {}:
            found.setdefault(name, f"target.difficulties.{tier}")
    return found


MOVEMENT_STAGES = ("stage1_move", "stage1b_indoor", "stage2_dodge", "stage3_travel", "stage4_flight")


@pytest.mark.parametrize("name", MOVEMENT_STAGES)
def test_movement_stages_gate_on_no_difficulty_tier(name):
    """The movement stages extend stage5_duel's config and inherited its tier-0 gate, clean_kill >= 0.95. Nothing
    is killed on a trip, so clean_kill is 0 there by construction; a stage with no difficulty tiers is judged for
    tier 0 on its whole summary (gates.py); and every movement stage failed its gate at every evaluation --
    stage1_move/stage.jsonl: "tier 0 clean_kill (min, 95% lower bound) 0 (needs 0.95)". The emission check above
    cannot catch it, because killed and died are columns every stage emits. An empty map is the only way to drop an
    inherited key, so each of these has to say `difficulties: {}` itself."""
    config = TrainConfig.load(Path(__file__).parent.parent / "configs" / f"{name}.yaml")
    assert config.target.difficulties == {}, f"{name} inherits a difficulty-tier gate it can never pass"


@pytest.mark.parametrize("path", sorted((Path(__file__).parent.parent / "configs").glob("*.yaml")), ids=str)
def test_shipped_configs_gate_only_on_columns_their_stage_emits(path):
    config = TrainConfig.load(path)
    emitted = sim_stage_columns().get(config.run_name or path.stem)
    if emitted is None:
        return
    unreachable = {n: w for n, w in gated_names(config.target).items() if n not in emitted}
    assert not unreachable, (f"{path.name} gates on columns its stage never emits: "
                             + ", ".join(f"{w}.{n}" for n, w in sorted(unreachable.items())))


@pytest.mark.parametrize("path", sorted((Path(__file__).parent.parent / "configs").glob("*.yaml")), ids=str)
def test_shipped_configs_load_and_validate(path):
    config = TrainConfig.load(path)
    arenas = sim_stage_arenas().get(config.run_name or path.stem)
    validate_target(config, sim_episode_info(), arenas)


def test_score_gate_allows_evaluation_noise():
    # A layout half a point below its baseline, measured to +/- 0.5: within a standard error of the difference.
    target = TargetConfig(min_layout_over_baseline=0.0, noise_z=1.0)
    learner = summary(5.0, {"rogue_dps": layout(7.5, stderr=0.5)})
    base = summary(5.0, {"rogue_dps": layout(8.0, stderr=0.2)})
    assert check_gates(learner, base, target).passed

    # Same gap, a well-measured score: now it is a real difference.
    sharp_learner = summary(5.0, {"rogue_dps": layout(7.5, stderr=0.05)})
    sharp_base = summary(5.0, {"rogue_dps": layout(8.0, stderr=0.05)})
    report = check_gates(sharp_learner, sharp_base, target)
    assert not report.passed and report.failures == ["rogue_dps score 7.5 (needs 8)"]

    # And noise_z 0 compares the raw means, whatever the standard errors say.
    assert not check_gates(learner, base, TargetConfig(min_layout_over_baseline=0.0, noise_z=0.0)).passed


def test_noise_allowance_is_validated():
    config = TrainConfig()
    config.eval.every_env_steps = 1
    config.eval.baseline = "fight"
    config.target.min_over_baseline = 0.1
    config.target.noise_z = -1.0
    with pytest.raises(ValueError, match="noise_z"):
        validate_target(config, ("killed",))


def test_layout_metrics_floor_is_absolute():
    """A layout that beats its own baseline can still be bad: where the scripted baseline is hopeless, beating it
    asks for nothing (stage5_duel passed warlock_dps against a required score of -2.34)."""
    target = TargetConfig(min_layout_over_baseline=0.0, layout_metrics={"killed": {"min": 0.75}})
    learner = summary(5.0, {"warlock_dps": {**layout(4.7), "killed": 0.65},
                            "warrior_dps": {**layout(7.9), "killed": 0.89}})
    base = summary(4.0, {"warlock_dps": layout(-2.3), "warrior_dps": layout(6.8)})

    report = check_gates(learner, base, target)
    assert not report.passed
    assert report.failures == ["warlock_dps killed (min) 0.65 (needs 0.75)"]  # both score gates passed

    learner["layouts"]["warlock_dps"]["killed"] = 0.8
    assert check_gates(learner, base, target).passed


def test_layout_metrics_skip_thin_layouts():
    target = TargetConfig(layout_metrics={"killed": {"min": 0.75}}, min_layout_episodes=16)
    learner = summary(5.0, {"mage_dps": {**layout(1.0, 4), "killed": 0.0}})
    report = check_gates(learner, None, target)
    assert report.passed
    assert report.skipped == ["mage_dps: 4 episodes"]


def test_livelocked_is_gateable_though_it_is_not_episode_info():
    """The share of episodes stuck in a cast/stop loop is derived by the summary, not averaged from an info
    column, so validation has to allow it by name."""
    config = TrainConfig()
    config.eval.every_env_steps = 1
    config.target.metrics = {"livelocked": {"max": 0.01}}
    config.target.layout_metrics = {"livelocked": {"max": 0.05}}
    validate_target(config, ("killed", "died"))

    target = TargetConfig(layout_metrics={"livelocked": {"max": 0.05}})
    learner = summary(5.0, {"warlock_dps": {**layout(4.7), "livelocked": 0.248}})
    assert not check_gates(learner, None, target).passed


def test_clean_kill_every_fight_is_gateable():
    config = TrainConfig()
    config.eval.every_env_steps = 1
    config.target.metrics = {"clean_kill": {"min": 1.0}}
    config.target.layout_metrics = {"clean_kill": {"min": 1.0}}
    config.layout_sampling.enabled = True
    config.layout_sampling.metric = "clean_kill"
    validate_target(config, ("killed", "died"))

    target = TargetConfig(metrics={"clean_kill": {"min": 1.0}}, layout_metrics={"clean_kill": {"min": 1.0}})
    assert check_gates(summary(5.0, {"mage_dps": {**layout(5.0), "clean_kill": 1.0}}, clean_kill=1.0), None,
                       target).passed
    report = check_gates(summary(5.0, {"mage_dps": {**layout(5.0), "clean_kill": 0.98}}, clean_kill=0.999), None,
                         target)
    assert not report.passed and len(report.failures) == 2  # one lost fight anywhere fails


def test_layout_sampling_metric_must_exist():
    config = TrainConfig()
    config.eval.every_env_steps = 1
    config.layout_sampling.enabled = True
    config.layout_sampling.metric = "clean_kills"
    with pytest.raises(ValueError, match="layout_sampling.metric"):
        validate_target(config, ("killed", "died"))


def test_a_confidence_bound_needs_enough_episodes():
    """With confidence, a share is judged by its Wilson bound over the row's episodes: 57 wins of 57 clears 0.90,
    two losses do not, and a perfect handful proves nothing."""
    from animus.gates import wilson_bound

    target = TargetConfig(layout_metrics={"clean_kill": {"min": 0.90, "confidence": 0.95}})
    row = lambda share, episodes: summary(5.0, {"mage_dps": {**layout(5.0, episodes), "clean_kill": share}})
    assert check_gates(row(1.0, 57), None, target).passed
    assert check_gates(row(56 / 57, 57), None, target).passed
    report = check_gates(row(55 / 57, 57), None, target)
    assert not report.passed and "lower bound" in report.failures[0]
    assert not check_gates(row(1.0, 16), None, target).passed  # 16 of 16: bound 0.86
    assert check_gates(row(0.95, 228), None, target).passed

    assert wilson_bound(0.95, 228, 0.95, lower=True) == pytest.approx(0.9205, abs=1e-3)
    assert wilson_bound(0.0, 0, 0.95, lower=True) == 0.0 and wilson_bound(0.0, 0, 0.95, lower=False) == 1.0
    # A maximum is judged by the upper bound: 1 livelock in 57 episodes may still be a 7% rate.
    upper = TargetConfig(layout_metrics={"livelocked": {"max": 0.05, "confidence": 0.95}})
    assert not check_gates(summary(5.0, {"a": {**layout(5.0, 57), "livelocked": 1 / 57}}), None, upper).passed


def test_confidence_must_be_a_probability_beside_a_bound():
    config = TrainConfig()
    config.eval.every_env_steps = 1
    config.target.metrics = {"clean_kill": {"min": 0.9, "confidence": 1.5}}
    with pytest.raises(ValueError, match="confidence"):
        validate_target(config, ("killed", "died"))
    config.target.metrics = {"clean_kill": {"confidence": 0.9}}
    with pytest.raises(ValueError, match="clean_kill"):
        validate_target(config, ("killed", "died"))
    config.target.metrics = {"clean_kill": {"min": 0.9, "confidence": 0.95}}
    validate_target(config, ("killed", "died"))


def stage_definitions() -> dict[str, dict]:
    """Each stage's Extends, Merges and Blocks, in queue order."""
    source = (Path(__file__).resolve().parents[2] / "animus-lib" / "src" / "Scenario" / "Curriculum"
              / "Stages" / "Stages.cpp").read_text()
    out: dict[str, dict] = {}
    for body in re.findall(r"stages\.push_back\(\{(.*?)\n        \}\);", source, re.S):
        name = re.search(r'\.Name = "([^"]+)"', body)
        if not name:
            continue
        def field(pattern: str, default: str = "") -> str:
            found = re.search(pattern, body)
            return found.group(1) if found else default
        out[name.group(1)] = dict(
            extends=field(r'\.Extends = "([^"]*)"'),
            merges=re.findall(r'"(stage[0-9a-z_]+)"', field(r"\.Merges = \{([^}]*)\}")),
            blocks={b.strip() for b in field(r"\.Blocks = \{([^}]*)\}").split(",") if b.strip()},
        )
    assert out, "no stage definitions found"
    return out


def test_no_stage_relearns_a_block_an_earlier_stage_already_trained():
    """A block the parent does not have starts from zero (animus.bootstrap), and a merge is the only way back.

    The curriculum branches: the PvE line trains pack, gauntlet and support, the PvP line drops all three, and a
    stage that rejoins the PvE line by extending the PvP one would start them again from nothing -- three stages
    of training spent twice. Every block should be introduced by exactly one stage and carried by extension or
    merge from then on."""
    stages = stage_definitions()
    order = list(stages)
    relearned = {}
    for index, name in enumerate(order):
        stage = stages[name]
        inherited: set[str] = set()
        for ancestor in [stage["extends"], *stage["merges"]]:
            if ancestor in stages:
                inherited |= stages[ancestor]["blocks"]
        trained_before: set[str] = set()
        for earlier in order[:index]:
            trained_before |= stages[earlier]["blocks"]
        lost = (stage["blocks"] - inherited) & trained_before
        if lost:
            relearned[name] = sorted(lost)
    assert not relearned, ("these stages start a block from zero that an earlier stage already trained; merge "
                           f"the stage that trained it: {relearned}")
