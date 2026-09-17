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


def test_a_per_minibatch_auxiliary_is_refused():
    """A flat hook cannot carry a teacher's memory through a replayed sequence, so it is refused rather than run."""
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=4))
    buffer = RolloutBuffer(2, 2, 1, 3, 4, 2, 0, 4)
    with pytest.raises(ValueError, match="sequence-aware"):
        trainer.update(buffer, auxiliary=lambda *_: None)


def test_a_sequence_aware_auxiliary_is_replayed_in_order():
    """The recurrent update calls a sequence-aware auxiliary (animus.distill.Distiller) once per decision, with the
    memories it handed out at the start of the sequence, and adds its loss to the actor's."""
    torch.manual_seed(0)
    steps, envs = 4, 2
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=4, epochs=1, minibatches=1))
    buffer = RolloutBuffer(steps, envs, 1, 3, 4, 2, 0, trainer.recurrent_size)
    fill(trainer, buffer, steps, envs, 1, 3, 4, 2, done_at=2)

    class FakeDistiller:
        coef = 0.5

        def __init__(self):
            self.sequences = []
            self.calls = 0

        def begin_sequence(self, rows, device):
            self.sequences.append(rows)
            return {0: torch.zeros(rows, 2, device=device)}

        def step_loss(self, obs, state, layout, mask, logits, memories, dones):
            self.calls += 1
            assert memories[0].shape[0] == obs.shape[0]
            assert dones.shape[0] == obs.shape[0]
            return logits.square().mean() * self.coef, obs.shape[0]

    distiller = FakeDistiller()
    stats = trainer.update(buffer, auxiliary=distiller)

    assert distiller.sequences == [envs]           # one sequence per minibatch of envs
    assert distiller.calls == steps                # one call per replayed decision
    assert stats["distill_rows"] > 0.0 and "distill_kl" in stats


def test_a_rollout_replays_from_the_memory_it_started_with():
    """The buffer keeps the memory each decision was taken with, and the update replays a sequence from the first
    one. The acting state is not reset between rollouts (animus.train.rollout), so a second rollout starts from
    whatever the first ended with, and this is what carries a plan past rollout_length."""
    torch.manual_seed(0)
    steps, envs = 3, 2
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=4, epochs=1, minibatches=1))
    buffer = RolloutBuffer(steps, envs, 1, 3, 4, 2, 0, trainer.recurrent_size)

    acting = trainer.acting_state(envs, 1)
    acting.memory[:] = 0.5      # what an earlier rollout left behind
    rng = np.random.default_rng(0)
    for step in range(steps):
        obs = rng.random((envs, 1, 3), dtype=np.float32)
        state = rng.random((envs, 4), dtype=np.float32)
        mask = np.ones((envs, 1, 2), bool)
        layout = np.zeros((envs, 1), np.int64)
        memory = acting.memory.copy()
        chosen, log_probs, values, foresight, goals = trainer.act_and_value(obs, mask, layout, state, state=acting)
        buffer.add_decision(obs, state, mask, layout, chosen, log_probs, values, None, foresight, memory, goals)
        dones = np.zeros(envs, bool)
        buffer.add_outcome(rng.random((envs, 1), dtype=np.float32), dones, dones,
                           np.zeros((envs, 1), np.float32), None)

    sequences = buffer.sequences()
    assert np.allclose(sequences["memory"][0], 0.5)          # the sequence starts where the last one ended
    assert not np.allclose(sequences["memory"][1], 0.5)      # and moves on from there
