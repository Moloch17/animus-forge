"""Masked sampling never picks a disallowed action, and the trainer runs one update end to end."""

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from animus.mappo.buffer import RolloutBuffer  # noqa: E402
from animus.mappo.networks import masked_distribution  # noqa: E402
from animus.mappo.trainer import MappoConfig, MappoTrainer  # noqa: E402


def test_masked_actions_never_sampled():
    torch.manual_seed(0)
    logits = torch.randn(2000, 4) * 5
    mask = torch.rand(2000, 4) < 0.5
    mask[:, 2] = True  # at least one allowed per row
    samples = masked_distribution(logits, mask).sample()
    assert mask[torch.arange(2000), samples].all()


def test_fully_masked_row_falls_back_to_action_zero():
    dist = masked_distribution(torch.zeros(1, 3), torch.zeros(1, 3, dtype=torch.bool))
    assert dist.sample().item() == 0


def test_trainer_update_smoke():
    envs, agents, obs_dim, state_dim, actions = 4, 2, 6, 7, 3
    trainer = MappoTrainer(obs_dim, state_dim, actions, agents, MappoConfig(hidden=(16,), epochs=2, minibatches=2))
    buffer = RolloutBuffer(8, envs, agents, obs_dim, state_dim, actions)
    rng = np.random.default_rng(0)

    while not buffer.full:
        obs = rng.random((envs, agents, obs_dim), dtype=np.float32)
        state = rng.random((envs, state_dim), dtype=np.float32)
        mask = rng.random((envs, agents, actions)) < 0.6
        mask[..., 0] = True
        chosen, log_probs = trainer.act(obs, mask)
        assert mask[np.arange(envs)[:, None], np.arange(agents)[None, :], chosen].all()
        buffer.add_decision(obs, state, mask, chosen, log_probs, trainer.value(state))
        done = rng.random(envs) < 0.2
        buffer.add_outcome(rng.random((envs, agents), dtype=np.float32), done, np.zeros(envs, bool),
                           np.zeros((envs, agents), np.float32))

    buffer.finish(trainer.value(rng.random((envs, state_dim), dtype=np.float32)), 0.99, 0.95)
    stats = trainer.update(buffer)
    assert all(np.isfinite(v) for v in stats.values())
