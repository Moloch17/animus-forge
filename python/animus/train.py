"""Train a MAPPO policy against a running Animus Forge sim.

    python -m animus.train --config configs/warrior_dummy.yaml [--resume runs/<name>/latest.pt]

The worldserver starts this automatically when AnimusForge.Learner.AutoStart = 1 (with
--resume-latest, so training continues across server restarts). Run by hand, the client retries
until the sim's socket appears.
"""

from __future__ import annotations

import argparse
import csv
import json
import random
import time
from dataclasses import asdict
from pathlib import Path

import numpy as np
import torch
import yaml

from .config import TrainConfig
from .env import ForgeEnv
from .mappo.buffer import RolloutBuffer
from .mappo.trainer import MappoTrainer


class RunLogger:
    """CSV always; TensorBoard when it is installed.

    Columns are fixed up front so metrics that only exist some updates (episode stats) are never
    dropped. When resuming, rows are appended to the existing file.
    """

    def __init__(self, run_dir: Path, columns: list[str], append: bool):
        self.csv_path = run_dir / "metrics.csv"
        resume_file = append and self.csv_path.exists() and self.csv_path.stat().st_size > 0
        if resume_file:
            with self.csv_path.open(newline="") as existing:
                columns = next(csv.reader(existing))
        self._csv_file = self.csv_path.open("a" if resume_file else "w", newline="")
        self._csv_writer = csv.DictWriter(self._csv_file, fieldnames=columns, restval="", extrasaction="ignore")
        if not resume_file:
            self._csv_writer.writeheader()
        try:
            from torch.utils.tensorboard import SummaryWriter

            self.tb = SummaryWriter(run_dir / "tb")
        except ImportError:
            self.tb = None

    def log(self, step: int, row: dict[str, float]) -> None:
        self._csv_writer.writerow(row)
        self._csv_file.flush()

        if self.tb is not None:
            for key, value in row.items():
                self.tb.add_scalar(key, value, step)

    def close(self) -> None:
        self._csv_file.close()
        if self.tb is not None:
            self.tb.close()


def save_checkpoint(path: Path, trainer: MappoTrainer, config: TrainConfig, spec, update: int, env_steps: int) -> None:
    torch.save(
        {
            "trainer": trainer.state_dict(),
            "config": config.to_dict(),
            "spec": asdict(spec),
            "update": update,
            "env_steps": env_steps,
        },
        path,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True)
    parser.add_argument("--resume", help="checkpoint to continue from")
    parser.add_argument(
        "--resume-latest", action="store_true", help="continue from runs/<run_name>/latest.pt when it exists"
    )
    parser.add_argument("--socket", help="sim socket path, overriding the config")
    args = parser.parse_args()

    config = TrainConfig.load(args.config)
    if args.socket:
        config.socket = args.socket
    random.seed(config.seed)
    np.random.seed(config.seed)
    torch.manual_seed(config.seed)

    run_dir = Path(config.runs_dir) / config.run_name
    run_dir.mkdir(parents=True, exist_ok=True)

    resume = args.resume
    if not resume and args.resume_latest and (run_dir / "latest.pt").exists():
        resume = str(run_dir / "latest.pt")
    (run_dir / "config.yaml").write_text(yaml.safe_dump(config.to_dict(), sort_keys=False))

    print(f"Connecting to {config.socket} ...", flush=True)
    env = ForgeEnv(config.socket)
    spec = env.spec
    (run_dir / "spec.json").write_text(json.dumps(asdict(spec), indent=2))
    print(
        f"Scenario {spec.scenario}: {spec.num_envs} envs x {spec.agents_per_env} agents, obs {spec.obs_dim}, "
        f"state {spec.state_dim}, actions {spec.num_actions}, decision every {spec.decision_ms} ms",
        flush=True,
    )

    trainer = MappoTrainer(
        spec.obs_dim,
        spec.state_dim,
        spec.num_actions,
        spec.agents_per_env,
        config.mappo,
        train_device=config.train_device,
        rollout_device=config.rollout_device,
    )

    update = 0
    env_steps = 0
    if resume:
        checkpoint = torch.load(resume, map_location="cpu", weights_only=False)
        if checkpoint["spec"]["scenario"] != spec.scenario:
            raise SystemExit(f"{resume} was trained on {checkpoint['spec']['scenario']}, the sim runs {spec.scenario}")
        trainer.load_state_dict(checkpoint["trainer"])
        update = checkpoint["update"]
        env_steps = checkpoint["env_steps"]
        print(f"Resumed from {resume} at update {update}, {env_steps} env steps", flush=True)

    envs, agents = spec.num_envs, spec.agents_per_env
    buffer = RolloutBuffer(config.rollout_length, envs, agents, spec.obs_dim, spec.state_dim, spec.num_actions)
    columns = [
        "update", "env_steps", "env_steps_per_sec", "update_seconds", "reward_per_decision", "episodes",
        *(f"action_{a}_rate" for a in range(spec.num_actions)),
        *(f"episode_{name}" for name in spec.episode_info_names),
        "policy_loss", "value_loss", "entropy", "clip_frac", "approx_kl",
    ]
    logger = RunLogger(run_dir, columns, append=bool(resume))

    step = env.reset()
    finished_episodes: list[np.ndarray] = []

    try:
        while env_steps < config.total_env_steps:
            buffer.reset()
            started = time.perf_counter()

            while not buffer.full:
                values = trainer.value(step.state)
                actions, log_probs = trainer.act(step.obs, step.mask)
                buffer.add_decision(step.obs, step.state, step.mask, actions, log_probs, values)

                step = env.step(actions)

                final_values = np.zeros((envs, agents), dtype=np.float32)
                if step.done.any():
                    final_values[step.done] = trainer.value(step.final_state)[step.done]
                    finished_episodes.extend(step.episode_info[step.done])

                buffer.add_outcome(step.reward, step.done, step.terminated, final_values)

            rollout_seconds = time.perf_counter() - started
            buffer.finish(trainer.value(step.state), config.mappo.gamma, config.mappo.gae_lambda)
            stats = trainer.update(buffer)

            update += 1
            env_steps += config.rollout_length * envs * agents

            if update % config.log_every == 0:
                row: dict[str, float] = {
                    "update": update,
                    "env_steps": env_steps,
                    "env_steps_per_sec": config.rollout_length * envs * agents / rollout_seconds,
                    "update_seconds": time.perf_counter() - started - rollout_seconds,
                    "reward_per_decision": float(buffer.rewards.mean()),
                    "episodes": len(finished_episodes),
                }
                for action in range(spec.num_actions):
                    row[f"action_{action}_rate"] = float((buffer.actions == action).mean())
                if finished_episodes:
                    means = np.mean(finished_episodes, axis=0)
                    for name, value in zip(spec.episode_info_names, means):
                        row[f"episode_{name}"] = float(value)
                row.update(stats)

                logger.log(update, row)
                summary = ", ".join(
                    f"{k} {v:.4g}" for k, v in row.items() if k.startswith("episode_") or k in ("entropy", "value_loss")
                )
                print(f"update {update} | steps {env_steps} | {row['env_steps_per_sec']:.0f} sps | {summary}", flush=True)
                finished_episodes.clear()

            if update % config.checkpoint_every == 0:
                save_checkpoint(run_dir / f"checkpoint_{update:06d}.pt", trainer, config, spec, update, env_steps)
                save_checkpoint(run_dir / "latest.pt", trainer, config, spec, update, env_steps)
    finally:
        save_checkpoint(run_dir / "latest.pt", trainer, config, spec, update, env_steps)
        logger.close()
        env.close()


if __name__ == "__main__":
    main()
