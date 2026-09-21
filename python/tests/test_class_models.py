"""One model per class, but eighteen things still being measured.

The merge joined a class's roles into one policy. What it must not join is the bookkeeping: a paladin that tanks
well and heals badly has to be sampled, weighted and reported as two things, or the healing disappears into the
class average and nothing ever asks for more of it.
"""

import numpy as np
import pytest

from animus.evaluation import ROLES, EvalResult, casting_weights


def result(layouts, roles, scores):
    """An eval result whose episode info carries the role column, as the sim reports it."""
    return EvalResult(policy="learner", arenas=("duel",), layouts=list(layouts),
                      returns=np.array(scores, dtype=np.float32),
                      infos=np.array([[float(ROLES.index(r))] for r in roles], dtype=np.float32),
                      info_names=("role",))


def test_a_class_is_summarised_per_role_as_well_as_whole():
    summary = result(["paladin"] * 4, ["tank", "tank", "heal", "heal"], [9.0, 9.0, 1.0, 1.0]).summary(())

    # The class average says nothing useful: it is exactly between a good tank and a bad healer.
    assert summary["castings"]["paladin_tank"]["score"] == pytest.approx(9.0)
    assert summary["castings"]["paladin_heal"]["score"] == pytest.approx(1.0)
    assert set(summary["castings"]) == {"paladin_tank", "paladin_heal"}


def test_the_weights_ask_for_more_of_the_role_that_is_failing():
    summary = {"castings": {"paladin_tank": {"score": 9.0}, "paladin_heal": {"score": 1.0},
                            "mage_dps": {"score": 5.0}}}
    baseline = {"castings": {"paladin_tank": {"score": 5.0}, "paladin_heal": {"score": 5.0},
                             "mage_dps": {"score": 5.0}}}
    weights = casting_weights(summary, baseline, strength=1.0, max_ratio=4.0)

    # The healer gets the data, not the paladin: the tank is not asked to train harder for the healer's sake.
    assert weights["paladin_heal"] > weights["mage_dps"] > weights["paladin_tank"]
    assert np.mean(list(weights.values())) == pytest.approx(1.0)


def test_the_wire_vector_is_layout_major_with_a_slot_per_role():
    """The sim indexes it as layout * 3 + role, so every class carries three slots whether or not it plays them."""
    weights = {"paladin_heal": 2.0, "paladin_tank": 0.5, "mage_dps": 1.0}
    layouts = ["paladin", "mage"]
    vector = [weights.get(f"{name}_{role}", 1.0) for name in layouts for role in ROLES]

    assert len(vector) == len(layouts) * 3
    assert vector[0 * 3 + ROLES.index("heal")] == 2.0
    assert vector[0 * 3 + ROLES.index("tank")] == 0.5
    assert vector[1 * 3 + ROLES.index("dps")] == 1.0
    # A mage cannot tank; the slot exists, is never drawn, and stays at the even weight.
    assert vector[1 * 3 + ROLES.index("tank")] == 1.0


def test_a_class_with_one_role_is_unchanged():
    summary = result(["mage"] * 3, ["dps"] * 3, [4.0, 5.0, 6.0]).summary(())
    assert set(summary["castings"]) == {"mage_dps"}
    assert summary["castings"]["mage_dps"]["score"] == pytest.approx(5.0)
