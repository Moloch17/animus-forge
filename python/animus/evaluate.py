"""Evaluate a checkpoint against a running Animus Forge sim, on seeded episodes.

    python -m animus.evaluate --checkpoint runs/<name>/best.pt [--episodes 128] [--seed 1000] [--baseline fight]

Uses the same seeded evaluation as training (animus.evaluation): with the same --seed and --episodes the
characters and opponents match the ones training scored. --baseline also scores a scripted sim policy on
those seeds. The sim must run the checkpoint's scenario with AnimusForge.Policy = "remote" and no learner of
its own attached (AnimusForge.Learner.AutoStart = 0).
"""

from __future__ import annotations

import argparse
from dataclasses import asdict

import torch

from .config import REPORT_COLUMNS, TrainConfig
from .env import ForgeEnv
from .evaluation import format_summary, run_evaluation
from .mappo.trainer import MappoConfig, MappoTrainer
from .runs import resume_mismatch


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--episodes", type=int, default=128)
    parser.add_argument("--seed", type=int, default=1000)
    parser.add_argument("--baseline", default="", help="also score this scripted sim policy on the same seeds")
    parser.add_argument("--socket", help="override the socket stored in the checkpoint config")
    parser.add_argument("--stochastic", action="store_true", help="sample actions instead of taking the argmax")
    args = parser.parse_args()

    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    saved = checkpoint["config"]
    mappo = MappoConfig(**{**saved["mappo"], "hidden": tuple(saved["mappo"]["hidden"])})
    socket_path = args.socket or saved.get("socket", TrainConfig.socket)

    env = ForgeEnv(socket_path)
    spec = env.spec
    if spec.scenario != checkpoint["spec"]["scenario"]:
        raise SystemExit(f"checkpoint was trained on {checkpoint['spec']['scenario']}, sim runs {spec.scenario}")

    # Layouts must match by name as well as size: two class/role lists can have equally sized layouts.
    if mismatch := resume_mismatch(checkpoint["spec"], asdict(spec)):
        raise SystemExit(f"the checkpoint's {', '.join(mismatch)} do not match the sim's (AnimusForge.ClassRoles?)")
    layouts = [(layout.obs_dim, layout.num_actions) for layout in spec.layouts]

    trainer = MappoTrainer(layouts, spec.state_dim, mappo)
    trainer.load_state_dict(checkpoint["trainer"], load_optimizers=False)

    def actions(step):
        return trainer.act(step.obs, step.mask, step.layout, deterministic=not args.stochastic)[0]

    try:
        env.reset()
        baseline = None
        if args.baseline:
            result, _ = run_evaluation(env, spec, actions, args.episodes, args.seed, baseline=args.baseline)
            baseline = result.summary(REPORT_COLUMNS)
        result, _ = run_evaluation(env, spec, actions, args.episodes, args.seed)
    finally:
        env.close()

    print(f"{spec.scenario} (update {checkpoint['update']}): score {result.score:.4g} over {result.episodes} seeded "
          f"episodes, {'stochastic' if args.stochastic else 'greedy'} policy [learner/{args.baseline or '-'}]")
    print(format_summary(result.summary(REPORT_COLUMNS), baseline, REPORT_COLUMNS))


if __name__ == "__main__":
    main()
