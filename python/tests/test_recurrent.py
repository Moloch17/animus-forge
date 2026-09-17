"""A recurrent actor: memory carried between decisions, cleared with an episode, and learned from sequences."""

import numpy as np
import pytest
import torch

from animus.mappo.buffer import RolloutBuffer
from animus.mappo.trainer import MappoConfig, MappoTrainer


def fill(trainer, buffer, steps, envs, agents, obs_dim, state_dim, actions, done_at=None):
    rng = np.random.default_rng(0)
    acting = trainer.acting_state(envs, agents)
    for step in range(steps):
        obs = rng.random((envs, agents, obs_dim), dtype=np.float32)
        state = rng.random((envs, state_dim), dtype=np.float32)
        mask = np.ones((envs, agents, actions), bool)
        layout = np.zeros((envs, agents), np.int64)
        memory = acting.memory.copy() if acting.memory is not None else None
        chosen, log_probs, values, foresight, goals = trainer.act_and_value(obs, mask, layout, state, state=acting)
        buffer.add_decision(obs, state, mask, layout, chosen, log_probs, values, None, foresight, memory, goals)
        dones = np.array([step == done_at, False])
        buffer.add_outcome(rng.random((envs, agents), dtype=np.float32), dones, dones,
                           np.zeros((envs, agents), np.float32))
        acting.clear(dones)

    last = trainer.foresight_of(rng.random((envs, agents, obs_dim), dtype=np.float32),
                                np.zeros((envs, agents), np.int64), acting.memory)
    buffer.finish(np.zeros((envs, agents), np.float32), 0.99, 0.95, last_foresight=last,
                  foresight_gammas=(0.95, 0.99) if last is not None else (), time_scale_decisions=10.0)


def test_memory_is_carried_and_cleared():
    torch.manual_seed(0)
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=6))
    acting = trainer.acting_state(2, 1)
    assert acting.memory.shape == (2, 1, 6) and not acting.memory.any()

    obs = np.ones((2, 1, 3), np.float32)
    mask = np.ones((2, 1, 2), bool)
    layout = np.zeros((2, 1), np.int64)
    trainer.act(obs, mask, layout, False, acting)
    carried = acting.memory.copy()
    assert carried.any()  # the GRU wrote something

    # The same observation with a different memory is a different decision: the policy is not stateless.
    trainer.act(obs, mask, layout, True, acting)
    assert not np.allclose(carried, acting.memory)

    acting.clear(np.array([True, False]))
    assert not acting.memory[0].any() and acting.memory[1].any()


def test_update_replays_sequences_and_learns():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), recurrent_size=6, epochs=2, minibatches=2)
    trainer = MappoTrainer([(3, 2)], 4, config)
    buffer = RolloutBuffer(6, 2, 1, 3, 4, 2, trainer.foresight_outputs, trainer.recurrent_size)
    fill(trainer, buffer, 6, 2, 1, 3, 4, 2, done_at=3)

    before = trainer.actor.memory.weight_ih.detach().clone()
    stats = trainer.update(buffer)
    assert stats["epochs_run"] == 2.0 and "policy_loss" in stats
    assert not torch.allclose(before, trainer.actor.memory.weight_ih)  # the GRU itself is trained


def test_recurrent_and_foresight_together():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), recurrent_size=4, foresight_coef=0.5, epochs=1)
    trainer = MappoTrainer([(3, 2)], 4, config)
    buffer = RolloutBuffer(4, 2, 1, 3, 4, 2, trainer.foresight_outputs, trainer.recurrent_size)
    fill(trainer, buffer, 4, 2, 1, 3, 4, 2, done_at=2)
    stats = trainer.update(buffer)
    assert stats["foresight_loss"] > 0.0


def test_distillation_is_refused_for_now():
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=4))
    buffer = RolloutBuffer(2, 2, 1, 3, 4, 2, 0, 4)
    with pytest.raises(ValueError, match="distillation"):
        trainer.update(buffer, auxiliary=lambda *_: None)
