"""Export a checkpoint's actor for in-game inference by mod-animus.

    python -m animus.export --checkpoint runs/warrior_dummy/latest.pt \\
        --out ../../mod-animus/models/warrior_dummy.amdl

The .amdl format (little-endian) is read by mod-animus/src/Model/MlpPolicy.cpp; change both together
and bump AMDL_VERSION:

    char[4]  magic "AMDL"
    u32      version
    u16      scenario name length, then that many bytes (UTF-8, no terminator)
    u32      obs_dim
    u32      num_agents       the actor input is obs followed by a one-hot agent id
    u32      num_actions
    u32      layer_count
    per layer:
        u32      in_dim
        u32      out_dim
        f32      weight[out_dim * in_dim]   row-major, as nn.Linear stores it
        f32      bias[out_dim]

Every layer but the last is followed by tanh (mappo.networks._mlp). The policy is the argmax of the
final logits over allowed actions.

Training also publishes the model on its own: every time it writes latest.pt it exports the actor to
<dir>/<scenario>.amdl in each of TrainConfig.model_dirs and $ANIMUS_MODEL_DIRS (see publish_model).
"""

from __future__ import annotations

import argparse
import os
import re
import struct
from pathlib import Path

import numpy as np
import torch

AMDL_MAGIC = b"AMDL"
AMDL_VERSION = 1

_LINEAR_KEY = re.compile(r"^net\.(\d+)\.(weight|bias)$")


def actor_layers(actor_state: dict[str, torch.Tensor]) -> list[tuple[np.ndarray, np.ndarray]]:
    """(weight [out, in], bias [out]) for each Linear of an Actor state dict, in forward order."""
    params: dict[int, dict[str, np.ndarray]] = {}
    for key, tensor in actor_state.items():
        match = _LINEAR_KEY.match(key)
        if not match:
            raise ValueError(f"unexpected actor parameter {key!r}")
        params.setdefault(int(match.group(1)), {})[match.group(2)] = tensor.detach().cpu().numpy()

    layers = []
    for index in sorted(params):
        weight, bias = params[index]["weight"], params[index]["bias"]
        if weight.ndim != 2 or bias.shape != (weight.shape[0],):
            raise ValueError(f"layer net.{index} has weight {weight.shape} and bias {bias.shape}")
        layers.append((weight.astype("<f4"), bias.astype("<f4")))

    for (prev, _), (weight, _) in zip(layers, layers[1:]):
        if weight.shape[1] != prev.shape[0]:
            raise ValueError(f"layer input {weight.shape[1]} does not match previous output {prev.shape[0]}")
    return layers


def write_amdl(
    path: str | Path,
    scenario: str,
    obs_dim: int,
    num_agents: int,
    num_actions: int,
    layers: list[tuple[np.ndarray, np.ndarray]],
) -> None:
    if layers[0][0].shape[1] != obs_dim + num_agents:
        raise ValueError(f"first layer takes {layers[0][0].shape[1]} inputs, expected {obs_dim} + {num_agents}")
    if layers[-1][0].shape[0] != num_actions:
        raise ValueError(f"last layer has {layers[-1][0].shape[0]} outputs, expected {num_actions}")

    name = scenario.encode("utf-8")
    with open(path, "wb") as out:
        out.write(AMDL_MAGIC)
        out.write(struct.pack("<IH", AMDL_VERSION, len(name)))
        out.write(name)
        out.write(struct.pack("<IIII", obs_dim, num_agents, num_actions, len(layers)))
        for weight, bias in layers:
            out.write(struct.pack("<II", weight.shape[1], weight.shape[0]))
            out.write(np.ascontiguousarray(weight, dtype="<f4").tobytes())
            out.write(np.ascontiguousarray(bias, dtype="<f4").tobytes())


def publish_model(actor_state: dict[str, torch.Tensor], spec: dict, model_dirs: list[str | Path]) -> list[Path]:
    """Export the actor as <dir>/<scenario>.amdl into every existing model dir; returns the files written.

    Each file is written beside its target and renamed over it, so a worldserver reloading its config
    never reads a half-written model. A missing or unwritable dir is reported and skipped: publishing
    must never stop a training run.
    """
    layers = actor_layers(actor_state)
    written = []
    for model_dir in model_dirs:
        model_dir = Path(model_dir)
        if not model_dir.is_dir():
            print(f"Model dir {model_dir} does not exist; not publishing the model there", flush=True)
            continue

        target = model_dir / f"{spec['scenario']}.amdl"
        partial = model_dir / f".{target.name}.partial"
        try:
            write_amdl(partial, spec["scenario"], spec["obs_dim"], spec["agents_per_env"], spec["num_actions"], layers)
            os.replace(partial, target)
        except OSError as error:
            print(f"Could not publish the model to {target}: {error}", flush=True)
            partial.unlink(missing_ok=True)
            continue
        written.append(target)
    return written


def model_dirs_from_env() -> list[str]:
    """Extra model dirs from $ANIMUS_MODEL_DIRS, separated like PATH."""
    return [path for path in os.environ.get("ANIMUS_MODEL_DIRS", "").split(os.pathsep) if path]


def read_amdl(path: str | Path) -> dict:
    """Parse an .amdl file. Used by the tests as the reference for the C++ loader."""
    data = Path(path).read_bytes()
    if data[:4] != AMDL_MAGIC:
        raise ValueError("not an .amdl file")
    offset = 4
    version, name_len = struct.unpack_from("<IH", data, offset)
    offset += 6
    if version != AMDL_VERSION:
        raise ValueError(f"unsupported .amdl version {version}")
    scenario = data[offset : offset + name_len].decode("utf-8")
    offset += name_len
    obs_dim, num_agents, num_actions, layer_count = struct.unpack_from("<IIII", data, offset)
    offset += 16

    layers = []
    for _ in range(layer_count):
        in_dim, out_dim = struct.unpack_from("<II", data, offset)
        offset += 8
        weight = np.frombuffer(data, dtype="<f4", count=out_dim * in_dim, offset=offset).reshape(out_dim, in_dim)
        offset += out_dim * in_dim * 4
        bias = np.frombuffer(data, dtype="<f4", count=out_dim, offset=offset)
        offset += out_dim * 4
        layers.append((weight, bias))

    if offset != len(data):
        raise ValueError(f"{len(data) - offset} trailing bytes")
    return {
        "scenario": scenario,
        "obs_dim": obs_dim,
        "num_agents": num_agents,
        "num_actions": num_actions,
        "layers": layers,
    }


def reference_decide(model: dict, obs: np.ndarray, mask: np.ndarray, agent: int = 0) -> tuple[int, np.ndarray]:
    """The forward pass mod-animus runs: returns (greedy allowed action, logits)."""
    x = np.concatenate([obs.astype(np.float32), np.eye(model["num_agents"], dtype=np.float32)[agent]])
    layers = model["layers"]
    for index, (weight, bias) in enumerate(layers):
        x = weight @ x + bias
        if index + 1 < len(layers):
            x = np.tanh(x)

    allowed = mask.astype(bool)
    if not allowed.any():
        return 0, x
    return int(np.where(allowed, x, -np.inf).argmax()), x


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    spec = checkpoint["spec"]
    layers = actor_layers(checkpoint["trainer"]["actor"])

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    write_amdl(out, spec["scenario"], spec["obs_dim"], spec["agents_per_env"], spec["num_actions"], layers)

    shape = " -> ".join(str(w.shape[1]) for w, _ in layers) + f" -> {layers[-1][0].shape[0]}"
    print(f"Wrote {out} ({spec['scenario']}, update {checkpoint.get('update', '?')}, {shape})")


if __name__ == "__main__":
    main()
