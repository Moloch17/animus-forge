"""Stage target gates and their startup validation."""

from pathlib import Path

import pytest

from animus.config import TargetConfig, TrainConfig
from animus.gates import check_gates, required_score, validate_target


def summary(score, layouts=None, **metrics):
    return {"score": score, "episodes": 100, "layouts": layouts or {}, **metrics}


def layout(score, episodes=32):
    return {"score": score, "episodes": episodes}


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
