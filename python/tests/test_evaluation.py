"""Seeded evaluation against a fake sim, convergence tracking and config overrides."""

import socket
import threading

import numpy as np
import pytest

from animus import protocol as p
from animus.config import TrainConfig
from animus.env import ForgeEnv
from animus.evaluation import ConvergenceTracker, EvalResult, run_evaluation
from animus.train import init_from_checkpoint

SPEC = p.Spec(
    version=p.PROTOCOL_VERSION,
    num_envs=2,
    agents_per_env=2,
    obs_dim=3,
    state_dim=3,
    num_actions=2,
    episode_info_dim=2,
    tick_ms=50,
    decision_ticks=1,
    episode_seconds=1,
    scenario="fake",
    layouts=(p.Layout("warrior_dps", 3, 2), p.Layout("mage_dps", 3, 2)),
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
        layout=np.tile(np.arange(a, dtype=np.uint16), (e, 1)),  # agent 0 warrior, agent 1 mage
        reward=np.zeros((e, a), np.float32),
        done=np.zeros(e, bool),
        terminated=np.zeros(e, bool),
        final_obs=np.zeros((e, a, SPEC.obs_dim), np.float32),
        final_state=np.zeros((e, SPEC.state_dim), np.float32),
        episode_info=np.zeros((e, a, SPEC.episode_info_dim), np.float32),
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
                    step.reward[e] = (1.0, 2.0)  # the warrior earns 1 per decision, the mage 2
                    if env_time[e] == 3:
                        step.done[e] = True
                        step.episode_seed[e] = env_seed[e]
                        seed = env_seed[e]
                        if seed != p.NO_EPISODE_SEED:
                            step.episode_info[e] = (20 + (seed % 2) * 40, 7.0)
                        reset(e)

    server = threading.Thread(target=fake_sim)
    server.start()
    env = ForgeEnv(path, connect_timeout=5)
    env.reset()

    result, training_step = run_evaluation(
        env, SPEC, lambda step: np.zeros((2, 2), np.int32), episodes=5, seed=77, baseline=""
    )
    env.close()
    server.join(timeout=5)
    listener.close()

    assert modes == [(True, 77, 5, ""), (False, 0, 0, "")]
    assert result.episodes == 10  # 5 seeded episodes x 2 agents
    np.testing.assert_allclose(result.returns, [3.0, 6.0] * 5)
    assert result.score == pytest.approx(4.5)
    assert result.layouts == ("warrior_dps", "mage_dps") * 5
    summary = result.summary(("dps", "missing"))
    assert summary["dps"] == pytest.approx(7.0)
    assert "missing" not in summary
    assert summary["bands"]["1-20"]["episodes"] == 6 and summary["bands"]["41-60"]["episodes"] == 4
    assert summary["layouts"]["mage_dps"]["score"] == pytest.approx(6.0)
    assert not training_step.done.any()


def test_stderr_in_summary():
    result = EvalResult("learner", np.array([1.0, 3.0, 5.0, 7.0]), np.zeros((4, 0), np.float32), ())
    assert result.stderr == pytest.approx(np.std([1, 3, 5, 7], ddof=1) / 2)
    assert result.summary(())["stderr"] == pytest.approx(result.stderr)
    assert EvalResult("learner", np.array([2.0]), np.zeros((1, 0), np.float32), ()).stderr == 0.0


def test_convergence_tracker_margins():
    tracker = ConvergenceTracker(patience=2, min_improvement=0.1, min_improvement_abs=0.01, z=2.0)
    assert tracker.observe(1.0, 10)
    assert not tracker.observe(1.05, 20)  # within 10% of the best
    assert not tracker.converged(20)
    assert not tracker.observe(0.9, 30)
    assert tracker.converged(30)
    assert not tracker.converged(30, min_env_steps=40)
    assert tracker.observe(1.2, 40)
    assert not tracker.converged(40)

    # Noise: 0.5 better than the best is not an improvement when both scores have a standard error of 0.3.
    noisy = ConvergenceTracker(patience=1, min_improvement=0.0, min_improvement_abs=0.0, z=2.0)
    noisy.observe(10.0, 0, stderr=0.3)
    assert not noisy.observe(10.5, 1, stderr=0.3)
    assert noisy.last_margin == pytest.approx(2.0 * np.sqrt(0.18))
    assert noisy.observe(11.0, 2, stderr=0.3)

    restored = ConvergenceTracker(patience=2)
    restored.load_state_dict(tracker.state_dict())
    assert restored.best == 1.2 and restored.best_env_steps == 40 and len(restored.history) == 4


def test_convergence_waits_for_a_flat_trend():
    """No new best for `patience` evaluations is not enough while the recent scores still climb steeply."""
    flat = ConvergenceTracker(patience=2, window=3, min_improvement=0.0, min_improvement_abs=1.0, z=0.0)
    for i, score in enumerate([10.0, 10.6, 10.9]):
        flat.observe(score, i * 100)
    assert flat.evals_since_best == 2
    assert flat.projected_gain() == pytest.approx(0.9)  # 0.45 per evaluation x 2 evaluations
    assert flat.converged(200)  # below the 1.0 margin

    # Recovering from a dip after the best: three evaluations without a new best, but climbing 0.75 per evaluation.
    recovering = ConvergenceTracker(patience=2, window=3, min_improvement=0.0, min_improvement_abs=1.0, z=0.0)
    for i, score in enumerate([12.0, 10.0, 10.5, 11.5]):
        recovering.observe(score, i * 100)
    assert recovering.evals_since_best == 3
    assert recovering.projected_gain() == pytest.approx(1.5)
    assert not recovering.converged(300)


def test_convergence_segment_reset_keeps_best():
    tracker = ConvergenceTracker(patience=1, min_improvement=0.0, min_improvement_abs=0.5)
    tracker.observe(5.0, 0)
    tracker.observe(4.0, 10)
    assert tracker.converged(10)
    tracker.reset_segment(10)
    assert tracker.best == 5.0 and tracker.evals_since_best == 0
    assert not tracker.converged(10)
    assert not tracker.converged(15, min_env_steps=10)  # counted from the restart
    tracker.observe(4.5, 20)
    assert tracker.converged(20, min_env_steps=10)
    assert tracker.projected_gain() is None  # one point in the new segment


def test_config_overrides(tmp_path):
    path = tmp_path / "c.yaml"
    path.write_text("total_env_steps: 100\neval:\n  every_env_steps: 10\n  baseline: greedy\n")
    config = TrainConfig.load(path, ["total_env_steps=5", "eval.episodes=16", "convergence.patience=3",
                                     "eval.report=[dps,died]", "target.min_over_baseline=0.2",
                                     "target.metrics={killed: {min: 0.9}}", "restarts.max_restarts=1"])
    assert config.total_env_steps == 5
    assert config.eval.every_env_steps == 10 and config.eval.episodes == 16 and config.eval.baseline == "greedy"
    assert config.eval.report == ("dps", "died")
    assert config.convergence.patience == 3
    assert config.target.min_over_baseline == 0.2 and config.target.metrics == {"killed": {"min": 0.9}}
    assert config.target.enabled and not TrainConfig().target.enabled
    assert config.restarts.max_restarts == 1

    with pytest.raises(ValueError):
        TrainConfig.load(path, ["eval.nope=1"])
    with pytest.raises(ValueError, match="replaced by 'convergence'"):
        TrainConfig.load(path, ["plateau.patience=3"])


def test_config_extends_merges_sections(tmp_path):
    (tmp_path / "base.yaml").write_text("run_name: base\ntotal_env_steps: 100\nmappo:\n  hidden: [8, 8]\n"
                                        "  gamma: 0.9\neval:\n  baseline: greedy\n")
    (tmp_path / "stage.yaml").write_text("extends: base.yaml\nrun_name: stage\nmappo:\n  gamma: 0.99\n")

    config = TrainConfig.load(tmp_path / "stage.yaml")
    assert config.run_name == "stage" and config.total_env_steps == 100
    assert tuple(config.mappo.hidden) == (8, 8) and config.mappo.gamma == 0.99
    assert config.eval.baseline == "greedy"

    (tmp_path / "loop.yaml").write_text("extends: loop.yaml\n")
    with pytest.raises(ValueError):
        TrainConfig.load(tmp_path / "loop.yaml")


def test_init_from_falls_back_to_latest(tmp_path):
    run = tmp_path / "warrior_dps"
    run.mkdir()
    assert init_from_checkpoint(str(run / "best.pt")) is None
    (run / "latest.pt").write_text("x")
    assert init_from_checkpoint(str(run / "best.pt")) == run / "latest.pt"
    (run / "best.pt").write_text("x")
    assert init_from_checkpoint(str(run / "best.pt")) == run / "best.pt"
