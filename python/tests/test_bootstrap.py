from types import SimpleNamespace

import pytest
import torch

from animus.bootstrap import seed_network, seed_trainer
from animus.config import TrainConfig
from animus.mappo.networks import Actor, Critic, one_hot_agents
from animus.mappo.trainer import MappoConfig, MappoTrainer


def test_seeded_actor_matches_earlier_stage_on_its_inputs_and_actions():
    torch.manual_seed(0)
    old_obs, new_obs, old_actions, new_actions, agents = 6, 9, 3, 5, 1
    old = Actor(old_obs, old_actions, agents, [16, 16])
    new = Actor(new_obs, new_actions, agents, [16, 16])

    new.load_state_dict(seed_network(new.state_dict(), old.state_dict(), old_obs, new_obs, agents, copy_head=True))

    obs = torch.randn(4, old_obs)
    agent = torch.zeros(4, dtype=torch.long)
    extended = torch.cat([obs, torch.randn(4, new_obs - old_obs)], dim=-1)

    old_logits = old.net(torch.cat([obs, one_hot_agents(agent, agents)], dim=-1))
    new_logits = new.net(torch.cat([extended, one_hot_agents(agent, agents)], dim=-1))

    # New feature columns start at zero, so whatever they hold does not change the old actions' logits.
    torch.testing.assert_close(new_logits[:, :old_actions], old_logits)


def test_critic_head_is_not_copied():
    torch.manual_seed(0)
    old = Critic(6, 1, [8])
    new = Critic(9, 1, [8])
    fresh_head = new.state_dict()["net.2.weight"].clone()

    seeded = seed_network(new.state_dict(), old.state_dict(), 6, 9, 1, copy_head=False)

    torch.testing.assert_close(seeded["net.2.weight"], fresh_head)
    torch.testing.assert_close(seeded["net.0.weight"][:, :6], old.state_dict()["net.0.weight"][:, :6])


def test_mismatched_hidden_sizes_are_rejected():
    old = Actor(6, 3, 1, [16])
    new = Actor(9, 5, 1, [32])
    with pytest.raises(ValueError):
        seed_network(new.state_dict(), old.state_dict(), 6, 9, 1, copy_head=True)


def test_seed_trainer_from_checkpoint():
    config = MappoConfig(hidden=(16, 16))
    old = MappoTrainer(6, 6, 3, 1, config)
    new = MappoTrainer(9, 9, 5, 1, config)
    checkpoint = {
        "trainer": old.state_dict(),
        "spec": {"agents_per_env": 1, "obs_dim": 6, "state_dim": 6, "num_actions": 3},
    }

    seed_trainer(new, checkpoint, SimpleNamespace(agents_per_env=1, obs_dim=9, state_dim=9, num_actions=5))

    torch.testing.assert_close(new.actor.state_dict()["net.2.weight"], old.actor.state_dict()["net.2.weight"])


def test_init_from_resolves_the_base_run():
    config = TrainConfig(run_name="warrior_dps_duel", init_from="runs/{base_run}/latest.pt")
    assert config.resolved_init_from() == "runs/warrior_dps/latest.pt"
    assert TrainConfig(run_name="warrior_dps").resolved_init_from() == ""

    pack = TrainConfig(run_name="mage_dps_pack", init_from="runs/{base_run}_duel/latest.pt")
    assert pack.resolved_init_from() == "runs/mage_dps_duel/latest.pt"
    gauntlet = TrainConfig(run_name="druid_tank_gauntlet", init_from="runs/{base_run}_pack/latest.pt")
    assert gauntlet.resolved_init_from() == "runs/druid_tank_pack/latest.pt"
    companion = TrainConfig(run_name="priest_heal_companion", init_from="runs/{base_run}_gauntlet/latest.pt")
    assert companion.resolved_init_from() == "runs/priest_heal_gauntlet/latest.pt"
