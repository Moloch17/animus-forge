import json

import numpy as np
import pytest
import torch

from animus.export import (
    export_layouts,
    layout_layers,
    model_name,
    read_amdl,
    reference_decide,
    with_agent_column,
    write_amdl,
)
from animus.mappo.networks import LayoutActor

LAYOUTS = [(9, 3), (12, 5)]


def spec_for(scenario: str, layouts=LAYOUTS, names=("warrior_dps", "priest_heal")) -> dict:
    return {
        "scenario": scenario,
        "layouts": [{"name": n, "obs_dim": o, "num_actions": a} for n, (o, a) in zip(names, layouts)],
    }


def wide_actor(layouts=LAYOUTS) -> LayoutActor:
    torch.manual_seed(0)
    actor = LayoutActor(layouts, [16, 8])
    # Orthogonal init with gain 0.01 makes every logit near zero; widen them so argmax is meaningful.
    with torch.no_grad():
        for head in actor.heads:
            head.weight.mul_(100.0)
    return actor


def duel_stage_dir(tmp_path):
    """A stage directory as the sim writes it: stage.json naming the duel stage's models."""
    stage = tmp_path / "layouts" / "stage1_duel"
    stage.mkdir(parents=True)
    models = {"warrior_dps": "warrior_dps_duel", "priest_heal": "priest_heal_duel"}
    (stage / "stage.json").write_text(json.dumps({"stage": "stage1_duel", "models": models}))
    return stage


def test_each_layout_exports_as_the_same_mlp(tmp_path):
    actor = wide_actor()
    out = tmp_path / "models"
    out.mkdir()
    written = export_layouts(actor.state_dict(), spec_for("stage1_duel"), out, duel_stage_dir(tmp_path))
    assert [path.name for path in written] == ["warrior_dps_duel.amdl", "priest_heal_duel.amdl"]

    rng = np.random.default_rng(1)
    for index, ((obs_dim, num_actions), path) in enumerate(zip(LAYOUTS, written)):
        model = read_amdl(path)
        assert (model["obs_dim"], model["num_agents"], model["num_actions"]) == (obs_dim, 1, num_actions)
        assert len(model["layers"]) == 3

        max_obs, max_actions = max(o for o, _ in LAYOUTS), max(a for _, a in LAYOUTS)
        for _ in range(32):
            obs = rng.standard_normal(obs_dim).astype(np.float32)
            mask = rng.integers(0, 2, num_actions).astype(np.uint8)
            mask[0] = 1

            action, logits = reference_decide(model, obs, mask)

            padded_obs = np.zeros(max_obs, np.float32)
            padded_obs[:obs_dim] = obs
            padded_mask = np.zeros(max_actions, np.uint8)
            padded_mask[:num_actions] = mask
            dist = actor(torch.from_numpy(padded_obs)[None], torch.tensor([index]), torch.from_numpy(padded_mask)[None])

            torch_logits = dist.logits[0, :num_actions].detach().numpy()
            allowed = mask.astype(bool)
            # Distribution logits are normalised; compare the allowed logits up to that shift.
            shift = logits[allowed][0] - torch_logits[allowed][0]
            np.testing.assert_allclose(logits[allowed], torch_logits[allowed] + shift, rtol=1e-4, atol=1e-4)
            assert action == int(dist.probs.argmax(dim=-1))


def test_model_names():
    duel = {"warrior_dps": "warrior_dps_duel", "druid_heal": "druid_heal_duel"}
    assert model_name("stage1_duel", "warrior_dps", 18, duel) == "warrior_dps_duel"
    assert model_name("stage5_party", "druid_heal", 18, {"druid_heal": "druid_heal_party"}) == "druid_heal_party"
    # Without stage.json: a single-layout scenario keeps its name, others append the layout's.
    assert model_name("warrior_dummy_20", "warrior_dummy_20", 1) == "warrior_dummy_20"
    assert model_name("custom", "mage_dps", 2) == "custom_mage_dps"


def test_empty_mask_falls_back_to_action_zero(tmp_path):
    actor = LayoutActor([(4, 3)], [8])
    path = tmp_path / "actor.amdl"
    write_amdl(path, "s", 4, 1, 3, with_agent_column(layout_layers(actor.state_dict(), 0)))
    action, _ = reference_decide(read_amdl(path), np.ones(4, np.float32), np.zeros(3, np.uint8))
    assert action == 0


def test_write_rejects_mismatched_dims(tmp_path):
    actor = LayoutActor([(4, 3)], [8])
    with pytest.raises(ValueError):
        write_amdl(tmp_path / "bad.amdl", "s", 5, 1, 3, with_agent_column(layout_layers(actor.state_dict(), 0)))


def test_layout_manifests_are_exported_beside_their_models(tmp_path):
    manifests = duel_stage_dir(tmp_path)
    (manifests / "warrior_dps_duel.json").write_text('{"model":"warrior_dps_duel"}\n')
    out = tmp_path / "models"
    out.mkdir()

    export_layouts(wide_actor().state_dict(), spec_for("stage1_duel"), out, manifest_dir=manifests)

    assert (out / "warrior_dps_duel.json").read_text() == '{"model":"warrior_dps_duel"}\n'
    assert not (out / "priest_heal_duel.json").exists()  # no manifest written for it
