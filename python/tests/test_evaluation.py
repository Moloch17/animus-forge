"""Seeded evaluation against a fake sim, plateau tracking and config overrides."""

import socket
import threading

import numpy as np
import pytest

from animus import protocol as p
from animus.config import TrainConfig
from animus.env import ForgeEnv
from animus.evaluation import PlateauTracker, run_evaluation
from animus.train import init_from_checkpoint, run_finished

SPEC = p.Spec(
    version=p.PROTOCOL_VERSION,
    num_envs=2,
    agents_per_env=1,
    obs_dim=3,
    state_dim=3,
    num_actions=2,
    episode_info_dim=2,
    tick_ms=50,
    decision_ticks=1,
    episode_seconds=1,
    scenario="fake",
    episode_info_names=("level", "dps"),
)


def read_exact(conn: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        assert chunk, "client closed early"
        data += chunk
    return data


def blank_step(decision: int) -> p.Step:
    e, a = SPEC.num_envs, SPEC.agents_per_env
    return p.Step(
        decision=decision,
        obs=np.zeros((e, a, SPEC.obs_dim), np.float32),
        state=np.zeros((e, SPEC.state_dim), np.float32),
        mask=np.ones((e, a, SPEC.num_actions), bool),
        reward=np.zeros((e, a), np.float32),
        done=np.zeros(e, bool),
        terminated=np.zeros(e, bool),
        final_obs=np.zeros((e, a, SPEC.obs_dim), np.float32),
        final_state=np.zeros((e, SPEC.state_dim), np.float32),
        episode_info=np.zeros((e, SPEC.episode_info_dim), np.float32),
        episode_seed=np.full(e, p.NO_EPISODE_SEED, np.uint32),
    )


def test_run_evaluation_collects_each_seed_once(tmp_path):
    """Fake sim: 3-decision episodes paying reward 1 per decision; seeds go to envs in reset order."""
    path = str(tmp_path / "forge.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    modes = []

    def fake_sim():
        conn, _ = listener.accept()
        with conn:
            read_exact(conn, p.HEADER.size + p.HELLO.size)
            spec_payload = p.encode_spec(SPEC)
            conn.sendall(p.encode_header(p.MsgType.SPEC, len(spec_payload)) + spec_payload)

            decision = 0
            evaluating, episodes, next_seed = False, 0, 0
            env_seed = [p.NO_EPISODE_SEED] * SPEC.num_envs
            env_time = [0] * SPEC.num_envs

            def reset(e):
                nonlocal next_seed
                env_time[e] = 0
                env_seed[e] = p.NO_EPISODE_SEED
                if evaluating and next_seed < episodes:
                    env_seed[e] = next_seed
                    next_seed += 1

            step = blank_step(decision)
            while True:
                payload = p.encode_step(SPEC, step)
                conn.sendall(p.encode_header(p.MsgType.STEP, len(payload)) + payload)
                msg_type, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
                if msg_type == p.MsgType.CLOSE:
                    return
                body = read_exact(conn, length)
                decision += 1
                step = blank_step(decision)
                if msg_type == p.MsgType.MODE:
                    evaluating, seed, episodes, baseline = p.decode_mode(body)
                    modes.append((evaluating, seed, episodes, baseline))
                    next_seed = 0
                    for e in range(SPEC.num_envs):
                        reset(e)
                    continue
                for e in range(SPEC.num_envs):
                    env_time[e] += 1
                    step.reward[e] = 1.0
                    if env_time[e] == 3:
                        step.done[e] = True
                        step.episode_seed[e] = env_seed[e]
                        seed = env_seed[e]
                        step.episode_info[e] = (20 + (seed % 2) * 40, 7.0) if seed != p.NO_EPISODE_SEED else (0, 0)
                        reset(e)

    server = threading.Thread(target=fake_sim)
    server.start()
    env = ForgeEnv(path, connect_timeout=5)
    env.reset()

    result, training_step = run_evaluation(
        env, SPEC, lambda step: np.zeros((2, 1), np.int32), episodes=5, seed=77, baseline=""
    )
    env.close()
    server.join(timeout=5)
    listener.close()

    assert modes == [(True, 77, 5, ""), (False, 0, 0, "")]
    assert result.episodes == 5
    np.testing.assert_allclose(result.returns, 3.0)
    assert result.score == pytest.approx(3.0)
    summary = result.summary(("dps", "missing"))
    assert summary["dps"] == pytest.approx(7.0)
    assert "missing" not in summary
    assert summary["bands"]["1-20"]["episodes"] == 3 and summary["bands"]["41-60"]["episodes"] == 2
    assert not training_step.done.any()


def test_plateau_tracker():
    tracker = PlateauTracker(patience=2, min_improvement=0.1, min_improvement_abs=0.01)
    assert tracker.observe(1.0, 10)
    assert not tracker.observe(1.05, 20)  # within 10% of the best
    assert not tracker.plateaued(20, 0)
    assert not tracker.observe(0.9, 30)
    assert tracker.plateaued(30, 0)
    assert not tracker.plateaued(30, 40)  # min_env_steps not reached
    assert tracker.observe(1.2, 40)
    assert not tracker.plateaued(40, 0)

    restored = PlateauTracker(patience=2)
    restored.load_state_dict(tracker.state_dict())
    assert restored.best == 1.2 and restored.best_env_steps == 40 and len(restored.history) == 4


def test_config_overrides(tmp_path):
    path = tmp_path / "c.yaml"
    path.write_text("total_env_steps: 100\neval:\n  every_env_steps: 10\n  baseline: greedy\n")
    config = TrainConfig.load(path, ["total_env_steps=5", "eval.episodes=16", "plateau.patience=3",
                                     "eval.report=[dps,died]"])
    assert config.total_env_steps == 5
    assert config.eval.every_env_steps == 10 and config.eval.episodes == 16 and config.eval.baseline == "greedy"
    assert config.eval.report == ("dps", "died")
    assert config.plateau.patience == 3

    with pytest.raises(ValueError):
        TrainConfig.load(path, ["eval.nope=1"])


def test_init_from_falls_back_to_latest(tmp_path):
    run = tmp_path / "warrior_dps"
    run.mkdir()
    assert init_from_checkpoint(str(run / "best.pt")) is None
    (run / "latest.pt").write_text("x")
    assert init_from_checkpoint(str(run / "best.pt")) == run / "latest.pt"
    (run / "best.pt").write_text("x")
    assert init_from_checkpoint(str(run / "best.pt")) == run / "best.pt"


def test_run_finished(tmp_path):
    marker = tmp_path / "finished.json"
    assert run_finished(marker, 100) is None
    marker.write_text('{"reason": "total_env_steps", "env_steps": 100}')
    assert run_finished(marker, 100)
    assert run_finished(marker, 200) is None  # the total was raised: keep training
    marker.write_text('{"reason": "plateau", "env_steps": 50}')
    assert run_finished(marker, 200)
