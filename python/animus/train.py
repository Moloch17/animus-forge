"""Train a MAPPO policy against a running Animus Forge sim.

    python -m animus.train --config configs/stage1_duel.yaml --run-name stage1_duel

The worldserver starts this automatically when AnimusForge.Learner.AutoStart = 1, and passes where runs and layouts
go (AnimusForge.OutputDir). Run by hand, the client retries until the sim's socket appears. Every start trains from
scratch: an earlier run in <runs_dir>/<run_name>/ is archived first (animus.runs).

With eval.every_env_steps set, the networks are scored on seeded episodes as they train (see
animus.evaluation): the best-scoring networks are kept in best.pt. With convergence.patience set the stage ends
once the score converges; with a target set it only moves on once the best networks also pass it, restarting from
best.pt when stuck below it, and exits with code 3 (halting the queue) when the restarts run out (animus.stage).
"""

from __future__ import annotations

import argparse
import copy
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
from .evaluation import ConvergenceTracker, EvalResult, format_summary, run_evaluation
from .gates import validate_target
from .mappo.buffer import RolloutBuffer
from .mappo.trainer import MappoTrainer
from .runs import archive_run, prune_checkpoints
from .stage import ADVANCE, EXIT_BELOW_TARGET, HALT, RESTART, Outcome, StageController
from .stages import STAGE_FILE, load_stage


class RunLogger:
    """CSV always; TensorBoard when it is installed.

    Columns are fixed up front so metrics that only exist some updates (episode stats) are never
    dropped.
    """

    def __init__(self, run_dir: Path, columns: list[str]):
        self.csv_path = run_dir / "metrics.csv"
        self._csv_file = self.csv_path.open("w", newline="")
        self._csv_writer = csv.DictWriter(self._csv_file, fieldnames=columns, restval="", extrasaction="ignore")
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


class EvalLog:
    """eval.csv (one row per evaluation), eval.jsonl (the full summary, level bands included) and stage.jsonl (each
    decision to move on, restart or halt, with the gates behind it)."""

    COLUMNS = ["update", "env_steps", "policy", "episodes", "score", "stderr", "margin", "best", "evals_since_best",
               "restarts", "seconds"]

    def __init__(self, run_dir: Path, tb):
        self.csv_path = run_dir / "eval.csv"
        self.jsonl_path = run_dir / "eval.jsonl"
        self.stage_path = run_dir / "stage.jsonl"
        self.tb = tb

    def write(self, update: int, env_steps: int, result: EvalResult, summary: dict, tracker: ConvergenceTracker,
              restarts: int = 0) -> None:
        row = {
            "update": update,
            "env_steps": env_steps,
            "policy": result.policy,
            "episodes": result.episodes,
            "score": result.score,
            "stderr": result.stderr,
            "margin": tracker.last_margin,
            "best": tracker.best,
            "evals_since_best": tracker.evals_since_best,
            "restarts": restarts,
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
        self.tb.add_scalar("eval/margin", tracker.last_margin, env_steps)
        for band, values in summary.get("bands", {}).items():
            for name, value in values.items():
                if isinstance(value, float):
                    self.tb.add_scalar(f"eval_{band}/{name}", value, env_steps)

    def write_outcome(self, update: int, env_steps: int, outcome: Outcome, restarts: int) -> None:
        with self.stage_path.open("a") as f:
            f.write(json.dumps({
                "update": update,
                "env_steps": env_steps,
                "action": outcome.action,
                "reason": outcome.reason,
                "restarts": restarts,
                "judged": outcome.stage,
                "gates": outcome.gates.to_dict() if outcome.gates else None,
            }) + "\n")


def init_from_checkpoint(path: str) -> Path | None:
    """The configured seed checkpoint, or the latest.pt beside a best.pt that does not exist."""
    candidate = Path(path)
    if candidate.exists():
        return candidate
    if candidate.name == "best.pt" and (candidate.parent / "latest.pt").exists():
        return candidate.parent / "latest.pt"
    return None


def seed_everything(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True)
    parser.add_argument("--socket", help="sim socket path, overriding the config")
    parser.add_argument(
        "--run-name", help="run name (runs/<name>/), overriding the config; the sim passes its scenario"
    )
    parser.add_argument("--runs-dir", help="where runs go, overriding the config; the sim passes its own")
    parser.add_argument("--layouts-dir", help="where the sim writes layouts and stage.json, overriding the config")
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
    if args.runs_dir:
        config.runs_dir = args.runs_dir
    if args.layouts_dir:
        config.layouts_dir = args.layouts_dir
    seed_everything(config.seed)

    run_dir = Path(config.runs_dir) / config.run_name
    if archived := archive_run(run_dir):
        print(f"Archived the earlier {config.run_name} run to {archived}; training from scratch", flush=True)
    finished_path = run_dir / "finished.json"
    (run_dir / "config.yaml").write_text(yaml.safe_dump(config.to_dict(), sort_keys=False))

    print(f"Connecting to {config.socket} ...", flush=True)
    env = ForgeEnv(config.socket)
    spec = env.spec
    (run_dir / "spec.json").write_text(json.dumps(asdict(spec), indent=2))

    # The sim writes stage.json once it has built the scenario, which is before it accepts a learner.
    stage = load_stage(config.layouts_dir, spec.scenario)
    if stage is not None:
        (run_dir / STAGE_FILE).write_text(json.dumps(stage, indent=2))
    print(
        f"Scenario {spec.scenario}: {spec.num_envs} envs x {spec.agents_per_env} agents, {len(spec.layouts)} layouts "
        f"(obs up to {spec.obs_dim}, actions up to {spec.num_actions}), state {spec.state_dim}, decision every "
        f"{spec.decision_ms} ms",
        flush=True,
    )
    validate_target(config, spec.episode_info_names)

    trainer = MappoTrainer(
        [(layout.obs_dim, layout.num_actions) for layout in spec.layouts],
        spec.state_dim,
        config.mappo,
        train_device=config.resolved_train_device(),
        rollout_device=config.resolved_rollout_device(),
    )
    print(f"Updates on {trainer.train_device}, rollouts on {config.resolved_rollout_device()}", flush=True)

    evaluating = config.eval.every_env_steps > 0
    controller = StageController(config)
    tracker = controller.tracker

    update = 0
    env_steps = 0
    if candidates := config.resolved_init_from(stage):
        seed_path = next((path for c in candidates if (path := init_from_checkpoint(c))), None)
        if seed_path:
            checkpoint = torch.load(seed_path, map_location="cpu", weights_only=False)
            # Block positions for block-wise seeding: the checkpoint's own, else the stage.json of its run.
            if checkpoint.get("stage") is None and (seed_path.parent / STAGE_FILE).is_file():
                checkpoint["stage"] = json.loads((seed_path.parent / STAGE_FILE).read_text())
            seeded = seed_trainer(trainer, checkpoint, spec, stage)
            print(f"Seeded the networks from {seed_path}: trunk and {len(seeded)} of {len(spec.layouts)} layouts",
                  flush=True)
        else:
            print(f"None of {', '.join(candidates)} to seed from; starting from scratch", flush=True)

    envs, agents = spec.num_envs, spec.agents_per_env
    buffer = RolloutBuffer(config.rollout_length, envs, agents, spec.obs_dim, spec.state_dim, spec.num_actions)
    columns = [
        "update", "env_steps", "env_steps_per_sec", "update_seconds", "reward_per_decision", "episodes",
        *(f"episode_{name}" for name in spec.episode_info_names),
        "policy_loss", "value_loss", "entropy", "entropy_coef", "clip_frac", "approx_kl",
    ]
    logger = RunLogger(run_dir, columns)
    eval_log = EvalLog(run_dir, logger.tb)
    # Metric gates are checked on the summary, so their columns are summarised even when not reported.
    report = tuple(config.eval.report)
    report += tuple(name for name in config.target.metrics if name not in report)
    baselines: dict[tuple[int, int], dict] = {}
    best_path = run_dir / "best.pt"

    def checkpoint_extra() -> dict:
        # The stage (its block positions) travels with the checkpoint, for seeding the stages that extend it.
        return {"convergence": tracker.state_dict(), "restarts": controller.restarts, "stage": stage}

    def learner_actions(step):
        return trainer.act(step.obs, step.mask, step.layout, deterministic=config.eval.deterministic)[0]

    def baseline_for(seed: int, episodes: int) -> dict | None:
        """The eval.baseline policy's summary on these seeds, scored once per run."""
        if not config.eval.baseline:
            return None
        if (seed, episodes) in baselines:
            return baselines[seed, episodes]

        is_eval_seeds = (seed, episodes) == (config.eval.seed, config.eval.episodes)
        baseline_path = run_dir / ("eval_baseline.json" if is_eval_seeds else f"eval_baseline_{seed}_{episodes}.json")
        key = {"policy": config.eval.baseline, "seed": seed, "episodes": episodes}
        cached = json.loads(baseline_path.read_text()) if baseline_path.exists() else None
        if cached and cached.get("key") == key:
            summary = cached["summary"]
        else:
            result, _ = run_evaluation(env, spec, learner_actions, episodes, seed, baseline=config.eval.baseline)
            summary = result.summary(report)
            baseline_path.write_text(json.dumps({"key": key, "summary": summary}, indent=2))
            eval_log.write(update, env_steps, result, summary, tracker, controller.restarts)
            print(f"Baseline {config.eval.baseline}: score {result.score:.4g} over {result.episodes} seeded "
                  f"episodes (seed {seed}, {result.seconds:.0f} s)", flush=True)
        baselines[seed, episodes] = summary
        return summary

    def evaluate():
        """Score the networks on the seeds (and the baseline once per run); returns the next training STEP."""
        controller.baseline_summary = baseline_for(config.eval.seed, config.eval.episodes)
        baseline_summary = controller.baseline_summary

        result, next_step = run_evaluation(env, spec, learner_actions, config.eval.episodes, config.eval.seed)
        summary = result.summary(report)
        improved = controller.observe(summary, env_steps)
        eval_log.write(update, env_steps, result, summary, tracker, controller.restarts)

        against = f", baseline {baseline_summary['score']:.4g}" if baseline_summary else ""
        print(f"Eval at {env_steps} env steps: score {result.score:.4g} +/- {result.stderr:.2g} "
              f"(best {tracker.best:.4g}, {tracker.evals_since_best} evals since, margin {tracker.last_margin:.2g})"
              f"{against}; "
              f"{result.episodes} episodes in {result.seconds:.0f} s"
              f" [learner/baseline]\n{format_summary(summary, baseline_summary, report)}", flush=True)

        if improved:
            save_checkpoint(best_path, trainer, config, spec, update, env_steps, checkpoint_extra())
        return next_step

    def confirm_best() -> tuple[dict, dict | None]:
        """Score best.pt on the held-out confirmation seeds, then put the training networks back."""
        nonlocal step
        target = config.target
        training_state = copy.deepcopy(trainer.state_dict())
        trainer.load_state_dict(torch.load(best_path, map_location="cpu", weights_only=False)["trainer"],
                                load_optimizers=False)
        try:
            baseline = baseline_for(target.confirm_seed, target.confirm_episodes)
            result, step = run_evaluation(env, spec, learner_actions, target.confirm_episodes, target.confirm_seed)
        finally:
            trainer.load_state_dict(training_state)
        result.policy = "confirm"
        summary = result.summary(report)
        eval_log.write(update, env_steps, result, summary, tracker, controller.restarts)
        print(f"Confirmation of best.pt on {result.episodes} held-out episodes: score {result.score:.4g} "
              f"+/- {result.stderr:.2g}\n{format_summary(summary, baseline, report)}", flush=True)
        return summary, baseline

    def restart_from_best() -> None:
        r = config.restarts
        trainer.load_state_dict(torch.load(best_path, map_location="cpu", weights_only=False)["trainer"],
                                load_optimizers=False)
        seed_everything(config.seed + controller.restarts + 1)
        if r.shrink != 1.0 or r.perturb != 0.0:
            trainer.shrink_perturb(r.shrink, r.perturb)
        if r.reset_optimizers:
            trainer.reset_optimizers()
        controller.record_restart(env_steps)

    def handle(outcome: Outcome) -> bool:
        """Carry out the controller's decision; True when training stops."""
        if outcome.action not in (ADVANCE, RESTART, HALT):
            return False
        eval_log.write_outcome(update, env_steps, outcome, controller.restarts)
        failures = "; ".join(outcome.gates.failures) if outcome.gates else ""
        if outcome.action == RESTART:
            restart_from_best()
            print(f"Converged below the target ({outcome.stage}: {failures}). Restart {controller.restarts} of "
                  f"{config.restarts.max_restarts} from best.pt (score {tracker.best:.4g}), entropy_coef "
                  f"{controller.entropy_coef(env_steps):.3g}.", flush=True)
            return False
        if outcome.action == HALT:
            print(f"Below the target after {controller.restarts} restarts ({outcome.stage}: {failures}); best score "
                  f"{tracker.best:.4g} at {tracker.best_env_steps} env steps. Stopping; the queue halts here.",
                  flush=True)
        else:
            print(f"Stage complete ({outcome.reason}): best score {tracker.best:.4g} at {tracker.best_env_steps} env "
                  f"steps after {controller.restarts} restarts.", flush=True)
        return True

    step = env.reset()
    if evaluating and config.eval.at_start and not tracker.history:
        step = evaluate()
    last_eval_env_steps = tracker.history[-1][0] if tracker.history else env_steps

    finished_episodes: list[np.ndarray] = []
    # A party seat left empty for an episode reports present = 0; its row is not an episode.
    present = spec.episode_info_names.index("present") if "present" in spec.episode_info_names else None
    outcome: Outcome | None = None

    try:
        while env_steps < config.total_env_steps:
            buffer.reset()
            started = time.perf_counter()
            trainer.entropy_coef = controller.entropy_coef(env_steps)

            while not buffer.full:
                values = trainer.value(step.state, step.obs, step.layout)
                actions, log_probs = trainer.act(step.obs, step.mask, step.layout)
                buffer.add_decision(step.obs, step.state, step.mask, step.layout, actions, log_probs, values)

                # The ended episodes' layouts: the next STEP already carries the new episodes'.
                layout = step.layout
                step = env.step(actions)

                final_values = np.zeros((envs, agents), dtype=np.float32)
                if step.done.any():
                    final_values[step.done] = trainer.value(step.final_state, step.final_obs, layout)[step.done]
                    ended = step.episode_info[step.done].reshape(-1, spec.episode_info_dim)
                    finished_episodes.extend(ended if present is None else ended[ended[:, present] > 0.0])

                buffer.add_outcome(step.reward, step.done, step.terminated, final_values)

            rollout_seconds = time.perf_counter() - started
            buffer.finish(trainer.value(step.state, step.obs, step.layout), config.mappo.gamma, config.mappo.gae_lambda)
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
                    "entropy_coef": trainer.entropy_coef,
                }
                if finished_episodes:
                    means = np.mean(finished_episodes, axis=0)
                    for name, value in zip(spec.episode_info_names, means):
                        row[f"episode_{name}"] = float(value)
                row.update(stats)

                logger.log(update, row)
                summary = ", ".join(
                    f"{k} {v:.4g}" for k, v in row.items() if k.startswith("episode_") or k in ("entropy", "value_loss")
                )
                print(f"update {update} | steps {env_steps} | {row['env_steps_per_sec']:.0f} sps | {summary}",
                      flush=True)
                finished_episodes.clear()

            if update % config.checkpoint_every == 0:
                save_checkpoint(run_dir / f"checkpoint_{update:06d}.pt", trainer, config, spec, update, env_steps,
                                checkpoint_extra())
                save_checkpoint(run_dir / "latest.pt", trainer, config, spec, update, env_steps, checkpoint_extra())
                prune_checkpoints(run_dir, config.keep_checkpoints)

            if evaluating and env_steps - last_eval_env_steps >= config.eval.every_env_steps:
                # The evaluation resets every env: the training episodes in progress are cut short, and the next
                # rollout starts from fresh ones (this rollout's advantages were already computed above).
                step = evaluate()
                last_eval_env_steps = env_steps
                if handle(decision := controller.after_eval(env_steps, confirm_best)):
                    outcome = decision
                    break

        if outcome is None:
            # One last score, so the best model also considers the final networks.
            if evaluating and last_eval_env_steps < env_steps:
                step = evaluate()
                last_eval_env_steps = env_steps
            handle(outcome := controller.at_budget(confirm_best))
    finally:
        save_checkpoint(run_dir / "latest.pt", trainer, config, spec, update, env_steps, checkpoint_extra())
        if outcome:
            finished_path.write_text(json.dumps({
                "reason": outcome.reason,
                "advanced": outcome.action == ADVANCE,
                "env_steps": env_steps,
                "update": update,
                "best_score": tracker.best,
                "best_env_steps": tracker.best_env_steps,
                "restarts": controller.restarts,
                "judged": outcome.stage,
                "gates": outcome.gates.to_dict() if outcome.gates else None,
            }, indent=2))
            print(f"{config.run_name} finished: {outcome.reason}", flush=True)
        logger.close()
        env.close()

    return 0 if outcome.action == ADVANCE else EXIT_BELOW_TARGET


if __name__ == "__main__":
    raise SystemExit(main())
