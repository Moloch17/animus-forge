"""Evaluate a checkpoint against a running Animus Forge sim.

    python -m animus.evaluate --checkpoint runs/<name>/latest.pt --episodes 256 [--stochastic]

Compare the printed DPS with the scripted baselines, which the sim itself runs and logs with
AnimusForge.Policy = never_hs / hs_at_threshold / random.
"""

from __future__ import annotations

import argparse

import numpy as np
import torch

from .config import TrainConfig
from .env import ForgeEnv
from .mappo.trainer import MappoConfig, MappoTrainer


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--episodes", type=int, default=256)
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

    trainer = MappoTrainer(spec.obs_dim, spec.state_dim, spec.num_actions, spec.agents_per_env, mappo)
    trainer.load_state_dict(checkpoint["trainer"], load_optimizers=False)

    episodes: list[np.ndarray] = []
    action_counts = np.zeros(spec.num_actions, dtype=np.int64)

    try:
        step = env.reset()
        while len(episodes) < args.episodes:
            actions, _ = trainer.act(step.obs, step.mask, deterministic=not args.stochastic)
            action_counts += np.bincount(actions.ravel(), minlength=spec.num_actions)
            step = env.step(actions)
            if step.done.any():
                episodes.extend(step.episode_info[step.done])
    finally:
        env.close()

    results = np.array(episodes[: args.episodes])
    print(f"{spec.scenario}: {len(results)} episodes, {'stochastic' if args.stochastic else 'greedy'} policy")
    for column, name in enumerate(spec.episode_info_names):
        print(f"  {name:>16}: {results[:, column].mean():10.2f} +- {results[:, column].std():.2f}")
    rates = action_counts / max(1, action_counts.sum())
    print("  action rates: " + ", ".join(f"{a}={r:.3f}" for a, r in enumerate(rates)))


if __name__ == "__main__":
    main()
