"""Stage target gates and their startup validation."""

from pathlib import Path

import pytest

from animus.config import TargetConfig, TrainConfig
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

    config.target.arenas = {"pvp": {"metrics": {"lost": {"min": 0.1}}}}
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


@pytest.mark.parametrize("path", sorted((Path(__file__).parent.parent / "configs").glob("*.yaml")), ids=str)
def test_shipped_configs_load_and_validate(path):
    config = TrainConfig.load(path)
    validate_target(config, tuple(config.target.metrics))


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
    asks for nothing (stage1_duel passed warlock_dps against a required score of -2.34)."""
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
