"""Train a MAPPO policy against a running Animus Forge sim.

    python -m animus.train --config configs/stage1_duel.yaml --run-name stage1_duel

The worldserver starts this when told to (`forge start`, `forge resume`, `forge run`) and
AnimusForge.Learner.AutoStart = 1, and passes where runs and layouts go (AnimusForge.OutputDir). Run by hand, the client
retries until the sim's socket appears. A start trains from scratch: an earlier run in <runs_dir>/<run_name>/ is
archived first (animus.runs). With --resume the run continues from its latest.pt instead, as long as the scenario's
layouts and dimensions are unchanged.

Progress (steps, losses, evaluation scores, restarts) is kept in <runs_dir>/<run_name>/progress.json for the sim's
console (animus.progress).

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
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import asdict
from pathlib import Path

import numpy as np
import torch
import yaml

from .bootstrap import seed_merges, seed_trainer
from .config import TrainConfig
from .distill import Distiller, auto_teachers, build_teacher
from .env import ForgeEnv
from .evaluation import (DERIVED_METRICS, ConvergenceTracker, EvalResult, format_summary, layout_weights,
                         run_evaluation)
from .gates import validate_target
from .mappo.buffer import RolloutBuffer
from .mappo.trainer import MappoTrainer, horizon_seconds, per_decision
from .progress import ProgressWriter
from .runs import FINISHED_FILE, archive_run, prune_checkpoints, resume_checkpoint_path, resume_mismatch
from .stage import ADVANCE, EXIT_BELOW_TARGET, EXTEND, HALT, RESTART, Outcome, StageController
from .stages import STAGE_FILE, load_stage


class RunLogger:
    """CSV always; TensorBoard when it is installed.

    Columns are fixed up front so metrics that only exist some updates (episode stats) are never
    dropped. A resumed run appends to its metrics.csv under the header already there.
    """

    def __init__(self, run_dir: Path, columns: list[str], append: bool = False):
        self.csv_path = run_dir / "metrics.csv"
        existing = append and self.csv_path.exists() and self.csv_path.stat().st_size > 0
        if existing:
            with self.csv_path.open(newline="") as f:
                columns = next(csv.reader(f))
        self._csv_file = self.csv_path.open("a" if existing else "w", newline="")
        self._csv_writer = csv.DictWriter(self._csv_file, fieldnames=columns, restval="", extrasaction="ignore")
        if not existing:
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
    """eval.csv (one row per evaluation), eval.jsonl (the full summary, level bands included), eval_episodes.jsonl
    (one row per scored episode) and stage.jsonl (each decision to move on, restart or halt, with the gates behind
    it)."""

    COLUMNS = ["update", "env_steps", "policy", "episodes", "score", "stderr", "margin", "best", "evals_since_best",
               "restarts", "seconds"]

    def __init__(self, run_dir: Path, tb):
        self.csv_path = run_dir / "eval.csv"
        self.jsonl_path = run_dir / "eval.jsonl"
        self.episodes_path = run_dir / "eval_episodes.jsonl"
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

        # The episodes behind the summary, every episode info column of each: which seeds a class/role failed, not
        # just that its mean is low, and what those episodes have in common.
        with self.episodes_path.open("a") as f:
            for episode in result.episodes_log():
                f.write(json.dumps({"update": update, "env_steps": env_steps, **episode}) + "\n")

        if self.tb is None or result.policy != "learner":
            return
        for name, value in summary.items():
            if isinstance(value, float):
                self.tb.add_scalar(f"eval/{name}", value, env_steps)
        self.tb.add_scalar("eval/margin", tracker.last_margin, env_steps)
        groups = [*summary.get("bands", {}).items(),
                  *((f"arena_{arena}", values) for arena, values in summary.get("arenas", {}).items()),
                  *((f"build_{build}", values) for build, values in summary.get("builds", {}).items())]
        for group, values in groups:
            for name, value in values.items():
                if isinstance(value, float):
                    self.tb.add_scalar(f"eval_{group}/{name}", value, env_steps)

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


def load_parent(path: Path) -> dict:
    """A parent stage's checkpoint, with the stage.json of its run when it carries none (for block positions)."""
    checkpoint = torch.load(path, map_location="cpu", weights_only=False)
    if checkpoint.get("stage") is None and (path.parent / STAGE_FILE).is_file():
        checkpoint["stage"] = json.loads((path.parent / STAGE_FILE).read_text())
    return checkpoint


def make_distiller(config: TrainConfig, spec, stage: dict | None, parents: list[dict], device) -> Distiller | None:
    """The distillation loss for distill.teachers: auto (from `parents`, extended stage first) or named checkpoints."""
    if not config.distill.teachers:
        return None

    named = config.named_teachers()
    if named:
        chosen = {}
        for arena, candidate in named.items():
            path = init_from_checkpoint(candidate)
            if path is None:
                print(f"Teacher {candidate} for arena {arena} does not exist; that arena is not distilled", flush=True)
                continue
            chosen[arena] = load_parent(path)
    else:
        chosen = auto_teachers(stage, parents)

    if not chosen:
        print("Distillation is on, but no arena has a teacher", flush=True)
        return None

    teachers = {arena: build_teacher(checkpoint, spec, stage, device) for arena, checkpoint in chosen.items()}
    for arena, teacher in teachers.items():
        print(f"Arena {arena} is taught by {teacher.name} ({len(teacher.layouts)} of {len(spec.layouts)} layouts)",
              flush=True)
    return Distiller(stage, teachers)


def use_threads(threads: int) -> None:
    """CPU threads for torch (0 = its own default): the learner runs beside the sim's map update threads."""
    if threads > 0:
        torch.set_num_threads(threads)


def seed_everything(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)


class TrainingRun:
    """One learner run against the sim, from connecting to finished.json.

    The state the phases share -- the networks, the current STEP, the update and env step counters, the stage
    controller -- lives here, so rollouts, evaluation, confirmation and restarts are methods rather than closures over
    one long function.
    """

    def __init__(self, config: TrainConfig, resume: bool):
        self.config = config
        use_threads(config.torch_threads)
        seed_everything(config.seed)

        self.run_dir = Path(config.runs_dir) / config.run_name
        self.resume_path: Path | None = None
        if resume:
            try:
                self.resume_path = resume_checkpoint_path(self.run_dir)
            except FileNotFoundError as error:
                raise SystemExit(str(error)) from None
        elif archived := archive_run(self.run_dir):
            print(f"Archived the earlier {config.run_name} run to {archived}; training from scratch", flush=True)
        self.finished_path = self.run_dir / FINISHED_FILE
        if self.resume_path:
            self.finished_path.unlink(missing_ok=True)
        (self.run_dir / "config.yaml").write_text(yaml.safe_dump(config.to_dict(), sort_keys=False))

        print(f"Connecting to {config.socket} ...", flush=True)
        self.env = ForgeEnv(config.socket)
        self.spec = spec = self.env.spec
        (self.run_dir / "spec.json").write_text(json.dumps(asdict(spec), indent=2))

        # The sim writes stage.json once it has built the scenario, which is before it accepts a learner.
        self.stage = load_stage(config.layouts_dir, spec.scenario)
        if self.stage is not None:
            (self.run_dir / STAGE_FILE).write_text(json.dumps(self.stage, indent=2))
        print(
            f"Scenario {spec.scenario}: {spec.num_envs} envs x {spec.agents_per_env} agents, {len(spec.layouts)} "
            f"layouts (obs up to {spec.obs_dim}, actions up to {spec.num_actions}), state {spec.state_dim}, decision "
            f"every {spec.decision_ms} ms",
            flush=True,
        )
        self.arena_names = tuple(arena["name"] for arena in (self.stage or {}).get("arenas", ()))
        validate_target(config, spec.episode_info_names, self.arena_names if self.stage and "arenas" in self.stage
                        else None)

        self.trainer = MappoTrainer(
            [(layout.obs_dim, layout.num_actions) for layout in spec.layouts],
            spec.state_dim,
            config.mappo,
            train_device=config.resolved_train_device(),
            rollout_device=config.resolved_rollout_device(),
        )
        print(f"Updates on {self.trainer.train_device}, rollouts on {config.resolved_rollout_device()}", flush=True)

        # Horizons are configured in game time; each decision compounds them.
        self.discounts = per_decision(config.mappo, spec.decision_ms)
        gamma, gae_lambda = self.discounts
        print(
            f"Per {spec.decision_ms} ms decision: gamma {gamma:.5f} (horizon "
            f"{horizon_seconds(gamma, spec.decision_ms):.0f} s), GAE trace {gamma * gae_lambda:.5f} (credit "
            f"{horizon_seconds(gamma * gae_lambda, spec.decision_ms):.1f} s), rollout "
            f"{config.rollout_length * spec.decision_ms / 1000.0:.1f} s",
            flush=True,
        )

        self.evaluating = config.eval.every_env_steps > 0
        self.controller = StageController(config)
        self.tracker = self.controller.tracker
        self.update = 0
        self.env_steps = 0
        self._load_or_seed()

        def new_buffer() -> RolloutBuffer:
            return RolloutBuffer(config.rollout_length, spec.num_envs, spec.agents_per_env, spec.obs_dim,
                                 spec.state_dim, spec.num_actions)

        self.buffer = new_buffer()
        # Overlapped updates fill this one while the update reads the other; they swap after every rollout.
        self.spare_buffer = new_buffer() if config.overlap_updates else None
        self.pending_update: Future | None = None
        self.carried_stats: dict[str, float] | None = None  # an update drained outside a rollout, still to log
        self.rollout_reward = 0.0
        self.rollout_allowed_actions = 0.0
        self.started_at = time.perf_counter()
        self.updater = ThreadPoolExecutor(max_workers=1, thread_name_prefix="update") \
            if config.overlap_updates else None
        columns = [
            "update", "env_steps", "env_steps_per_sec", "update_seconds", "reward_per_decision", "episodes",
            *(f"episode_{name}" for name in spec.episode_info_names),
            "policy_loss", "value_loss", "entropy", "entropy_coef", "clip_frac", "approx_kl",
            "explained_variance", "actor_grad_norm", "critic_grad_norm", "epochs_run", "allowed_actions",
            "elapsed_seconds", "update_compute_seconds", "distill_coef", "distill_kl", "distill_rows",
        ]
        self.logger = RunLogger(self.run_dir, columns, append=self.resume_path is not None)
        # Metric gates are checked on the summary, so their columns are summarised even when not reported.
        report = tuple(config.eval.report)
        gated = (*config.target.metrics, *config.target.layout_metrics,
                 *(name for gates in config.target.arenas.values() for name in gates.get("metrics", {})))
        # Derived fields are computed by the summary itself, not averaged from an episode info column.
        report += tuple(name for name in gated if name not in report and name not in DERIVED_METRICS)
        self.report = report
        self.eval_log = EvalLog(self.run_dir, self.logger.tb)
        # Self-play arenas scored against the baseline as their opponent (eval.opponent_baseline).
        self.opponents = config.eval.baseline if config.eval.opponent_baseline else ""
        self.baselines: dict[tuple[int, int], dict] = {}
        self.best_path = self.run_dir / "best.pt"

        self.progress = ProgressWriter(self.run_dir, config, spec, resumed_update=self.update,
                                       resumed_env_steps=self.env_steps)
        cached_baseline_score = None
        if self.resume_path and (self.run_dir / "eval_baseline.json").exists():
            cached = json.loads((self.run_dir / "eval_baseline.json").read_text())
            cached_baseline_score = cached.get("summary", {}).get("score")
        self.progress.restore_evaluation(self.tracker, cached_baseline_score, self.controller)

        self.step = None
        self.last_eval_env_steps = 0
        self.finished_episodes: list[np.ndarray] = []
        # A party seat left empty for an episode reports present = 0; its row is not an episode.
        names = spec.episode_info_names
        self.present_column = names.index("present") if "present" in names else None

    # ------------------------------------------------------------------ setup

    def _load_or_seed(self) -> None:
        """Resume the run's latest.pt, or seed the fresh networks from the stage this one extends and a merge stage's
        further parents; either way the parents teach a distilled run (self.distiller)."""
        config, spec = self.config, self.spec
        if self.resume_path:
            checkpoint = torch.load(self.resume_path, map_location="cpu", weights_only=False)
            if mismatch := resume_mismatch(checkpoint.get("spec", {}), asdict(spec)):
                raise SystemExit(f"cannot resume {config.run_name}: the scenario's {', '.join(mismatch)} changed since "
                                 f"{self.resume_path} was saved; start it fresh instead")
            self.trainer.load_state_dict(checkpoint["trainer"])
            self.update = int(checkpoint.get("update", 0))
            self.env_steps = int(checkpoint.get("env_steps", 0))
            # The convergence test, the restarts and the best evaluation carry on where the run stopped.
            self.tracker.load_state_dict(checkpoint.get("convergence"))
            self.controller.load_state_dict(checkpoint.get("controller"))
            print(f"Resumed {config.run_name} from {self.resume_path} at update {self.update}, {self.env_steps} env "
                  f"steps ({self.controller.restarts} restarts)", flush=True)

        # The parents: the extended stage's checkpoint (the first init_from candidate that exists) and a merge stage's
        # further parents.
        candidates = config.resolved_init_from(self.stage)
        base_path = next((path for c in candidates if (path := init_from_checkpoint(c))), None)
        merge_paths = []
        for candidate in config.resolved_merge_from(self.stage):
            if path := init_from_checkpoint(candidate):
                merge_paths.append(path)
            else:
                print(f"Merged stage checkpoint {candidate} does not exist: nothing is seeded or taught from it",
                      flush=True)
        base = load_parent(base_path) if base_path else None
        merged = [load_parent(path) for path in merge_paths]

        if not self.resume_path and base is not None:
            seeded = seed_trainer(self.trainer, base, spec, self.stage)
            print(f"Seeded the networks from {base_path}: trunk and {len(seeded)} of {len(spec.layouts)} layouts",
                  flush=True)
            for layout, blocks in (seed_merges(self.trainer, merged, spec, self.stage, base) if merged else {}).items():
                print(f"  {layout}: {', '.join(blocks)} from the merged stages", flush=True)
        elif not self.resume_path and candidates:
            print(f"None of {', '.join(candidates)} to seed from; starting from scratch", flush=True)

        self.distiller = make_distiller(config, spec, self.stage, [p for p in (base, *merged) if p is not None],
                                        self.trainer.train_device)

    # ------------------------------------------------------------------ checkpoints

    def _checkpoint_extra(self) -> dict:
        # The stage (its block positions) travels with the checkpoint, for seeding the stages that extend it.
        return {"convergence": self.tracker.state_dict(), "controller": self.controller.state_dict(),
                "stage": self.stage}

    def _save(self, path: Path) -> None:
        self.drain_update()
        save_checkpoint(path, self.trainer, self.config, self.spec, self.update, self.env_steps,
                        self._checkpoint_extra())

    def maybe_checkpoint(self) -> None:
        if self.update % self.config.checkpoint_every == 0:
            self._save(self.run_dir / f"checkpoint_{self.update:06d}.pt")
            self._save(self.run_dir / "latest.pt")
            prune_checkpoints(self.run_dir, self.config.keep_checkpoints)

    # ------------------------------------------------------------------ evaluation

    def learner_actions(self, step):
        return self.trainer.act(step.obs, step.mask, step.layout, deterministic=self.config.eval.deterministic)[0]

    def baseline_for(self, seed: int, episodes: int) -> dict | None:
        """The eval.baseline policy's summary on these seeds, scored once per run."""
        config = self.config
        if not config.eval.baseline:
            return None
        if (seed, episodes) in self.baselines:
            return self.baselines[seed, episodes]

        is_eval_seeds = (seed, episodes) == (config.eval.seed, config.eval.episodes)
        baseline_path = self.run_dir / ("eval_baseline.json" if is_eval_seeds
                                        else f"eval_baseline_{seed}_{episodes}.json")
        key = {"policy": config.eval.baseline, "seed": seed, "episodes": episodes, "opponents": self.opponents,
               "arenas": list(self.arena_names),
               # The tuning prices the reward terms the baseline is scored in: a change must score it again.
               "tuning": (self.stage or {}).get("tuning")}
        cached = json.loads(baseline_path.read_text()) if baseline_path.exists() else None
        if cached and cached.get("key") == key:
            summary = cached["summary"]
        else:
            result, _ = run_evaluation(self.env, self.spec, self.learner_actions, episodes, seed,
                                       baseline=config.eval.baseline, opponents=self.opponents,
                                       arenas=self.arena_names)
            summary = result.summary(self.report)
            baseline_path.write_text(json.dumps({"key": key, "summary": summary}, indent=2))
            self.eval_log.write(self.update, self.env_steps, result, summary, self.tracker, self.controller.restarts)
            print(f"Baseline {config.eval.baseline}: score {result.score:.4g} over {result.episodes} seeded "
                  f"episodes (seed {seed}, {result.seconds:.0f} s)", flush=True)
        self.baselines[seed, episodes] = summary
        return summary

    def evaluate(self) -> None:
        """Score the networks on the seeds (and the baseline once per run); the next training STEP becomes current."""
        self.drain_update()
        config, tracker, controller = self.config, self.tracker, self.controller
        controller.baseline_summary = self.baseline_for(config.eval.seed, config.eval.episodes)
        baseline_summary = controller.baseline_summary

        self.progress.write("evaluating", self.update, self.env_steps)
        result, self.step = run_evaluation(self.env, self.spec, self.learner_actions, config.eval.episodes,
                                           config.eval.seed, opponents=self.opponents, arenas=self.arena_names)
        summary = result.summary(self.report)
        improved = controller.observe(summary, self.env_steps)
        self.eval_log.write(self.update, self.env_steps, result, summary, tracker, controller.restarts)
        self.progress.evaluated(self.env_steps, result.score, baseline_summary["score"] if baseline_summary else None,
                                tracker, controller)
        self.progress.write("training", self.update, self.env_steps)
        self.last_eval_env_steps = self.env_steps

        against = f", baseline {baseline_summary['score']:.4g}" if baseline_summary else ""
        print(f"Eval at {self.env_steps} env steps: score {result.score:.4g} +/- {result.stderr:.2g} "
              f"(best {tracker.best:.4g}, {tracker.evals_since_best} evals since, margin {tracker.last_margin:.2g})"
              f"{against}; "
              f"{result.episodes} episodes in {result.seconds:.0f} s"
              f" [learner/baseline]\n{format_summary(summary, baseline_summary, self.report)}", flush=True)

        if improved:
            self._save(self.best_path)

        self.send_layout_weights(summary, baseline_summary)
        self.send_replay(result)

    def send_replay(self, result: EvalResult) -> None:
        """Send the sim the seeds this evaluation lost, for training resets to rebuild (protocol REPLAY)."""
        sampling = self.config.layout_sampling
        if not sampling.enabled or sampling.replay_fraction <= 0.0 or not sampling.metric:
            return

        seeds = result.failed_seeds(sampling.metric)
        self.env.set_replay(self.config.eval.seed, sampling.replay_fraction, seeds)
        print(f"Replaying {len(seeds)} lost evaluation episodes in {sampling.replay_fraction:.0%} of training resets",
              flush=True)

    def send_layout_weights(self, summary: dict, baseline: dict | None) -> None:
        """Weight the training episodes toward the class/roles furthest below their baseline (protocol WEIGHTS)."""
        sampling = self.config.layout_sampling
        if not sampling.enabled or baseline is None or not summary.get("layouts"):
            return

        weights = layout_weights(summary, baseline, sampling.strength, sampling.max_ratio, sampling.metric)
        names = [layout.name for layout in self.spec.layouts]
        self.env.set_layout_weights([weights.get(name, 1.0) for name in names])
        heaviest = sorted(weights.items(), key=lambda item: -item[1])[:3]
        print("Layout weights: " + ", ".join(f"{name} {weight:.2f}" for name, weight in heaviest)
              + f" (of {len(weights)} class/roles)", flush=True)

    def confirm_best(self) -> tuple[dict, dict | None]:
        """Score best.pt on the held-out confirmation seeds, then put the training networks back."""
        self.drain_update()
        target, trainer = self.config.target, self.trainer
        training_state = copy.deepcopy(trainer.state_dict())
        trainer.load_state_dict(torch.load(self.best_path, map_location="cpu", weights_only=False)["trainer"],
                                load_optimizers=False)
        try:
            baseline = self.baseline_for(target.confirm_seed, target.confirm_episodes)
            result, self.step = run_evaluation(self.env, self.spec, self.learner_actions, target.confirm_episodes,
                                               target.confirm_seed, opponents=self.opponents,
                                               arenas=self.arena_names)
        finally:
            trainer.load_state_dict(training_state)
        result.policy = "confirm"
        summary = result.summary(self.report)
        self.eval_log.write(self.update, self.env_steps, result, summary, self.tracker, self.controller.restarts)
        print(f"Confirmation of best.pt on {result.episodes} held-out episodes: score {result.score:.4g} "
              f"+/- {result.stderr:.2g}\n{format_summary(summary, baseline, self.report)}", flush=True)
        return summary, baseline

    # ------------------------------------------------------------------ stage decisions

    def restart_from_best(self) -> None:
        self.drain_update()
        r = self.config.restarts
        self.trainer.load_state_dict(torch.load(self.best_path, map_location="cpu", weights_only=False)["trainer"],
                                     load_optimizers=False)
        seed_everything(self.config.seed + self.controller.restarts + 1)
        if r.shrink != 1.0 or r.perturb != 0.0:
            self.trainer.shrink_perturb(r.shrink, r.perturb)
        if r.reset_optimizers:
            self.trainer.reset_optimizers()
        self.controller.record_restart(self.env_steps)

    def handle(self, outcome: Outcome) -> bool:
        """Carry out the controller's decision; True when training stops."""
        if outcome.action not in (ADVANCE, RESTART, HALT, EXTEND):
            return False
        tracker, controller = self.tracker, self.controller
        self.eval_log.write_outcome(self.update, self.env_steps, outcome, controller.restarts)
        failures = "; ".join(outcome.gates.failures) if outcome.gates else ""
        if outcome.action == RESTART:
            self.restart_from_best()
            print(f"Converged below the target ({outcome.stage}: {failures}). Restart {controller.restarts} of "
                  f"{self.config.restarts.max_restarts} from best.pt (score {tracker.best:.4g}), entropy_coef "
                  f"{controller.entropy_coef(self.env_steps):.3g}.", flush=True)
            return False
        if outcome.action == EXTEND:
            controller.record_extension(self.env_steps)
            print(f"Converged below the target ({outcome.stage}: {failures}) with {controller.restarts} restarts used; "
                  f"training on until it passes or total_env_steps (target.until_passed).", flush=True)
            return False
        if outcome.action == HALT:
            print(f"Below the target after {controller.restarts} restarts ({outcome.stage}: {failures}); best score "
                  f"{tracker.best:.4g} at {tracker.best_env_steps} env steps. Stopping; the queue halts here.",
                  flush=True)
        else:
            print(f"Stage complete ({outcome.reason}): best score {tracker.best:.4g} at {tracker.best_env_steps} env "
                  f"steps after {controller.restarts} restarts.", flush=True)
        return True

    # ------------------------------------------------------------------ training

    def finish_update(self) -> dict[str, float] | None:
        """Wait for an overlapped update to finish and hand its weights to the rollout networks. None if none ran."""
        if self.pending_update is None:
            return None

        pending, self.pending_update = self.pending_update, None
        stats = pending.result()  # an update that raised re-raises here, on the training thread
        self.trainer.sync_rollout()
        return stats

    def drain_update(self) -> None:
        """Finish any overlapped update, so the networks are whole: before an evaluation, a checkpoint or a restart.
        Its stats are kept for the next logged row."""
        if (stats := self.finish_update()) is not None:
            self.carried_stats = stats

    def rollout(self) -> tuple[dict[str, float], float, float]:
        """Fill the buffer from the sim and update the networks; returns (update stats, start time, rollout s)."""
        spec, trainer, buffer = self.spec, self.trainer, self.buffer
        envs, agents = spec.num_envs, spec.agents_per_env
        buffer.reset()
        started = time.perf_counter()

        while not buffer.full:
            step = self.step
            values = trainer.value(step.state, step.obs, step.layout)
            actions, log_probs = trainer.act(step.obs, step.mask, step.layout)
            buffer.add_decision(step.obs, step.state, step.mask, step.layout, actions, log_probs, values, step.present)

            # The ended episodes' layouts: the next STEP already carries the new episodes'.
            layout = step.layout
            self.step = step = self.env.step(actions)

            final_values = np.zeros((envs, agents), dtype=np.float32)
            if step.done.any():
                # Only the envs that finished need one: an env ends an episode once in hundreds of decisions, so
                # valuing all of them and then throwing most away is a forward pass over ~20x the rows needed.
                done = step.done
                final_values[done] = trainer.value(step.final_state[done], step.final_obs[done], layout[done])
                ended = step.episode_info[step.done].reshape(-1, spec.episode_info_dim)
                present = self.present_column
                self.finished_episodes.extend(ended if present is None else ended[ended[:, present] > 0.0])

            buffer.add_outcome(step.reward, step.done, step.terminated, final_values)

        rollout_seconds = time.perf_counter() - started
        buffer.finish(trainer.value(self.step.state, self.step.obs, self.step.layout), *self.discounts)
        # Read before the buffers swap below: log_update runs on the rollout that has just been collected.
        self.rollout_reward = buffer.mean_reward()
        self.rollout_allowed_actions = buffer.mean_allowed_actions()
        trainer.entropy_coef = self.controller.entropy_coef(self.env_steps)
        if self.distiller is not None:
            self.distiller.coef = self.config.distill.coef_at(self.env_steps)

        self.update += 1
        self.env_steps += self.config.rollout_length * envs * agents

        if self.updater is None:
            stats = trainer.update(buffer, self.distiller)
            return stats, started, rollout_seconds

        # Overlapped: the update of the rollout before last has been running while this one was collected. Take its
        # stats and its weights, then hand this rollout to the worker and collect the next one meanwhile. The
        # networks are synced on the join, so the rollout that follows acts on the weights of the update before it.
        stats = self.finish_update()
        self.pending_update = self.updater.submit(trainer.update, buffer, self.distiller, sync=False)
        self.buffer, self.spare_buffer = self.spare_buffer, buffer
        if stats is None:
            stats, self.carried_stats = self.carried_stats, None
        if stats is None:
            # The very first update has nothing to overlap with; wait for it, so every update has a logged row.
            stats = self.finish_update()
        return stats, started, rollout_seconds

    def log_update(self, stats: dict[str, float], started: float, rollout_seconds: float) -> None:
        config, spec = self.config, self.spec
        if self.update % config.log_every != 0:
            return

        row: dict[str, float] = {
            "update": self.update,
            "env_steps": self.env_steps,
            "env_steps_per_sec": config.rollout_length * spec.num_envs * spec.agents_per_env / rollout_seconds,
            "update_seconds": time.perf_counter() - started - rollout_seconds,
            "reward_per_decision": self.rollout_reward,
            # Entropy is only readable against how many actions were legal to begin with.
            "allowed_actions": self.rollout_allowed_actions,
            "elapsed_seconds": time.perf_counter() - self.started_at,
            "episodes": len(self.finished_episodes),
            "entropy_coef": self.trainer.entropy_coef,
            **({"distill_coef": self.distiller.coef} if self.distiller is not None else {}),
        }
        if self.finished_episodes:
            means = np.mean(self.finished_episodes, axis=0)
            for name, value in zip(spec.episode_info_names, means):
                row[f"episode_{name}"] = float(value)
        row.update(stats)

        # The floor reads the entropy this update reached against how many actions were legal for it.
        if "entropy" in row:
            self.controller.observe_entropy(row["entropy"], self.rollout_allowed_actions)

        self.logger.log(self.update, row)
        self.progress.training(row)
        self.progress.write("training", self.update, self.env_steps)
        summary = ", ".join(
            f"{k} {v:.4g}" for k, v in row.items() if k.startswith("episode_") or k in ("entropy", "value_loss")
        )
        print(f"update {self.update} | steps {self.env_steps} | {row['env_steps_per_sec']:.0f} sps | {summary}",
              flush=True)
        self.finished_episodes.clear()

    def train(self) -> Outcome:
        """Train until the stage decides to move on or halt, or the step budget runs out."""
        config, controller = self.config, self.controller
        while self.env_steps < config.total_env_steps:
            stats, started, rollout_seconds = self.rollout()
            self.log_update(stats, started, rollout_seconds)
            self.maybe_checkpoint()

            if self.evaluating and self.env_steps - self.last_eval_env_steps >= config.eval.every_env_steps:
                # The evaluation resets every env: the training episodes in progress are cut short, and the next
                # rollout starts from fresh ones (this rollout's advantages were already computed above).
                self.evaluate()
                if self.handle(decision := controller.after_eval(self.env_steps, self.confirm_best)):
                    return decision

        # One last score, so the best model also considers the final networks.
        if self.evaluating and self.last_eval_env_steps < self.env_steps:
            self.evaluate()
        if self.evaluating and controller.baseline_summary is None:
            # A run resumed at its budget judges without evaluating: the baseline is cached in eval_baseline.json.
            controller.baseline_summary = self.baseline_for(config.eval.seed, config.eval.episodes)
        outcome = controller.at_budget(self.confirm_best)
        self.handle(outcome)
        return outcome

    def run(self) -> int:
        """The whole run; returns the process exit code."""
        self.step = self.env.reset()
        self.progress.write("training", self.update, self.env_steps)
        if self.evaluating and self.config.eval.at_start and not self.tracker.history:
            self.evaluate()
        self.last_eval_env_steps = self.tracker.history[-1][0] if self.tracker.history else self.env_steps

        outcome: Outcome | None = None
        try:
            outcome = self.train()
        finally:
            self.finish(outcome)

        return 0 if outcome.action == ADVANCE else EXIT_BELOW_TARGET

    def finish(self, outcome: Outcome | None) -> None:
        """Save latest.pt and, when the stage was decided, finished.json; close the logs and the connection."""
        tracker = self.tracker
        self._save(self.run_dir / "latest.pt")
        if outcome:
            self.finished_path.write_text(json.dumps({
                "reason": outcome.reason,
                "advanced": outcome.action == ADVANCE,
                "env_steps": self.env_steps,
                "update": self.update,
                "best_score": tracker.best,
                "best_env_steps": tracker.best_env_steps,
                "restarts": self.controller.restarts,
                "judged": outcome.stage,
                "gates": outcome.gates.to_dict() if outcome.gates else None,
            }, indent=2))
            print(f"{self.config.run_name} finished: {outcome.reason}", flush=True)
        self.progress.write("finished" if outcome else "stopped", self.update, self.env_steps,
                            outcome.reason if outcome else "", advanced=bool(outcome and outcome.action == ADVANCE))
        self.drain_update()
        if self.updater is not None:
            self.updater.shutdown()
        self.logger.close()
        self.env.close()


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
        "--overlay", action="append", default=[], metavar="YAML",
        help="merge this config over --config, section by section (before --set), e.g. configs/fast.yaml",
    )
    parser.add_argument(
        "--set", action="append", default=[], metavar="KEY=VALUE",
        help="override a config value, e.g. --set total_env_steps=5000000 --set eval.every_env_steps=1000000",
    )
    parser.add_argument(
        "--resume", action="store_true",
        help="continue runs/<run name>/latest.pt instead of archiving the run and training from scratch",
    )
    args = parser.parse_args()

    config = TrainConfig.load(args.config, args.set, args.overlay)
    if args.socket:
        config.socket = args.socket
    if args.run_name:
        config.run_name = args.run_name
    if args.runs_dir:
        config.runs_dir = args.runs_dir
    if args.layouts_dir:
        config.layouts_dir = args.layouts_dir

    return TrainingRun(config, resume=args.resume).run()


if __name__ == "__main__":
    raise SystemExit(main())
