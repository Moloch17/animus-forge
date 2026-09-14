import os

import numpy as np
import pytest
import torch

from animus.export import actor_layers, model_dirs_from_env, publish_model, read_amdl, reference_decide, write_amdl
from animus.mappo.networks import Actor


@pytest.mark.parametrize("num_agents", [1, 3])
def test_amdl_round_trip_matches_torch_actor(tmp_path, num_agents):
    torch.manual_seed(0)
    obs_dim, num_actions = 9, 3
    actor = Actor(obs_dim, num_actions, num_agents, [16, 8])
    # Orthogonal init with gain 0.01 makes every logit near zero; widen them so argmax is meaningful.
    with torch.no_grad():
        actor.net[-1].weight.mul_(100.0)

    path = tmp_path / "actor.amdl"
    write_amdl(path, "warrior_dummy", obs_dim, num_agents, num_actions, actor_layers(actor.state_dict()))
    model = read_amdl(path)

    assert model["scenario"] == "warrior_dummy"
    assert (model["obs_dim"], model["num_agents"], model["num_actions"]) == (obs_dim, num_agents, num_actions)
    assert len(model["layers"]) == 3

    rng = np.random.default_rng(1)
    for _ in range(64):
        obs = rng.standard_normal(obs_dim).astype(np.float32)
        mask = rng.integers(0, 2, num_actions).astype(np.uint8)
        mask[0] = 1
        agent = int(rng.integers(0, num_agents))

        action, logits = reference_decide(model, obs, mask, agent)

        dist = actor(torch.from_numpy(obs)[None], torch.tensor([agent]), torch.from_numpy(mask)[None])
        torch_logits = actor.net(
            torch.cat([torch.from_numpy(obs), torch.eye(num_agents)[agent]])
        ).detach().numpy()

        np.testing.assert_allclose(logits, torch_logits, rtol=1e-5, atol=1e-5)
        assert action == int(dist.probs.argmax(dim=-1))


def test_empty_mask_falls_back_to_action_zero(tmp_path):
    actor = Actor(4, 3, 1, [8])
    path = tmp_path / "actor.amdl"
    write_amdl(path, "s", 4, 1, 3, actor_layers(actor.state_dict()))
    action, _ = reference_decide(read_amdl(path), np.ones(4, np.float32), np.zeros(3, np.uint8))
    assert action == 0


def test_write_rejects_mismatched_dims(tmp_path):
    actor = Actor(4, 3, 1, [8])
    with pytest.raises(ValueError):
        write_amdl(tmp_path / "bad.amdl", "s", 5, 1, 3, actor_layers(actor.state_dict()))


def test_publish_writes_every_existing_dir_and_skips_missing_ones(tmp_path):
    actor = Actor(4, 3, 1, [8])
    spec = {"scenario": "s", "obs_dim": 4, "agents_per_env": 1, "num_actions": 3}
    first, second = tmp_path / "a", tmp_path / "b"
    first.mkdir()
    second.mkdir()
    (second / "s.amdl").write_bytes(b"stale")

    written = publish_model(actor.state_dict(), spec, [first, tmp_path / "missing", second])

    assert written == [first / "s.amdl", second / "s.amdl"]
    for path in written:
        assert read_amdl(path)["obs_dim"] == 4
    assert not (tmp_path / "missing").exists()
    assert sorted(p.name for p in second.iterdir()) == ["s.amdl"]


def test_model_dirs_from_env(monkeypatch):
    monkeypatch.setenv("ANIMUS_MODEL_DIRS", f"/x{os.pathsep}{os.pathsep}/y")
    assert model_dirs_from_env() == ["/x", "/y"]
    monkeypatch.delenv("ANIMUS_MODEL_DIRS")
    assert model_dirs_from_env() == []
