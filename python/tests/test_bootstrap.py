from types import SimpleNamespace

import pytest
import torch

from animus.bootstrap import seed_trainer
from animus.config import TrainConfig
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout


def spec(layouts: list[Layout], state_dim: int) -> SimpleNamespace:
    return SimpleNamespace(layouts=tuple(layouts), state_dim=state_dim)


def checkpoint_spec(layouts: list[Layout]) -> dict:
    return {"layouts": [{"name": l.name, "obs_dim": l.obs_dim, "num_actions": l.num_actions} for l in layouts]}


def test_seeded_actor_matches_earlier_stage_on_its_inputs_and_actions():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16))
    old_layouts = [Layout("warrior_dps", 6, 3), Layout("mage_dps", 5, 4)]
    new_layouts = [Layout("mage_dps", 8, 6), Layout("warrior_dps", 9, 5), Layout("priest_heal", 7, 4)]
    old = MappoTrainer([(l.obs_dim, l.num_actions) for l in old_layouts], 4, config)
    new = MappoTrainer([(l.obs_dim, l.num_actions) for l in new_layouts], 11, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec(old_layouts)}

    seeded = seed_trainer(new, checkpoint, spec(new_layouts, 11))
    assert sorted(seeded) == ["mage_dps", "warrior_dps"]

    # warrior_dps is layout 0 before and 1 now. New feature columns start at zero, so whatever they hold does
    # not change the earlier actions' logits (up to the normalising shift of a distribution's logits).
    obs = torch.randn(4, 6)
    new_obs = torch.cat([obs, torch.randn(4, 3)], dim=-1)
    padded_old = torch.cat([obs, torch.zeros(4, 0)], dim=-1)

    old_logits = old.actor(padded_old, torch.zeros(4, dtype=torch.long), torch.ones(4, 4)).logits[:, :3]
    new_logits = new.actor(new_obs, torch.ones(4, dtype=torch.long), torch.ones(4, 6)).logits[:, :3]
    torch.testing.assert_close(new_logits - new_logits[:, :1], old_logits - old_logits[:, :1], rtol=1e-4, atol=1e-4)


def test_critic_state_encoder_and_head_are_not_copied():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8,))
    layouts = [Layout("warrior_dps", 6, 3)]
    old = MappoTrainer([(6, 3)], 4, config)
    new = MappoTrainer([(6, 3)], 4, config)
    fresh_head = new.critic.state_dict()["head.weight"].clone()
    fresh_state = new.critic.state_dict()["state_encoder.weight"].clone()

    seed_trainer(new, {"trainer": old.state_dict(), "spec": checkpoint_spec(layouts)}, spec(layouts, 4))

    critic = new.critic.state_dict()
    torch.testing.assert_close(critic["head.weight"], fresh_head)
    torch.testing.assert_close(critic["state_encoder.weight"], fresh_state)
    torch.testing.assert_close(critic["adapters.0.weight"], old.critic.state_dict()["adapters.0.weight"])


def stage_with(layouts: dict[str, list[tuple[str, int, int]]]) -> dict:
    """A stage.json with block spans: layout name -> [(block, features, actions)], blocks placed in order."""
    entries = {}
    for name, blocks in layouts.items():
        obs = actions = 0
        spans = []
        for block, features, count in blocks:
            spans.append({"name": block, "obs": [obs, features], "actions": [actions, count]})
            obs += features
            actions += count
        entries[name] = {"obs_dim": obs, "num_actions": actions, "blocks": spans}
    return {"layouts": entries}


def test_a_branch_is_seeded_block_by_block():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16))
    # The base (party-like) has core, duel, pack and pvp; the branch keeps core, duel and pvp.
    base = stage_with({"warrior_dps": [("core", 4, 3), ("duel", 3, 2), ("pack", 5, 4), ("pvp", 2, 0)]})
    branch = stage_with({"warrior_dps": [("core", 4, 3), ("duel", 3, 2), ("pvp", 2, 0), ("arena", 1, 1)]})
    old = MappoTrainer([(14, 9)], 4, config)
    new = MappoTrainer([(10, 6)], 4, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("warrior_dps", 14, 9)]),
                  "stage": base}

    fresh_head = new.actor.state_dict()["heads.0.weight"].clone()
    assert seed_trainer(new, checkpoint, spec([Layout("warrior_dps", 10, 6)], 4), branch) == ["warrior_dps"]

    for network, old_network in ((new.actor, old.actor), (new.critic, old.critic)):
        new_w = network.state_dict()["adapters.0.weight"]
        old_w = old_network.state_dict()["adapters.0.weight"]
        torch.testing.assert_close(new_w[:, 0:7], old_w[:, 0:7])       # core and duel stay in place
        torch.testing.assert_close(new_w[:, 7:9], old_w[:, 12:14])     # pvp moves up past the dropped pack
        assert torch.count_nonzero(new_w[:, 9:]) == 0                  # the new block starts at zero

    new_head = new.actor.state_dict()["heads.0.weight"]
    old_head = old.actor.state_dict()["heads.0.weight"]
    torch.testing.assert_close(new_head[0:5], old_head[0:5])            # core and duel actions
    torch.testing.assert_close(new_head[5], fresh_head[5])               # the new block's action keeps its init


def test_mismatched_block_sizes_are_rejected():
    config = MappoConfig(hidden=(8,))
    old = MappoTrainer([(7, 5)], 4, config)
    new = MappoTrainer([(8, 5)], 4, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("mage_dps", 7, 5)]),
                  "stage": stage_with({"mage_dps": [("core", 4, 3), ("duel", 3, 2)]})}

    with pytest.raises(ValueError):
        seed_trainer(new, checkpoint, spec([Layout("mage_dps", 8, 5)], 4),
                     stage_with({"mage_dps": [("core", 4, 3), ("duel", 4, 2)]}))


def test_without_spans_the_layout_is_seeded_as_a_prefix():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8,))
    old = MappoTrainer([(5, 3)], 4, config)
    new = MappoTrainer([(7, 4)], 4, config)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec([Layout("rogue_dps", 5, 3)])}

    seed_trainer(new, checkpoint, spec([Layout("rogue_dps", 7, 4)], 4),
                 stage_with({"rogue_dps": [("core", 5, 3), ("duel", 2, 1)]}))

    new_w = new.actor.state_dict()["adapters.0.weight"]
    torch.testing.assert_close(new_w[:, :5], old.actor.state_dict()["adapters.0.weight"])
    assert torch.count_nonzero(new_w[:, 5:]) == 0


def test_init_from_follows_the_stage_seed_chain():
    stage = {"stage": "class_role_pack", "seed_chain": ["class_role_duel", "class_role"]}
    config = TrainConfig(run_name="class_role_pack", runs_dir="/out/runs")
    assert config.resolved_init_from(stage) == ["/out/runs/class_role_duel/best.pt", "/out/runs/class_role/best.pt"]

    # The first stage, and a scenario the sim wrote no stage.json for, train from scratch.
    assert config.resolved_init_from({"seed_chain": []}) == []
    assert config.resolved_init_from(None) == []

    # Named candidates, with the run directory filled in.
    named = TrainConfig(run_name="mage", runs_dir="runs", init_from=["{runs_dir}/other/best.pt", ""])
    assert named.resolved_init_from(stage) == ["runs/other/best.pt"]
    assert TrainConfig(init_from="").resolved_init_from(stage) == []
    assert TrainConfig(init_from=None).resolved_init_from(stage) == []
