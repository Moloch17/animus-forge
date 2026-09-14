"""Train a MAPPO policy against a running Animus Forge sim.

    python -m animus.train --config configs/warrior_dummy.yaml [--resume runs/<name>/latest.pt]

The worldserver starts this automatically when AnimusForge.Learner.AutoStart = 1 (with
--resume-latest, so training continues across server restarts). Run by hand, the client retries
until the sim's socket appears.

With eval.every_env_steps set, the networks are scored on seeded episodes as they train (see
animus.evaluation): the best-scoring networks are kept in best.pt and published, and with plateau.patience
set the run stops once the score stops improving.
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

from .bootstrap import seed_trainer
from .config import TrainConfig
from .env import ForgeEnv
from .evaluation import EvalResult, PlateauTracker, format_summary, run_evaluation
from .export import model_dirs_from_env, publish_model
from .mappo.buffer import RolloutBuffer
from .mappo.trainer import MappoTrainer
from .runs import start_clean_run


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


def save_checkpoint(
    path: Path, trainer: MappoTrainer, config: TrainConfig, spec, update: int, env_steps: int, extra: dict | None = None
) -> None:
    # Write then rename, so a server stop mid-save never leaves a truncated latest.pt or best.pt behind.
    partial = path.with_suffix(path.suffix + ".partial")
    torch.save(
        {
            "trainer": trainer.state_dict(),
            "config": config.to_dict(),
            "spec": asdict(spec),
            "update": update,
            "env_steps": env_steps,
            **(extra or {}),
        },
        partial,
    )
    partial.replace(path)


def publish(actor_state: dict, spec, label: str) -> None:
    """Export an actor to every $ANIMUS_MODEL_DIRS dir; mod-animus loads it on its next config reload."""
    for path in publish_model(actor_state, asdict(spec), model_dirs_from_env()):
        print(f"Published {label} model to {path}", flush=True)


class EvalLog:
    """eval.csv (one row per evaluation) and eval.jsonl (the full summary, level bands included)."""

    COLUMNS = ["update", "env_steps", "policy", "episodes", "score", "best", "evals_since_best", "seconds"]

    def __init__(self, run_dir: Path, tb):
        self.csv_path = run_dir / "eval.csv"
        self.jsonl_path = run_dir / "eval.jsonl"
        self.tb = tb

    def write(self, update: int, env_steps: int, result: EvalResult, summary: dict, tracker: PlateauTracker) -> None:
        row = {
            "update": update,
            "env_steps": env_steps,
            "policy": result.policy,
            "episodes": result.episodes,
            "score": result.score,
            "best": tracker.best,
            "evals_since_best": tracker.evals_since_best,
            "seconds": round(result.seconds, 1),
        }
        new_file = not self.csv_path.exists()
        with self.csv_path.open("a", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=self.COLUMNS)
            if new_file:
                writer.writeheader()
            writer.writerow(row)
        with self.jsonl_path.open("a") as f:
            f.write(json.dumps({**row, "summary": summary}) + "\n")

        if self.tb is None or result.policy != "learner":
            return
        for name, value in summary.items():
            if isinstance(value, float):
                self.tb.add_scalar(f"eval/{name}", value, env_steps)
        for band, values in summary.get("bands", {}).items():
            for name, value in values.items():
                if isinstance(value, float):
                    self.tb.add_scalar(f"eval_{band}/{name}", value, env_steps)


def init_from_checkpoint(path: str) -> Path | None:
    """The configured seed checkpoint, or the latest.pt beside a best.pt that does not exist."""
    candidate = Path(path)
    if candidate.exists():
        return candidate
    if candidate.name == "best.pt" and (candidate.parent / "latest.pt").exists():
        return candidate.parent / "latest.pt"
    return None


def run_finished(finished_path: Path, total_env_steps: int) -> dict | None:
    """The finished.json of a run that is done: stopped on a plateau, or reached a total not raised since."""
    if not finished_path.exists():
        return None
    finished = json.loads(finished_path.read_text())
    if finished.get("reason") == "plateau" or finished.get("env_steps", 0) >= total_env_steps:
        return finished
    return None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True)
    parser.add_argument("--resume", help="checkpoint to continue from")
    parser.add_argument(
        "--resume-latest", action="store_true", help="continue from runs/<run_name>/latest.pt when it exists"
    )
    parser.add_argument(
        "--clean-run",
        help="clean run id: the first start under a new id archives runs/<run_name>/ and trains from scratch",
    )
    parser.add_argument("--socket", help="sim socket path, overriding the config")
    parser.add_argument(
        "--run-name", help="run name (runs/<name>/), overriding the config; the sim passes its scenario"
    )
    parser.add_argument(
        "--set", action="append", default=[], metavar="KEY=VALUE",
        help="override a config value, e.g. --set total_env_steps=5000000 --set eval.every_env_steps=1000000",
    )
    args = parser.parse_args()

    config = TrainConfig.load(args.config, args.set)
    if args.socket:
        config.socket = args.socket
    if args.run_name:
        config.run_name = args.run_name
    random.seed(config.seed)
    np.random.seed(config.seed)
    torch.manual_seed(config.seed)

    run_dir = Path(config.runs_dir) / config.run_name
    if args.clean_run:
        if archived := start_clean_run(run_dir, args.clean_run):
            print(f"Clean run {args.clean_run}: archived the earlier run to {archived}", flush=True)
    run_dir.mkdir(parents=True, exist_ok=True)

    # A finished run exits at once (cleanly), so a queued scenario moves on without training it again.
    finished_path = run_dir / "finished.json"
    if finished := run_finished(finished_path, config.total_env_steps):
        print(f"{config.run_name} already finished ({finished.get('reason')} at {finished.get('env_steps')} env "
              "steps); nothing to do", flush=True)
        return

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

    evaluating = config.eval.every_env_steps > 0
    tracker = PlateauTracker(
        patience=config.plateau.patience if evaluating else 0,
        min_improvement=config.plateau.min_improvement,
        min_improvement_abs=config.plateau.min_improvement_abs,
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
        tracker.load_state_dict(checkpoint.get("plateau"))
        print(f"Resumed from {resume} at update {update}, {env_steps} env steps", flush=True)
    elif init_from := config.resolved_init_from():
        if seed_path := init_from_checkpoint(init_from):
            seed_trainer(trainer, torch.load(seed_path, map_location="cpu", weights_only=False), spec)
            print(f"Seeded the networks from {seed_path}", flush=True)
        else:
            print(f"No {init_from} to seed from; starting from scratch", flush=True)

    envs, agents = spec.num_envs, spec.agents_per_env
    buffer = RolloutBuffer(config.rollout_length, envs, agents, spec.obs_dim, spec.state_dim, spec.num_actions)
    columns = [
        "update", "env_steps", "env_steps_per_sec", "update_seconds", "reward_per_decision", "episodes",
        *(f"action_{a}_rate" for a in range(spec.num_actions)),
        *(f"episode_{name}" for name in spec.episode_info_names),
        "policy_loss", "value_loss", "entropy", "clip_frac", "approx_kl",
    ]
    logger = RunLogger(run_dir, columns, append=bool(resume))
    eval_log = EvalLog(run_dir, logger.tb)
    report = tuple(config.eval.report)
    baseline_summary: dict | None = None

    def checkpoint_extra() -> dict:
        return {"plateau": tracker.state_dict()}

    def learner_actions(step):
        return trainer.act(step.obs, step.mask, deterministic=config.eval.deterministic)[0]

    def evaluate():
        """Score the networks on the seeds (and the baseline once per run); returns the next training STEP."""
        nonlocal baseline_summary

        if config.eval.baseline and baseline_summary is None:
            baseline_path = run_dir / "eval_baseline.json"
            key = {"policy": config.eval.baseline, "seed": config.eval.seed, "episodes": config.eval.episodes}
            cached = json.loads(baseline_path.read_text()) if baseline_path.exists() else None
            if cached and cached.get("key") == key:
                baseline_summary = cached["summary"]
            else:
                result, _ = run_evaluation(env, spec, learner_actions, config.eval.episodes, config.eval.seed,
                                           baseline=config.eval.baseline)
                baseline_summary = result.summary(report)
                baseline_path.write_text(json.dumps({"key": key, "summary": baseline_summary}, indent=2))
                eval_log.write(update, env_steps, result, baseline_summary, tracker)
                print(f"Baseline {config.eval.baseline}: score {result.score:.4g} over {result.episodes} seeded "
                      f"episodes ({result.seconds:.0f} s)", flush=True)

        result, next_step = run_evaluation(env, spec, learner_actions, config.eval.episodes, config.eval.seed)
        summary = result.summary(report)
        improved = tracker.observe(result.score, env_steps)
        eval_log.write(update, env_steps, result, summary, tracker)

        against = f", baseline {baseline_summary['score']:.4g}" if baseline_summary else ""
        print(f"Eval at {env_steps} env steps: score {result.score:.4g} (best {tracker.best:.4g}, "
              f"{tracker.evals_since_best} evals since){against}; {result.episodes} episodes in {result.seconds:.0f} s"
              f" [learner/baseline]\n{format_summary(summary, baseline_summary, report)}", flush=True)

        if improved:
            save_checkpoint(run_dir / "best.pt", trainer, config, spec, update, env_steps, checkpoint_extra())
            publish(trainer.actor.state_dict(), spec, f"best (update {update}, score {result.score:.4g})")
        return next_step

    step = env.reset()
    if evaluating and config.eval.at_start and not tracker.history:
        step = evaluate()
    last_eval_env_steps = tracker.history[-1][0] if tracker.history else env_steps

    finished_episodes: list[np.ndarray] = []
    finish_reason = None

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
                save_checkpoint(run_dir / f"checkpoint_{update:06d}.pt", trainer, config, spec, update, env_steps,
                                checkpoint_extra())
                save_checkpoint(run_dir / "latest.pt", trainer, config, spec, update, env_steps, checkpoint_extra())
                if not evaluating:
                    publish(trainer.actor.state_dict(), spec, f"update {update}")

            if evaluating and env_steps - last_eval_env_steps >= config.eval.every_env_steps:
                # The evaluation resets every env: the training episodes in progress are cut short, and the next
                # rollout starts from fresh ones (this rollout's advantages were already computed above).
                step = evaluate()
                last_eval_env_steps = env_steps
                if tracker.plateaued(env_steps, config.plateau.min_env_steps):
                    finish_reason = "plateau"
                    print(f"Plateau: no improvement in {tracker.evals_since_best} evaluations; best score "
                          f"{tracker.best:.4g} at {tracker.best_env_steps} env steps. Stopping.", flush=True)
                    break

        if finish_reason is None:
            finish_reason = "total_env_steps"
            # One last score, so the best model also considers the final networks.
            if evaluating and last_eval_env_steps < env_steps:
                step = evaluate()
                last_eval_env_steps = env_steps
    finally:
        save_checkpoint(run_dir / "latest.pt", trainer, config, spec, update, env_steps, checkpoint_extra())
        best_path = run_dir / "best.pt"
        if evaluating and best_path.exists():
            best = torch.load(best_path, map_location="cpu", weights_only=False)
            publish(best["trainer"]["actor"], spec, f"best (update {best['update']})")
        else:
            publish(trainer.actor.state_dict(), spec, f"update {update}")
        if finish_reason:
            finished_path.write_text(json.dumps({
                "reason": finish_reason,
                "env_steps": env_steps,
                "update": update,
                "best_score": tracker.best,
                "best_env_steps": tracker.best_env_steps,
            }, indent=2))
            print(f"{config.run_name} finished: {finish_reason}", flush=True)
        logger.close()
        env.close()


if __name__ == "__main__":
    main()
