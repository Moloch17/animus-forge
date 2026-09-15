"""Config loading rejects values of the wrong type at load time."""

import pytest

from animus.config import TrainConfig, from_dict


def test_numbers_bools_and_unions_load():
    config = from_dict(TrainConfig, {
        "total_env_steps": 1000,
        "init_from": ["a.pt", "b.pt"],
        "mappo": {"hidden": [64, 64], "entropy_coef": 1},
        "target": {"min_over_baseline": None, "metrics": {"killed": {"min": 0.5}}},
        "eval": {"at_start": False},
    })
    assert config.total_env_steps == 1000
    assert config.init_from == ["a.pt", "b.pt"]
    assert config.mappo.hidden == (64, 64)
    assert config.mappo.entropy_coef == 1.0 and isinstance(config.mappo.entropy_coef, float)
    assert config.eval.at_start is False


@pytest.mark.parametrize("raw, key", [
    ({"total_env_steps": "3e8"}, "total_env_steps"),          # YAML 1.1 reads 3e8 as a string
    ({"rollout_length": True}, "rollout_length"),             # a bool is not a count
    ({"mappo": {"gamma": "0.99"}}, "mappo.gamma"),
    ({"mappo": {"hidden": [64, "wide"]}}, "mappo.hidden"),
    ({"eval": {"at_start": "yes please"}}, "eval.at_start"),
])
def test_wrong_types_fail_at_load(raw, key):
    with pytest.raises(ValueError, match=key.replace(".", r"\.")):
        from_dict(TrainConfig, raw)


def test_unknown_keys_still_fail():
    with pytest.raises(ValueError, match="unknown config keys"):
        from_dict(TrainConfig, {"eval": {"evry_env_steps": 10}})
