from pathlib import Path
from types import SimpleNamespace

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


def test_init_from_resolves_candidates():
    config = TrainConfig(run_name="stage2_pack", init_from="runs/stage1_duel/best.pt")
    assert config.resolved_init_from() == ["runs/stage1_duel/best.pt"]
    assert TrainConfig(run_name="stage1_duel").resolved_init_from() == []
    assert TrainConfig(run_name="stage1_duel", init_from=None).resolved_init_from() == []

    run = TrainConfig(run_name="stage3_gauntlet", init_from=["runs/stage2_pack/best.pt", "", "runs/{run_name}/old.pt"])
    assert run.resolved_init_from() == ["runs/stage2_pack/best.pt", "runs/stage3_gauntlet/old.pt"]


def test_stage_configs_seed_from_every_earlier_stage_closest_first():
    stages = ["stage1_duel", "stage2_pack", "stage3_gauntlet", "stage4_companion", "stage5_party", "stage6_pvp",
              "stage7_arena"]
    configs = Path(__file__).resolve().parents[1] / "configs"
    for index, stage in enumerate(stages):
        config = TrainConfig.load(configs / f"{stage}.yaml")
        assert config.run_name == stage
        assert config.resolved_init_from() == [f"runs/{earlier}/best.pt" for earlier in reversed(stages[:index])]
