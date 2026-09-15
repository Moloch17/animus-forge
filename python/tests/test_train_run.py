"""A whole learner run (animus.train.TrainingRun) against a fake sim: updates, evaluations, checkpoints, finish."""

import csv
import json
import socket
import threading
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip("torch")

from animus import protocol as p  # noqa: E402
from animus.config import TrainConfig  # noqa: E402
from animus.train import TrainingRun  # noqa: E402

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
    layouts=(p.Layout("warrior_dps", 3, 2), p.Layout("mage_dps", 2, 2)),
    episode_info_names=("present", "dps"),
)

EPISODE_DECISIONS = 3


def read_exact(conn: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise ConnectionError("client closed")
        data += chunk
    return data


def fake_sim(listener: socket.socket, modes: list) -> None:
    """3-decision episodes paying 1 per decision; env 1's second seat is empty (present 0, only the no-op)."""
    conn, _ = listener.accept()
    with conn:
        read_exact(conn, p.HEADER.size + p.HELLO.size)
        payload = p.encode_spec(SPEC)
        conn.sendall(p.encode_header(p.MsgType.SPEC, len(payload)) + payload)

        e_count, a_count = SPEC.num_envs, SPEC.agents_per_env
        evaluating, episodes, next_seed = False, 0, 0
        env_seed = [p.NO_EPISODE_SEED] * e_count
        env_time = [0] * e_count
        decision = 0

        def reset(e):
            nonlocal next_seed
            env_time[e] = 0
            env_seed[e] = p.NO_EPISODE_SEED
            if evaluating and next_seed < episodes:
                env_seed[e], next_seed = next_seed, next_seed + 1

        def blank():
            present = np.ones((e_count, a_count), bool)
            present[1, 1] = False
            mask = np.ones((e_count, a_count, SPEC.num_actions), bool)
            mask[1, 1, 1:] = False
            return p.Step(
                decision=decision,
                obs=np.random.default_rng(decision).random((e_count, a_count, SPEC.obs_dim), dtype=np.float32),
                state=np.zeros((e_count, SPEC.state_dim), np.float32),
                mask=mask,
                layout=np.tile(np.arange(a_count, dtype=np.uint16), (e_count, 1)),
                present=present,
                reward=np.zeros((e_count, a_count), np.float32),
                done=np.zeros(e_count, bool),
                terminated=np.zeros(e_count, bool),
                final_obs=np.zeros((e_count, a_count, SPEC.obs_dim), np.float32),
                final_state=np.zeros((e_count, SPEC.state_dim), np.float32),
                episode_info=np.zeros((e_count, a_count, SPEC.episode_info_dim), np.float32),
                episode_seed=np.full(e_count, p.NO_EPISODE_SEED, np.uint32),
            )

        step = blank()
        while True:
            payload = p.encode_step(SPEC, step)
            try:
                conn.sendall(p.encode_header(p.MsgType.STEP, len(payload)) + payload)
                msg_type, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
            except (ConnectionError, OSError):
                return
            if msg_type == p.MsgType.CLOSE:
                return
            body = read_exact(conn, length)
            decision += 1
            step = blank()
            if msg_type == p.MsgType.MODE:
                evaluating, _, episodes, baseline = p.decode_mode(body)
                modes.append((evaluating, episodes, baseline))
                next_seed = 0
                for e in range(e_count):
                    reset(e)
                continue
            for e in range(e_count):
                env_time[e] += 1
                step.reward[e] = np.where(step.present[e], 1.0, 0.0)
                if env_time[e] == EPISODE_DECISIONS:
                    step.done[e] = True
                    step.episode_seed[e] = env_seed[e]
                    step.episode_info[e, :, 0] = step.present[e]
                    reset(e)


def test_training_run_trains_evaluates_and_finishes(tmp_path):
    path = str(tmp_path / "forge.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    modes = []
    server = threading.Thread(target=fake_sim, args=(listener, modes))
    server.start()

    steps_per_update = 4 * SPEC.num_envs * SPEC.agents_per_env
    config = TrainConfig.load(Path(__file__).parent.parent / "configs" / "stage1_duel.yaml", [
        f"socket={path}", f"runs_dir={tmp_path / 'runs'}", f"layouts_dir={tmp_path / 'layouts'}", "run_name=fake",
        "rollout_length=4", f"total_env_steps={2 * steps_per_update}", "checkpoint_every=1", "init_from=''",
        "train_device=cpu", "mappo.hidden=[8, 8]", "mappo.epochs=1", "mappo.minibatches=1",
        f"eval.every_env_steps={steps_per_update}", "eval.episodes=2", "eval.baseline=''",
        "convergence.patience=0", "target.min_over_baseline=null", "target.min_layout_over_baseline=null",
    ])

    exit_code = TrainingRun(config, resume=False).run()
    server.join(timeout=10)
    listener.close()

    run_dir = tmp_path / "runs" / "fake"
    assert exit_code == 0
    finished = json.loads((run_dir / "finished.json").read_text())
    assert (finished["advanced"], finished["update"], finished["env_steps"]) == (True, 2, 2 * steps_per_update)

    with (run_dir / "metrics.csv").open() as f:
        rows = list(csv.DictReader(f))
    assert [int(row["update"]) for row in rows] == [1, 2]
    # Empty seats earn nothing and are not samples: every present seat earns 1 per decision.
    assert all(float(row["reward_per_decision"]) == pytest.approx(1.0) for row in rows)

    with (run_dir / "eval.csv").open() as f:
        evals = list(csv.DictReader(f))
    assert [int(row["env_steps"]) for row in evals] == [0, steps_per_update, 2 * steps_per_update]
    assert modes.count((True, 2, "")) == 3

    assert (run_dir / "latest.pt").exists() and (run_dir / "best.pt").exists()
    assert json.loads((run_dir / "progress.json").read_text())["phase"] == "finished"
