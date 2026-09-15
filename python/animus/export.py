"""Export a checkpoint's actor as plain MLP models (.amdl) for in-game inference.

    python -m animus.export --checkpoint runs/class_role_duel/best.pt --out exported/class_role_duel

Exporting is always run by hand, and the exported files are copied to a server by hand: training never writes
models anywhere but its own run directory.

The .amdl format (little-endian); a reader must follow it exactly, and a change bumps AMDL_VERSION:

    char[4]  magic "AMDL"
    u32      version
    u16      model name length, then that many bytes (UTF-8, no terminator)
    u32      obs_dim
    u32      num_agents       the actor input is obs followed by a one-hot agent id
    u32      num_actions
    u32      layer_count
    per layer:
        u32      in_dim
        u32      out_dim
        f32      weight[out_dim * in_dim]   row-major, as nn.Linear stores it
        f32      bias[out_dim]

Every layer but the last is followed by tanh. The policy is the argmax of the final logits over allowed actions.

The learner's actor is layout-aware (mappo.networks.LayoutActor): one input adapter and action head per layout
around a shared trunk. For one layout, adapter + trunk + head is exactly such an MLP, so every layout exports as
its own model, <model name>.amdl. A class/role stage's stage.json names each layout's model (warrior_dps at
class_role_duel -> warrior_dps_duel); a scenario without one keeps its own name (one layout) or appends the layout's.
num_agents is 1, with a zero-weight agent column (the format has at least one).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
from pathlib import Path

import numpy as np
import torch

from .stages import STAGE_FILE, model_names

AMDL_MAGIC = b"AMDL"
AMDL_VERSION = 1

_TRUNK_KEY = re.compile(r"^trunk\.layers\.(\d+)\.(weight|bias)$")


def layout_layers(actor_state: dict[str, torch.Tensor], layout: int) -> list[tuple[np.ndarray, np.ndarray]]:
    """(weight [out, in], bias [out]) of one layout's adapter, the trunk and its head, in forward order."""

    def pair(prefix: str) -> tuple[np.ndarray, np.ndarray]:
        weight = actor_state[f"{prefix}.weight"].detach().cpu().numpy().astype("<f4")
        bias = actor_state[f"{prefix}.bias"].detach().cpu().numpy().astype("<f4")
        return weight, bias

    trunk = sorted({int(m.group(1)) for key in actor_state if (m := _TRUNK_KEY.match(key))})
    layers = [pair(f"adapters.{layout}"), *(pair(f"trunk.layers.{i}") for i in trunk), pair(f"heads.{layout}")]

    for (prev, _), (weight, _) in zip(layers, layers[1:]):
        if weight.shape[1] != prev.shape[0]:
            raise ValueError(f"layer input {weight.shape[1]} does not match previous output {prev.shape[0]}")
    return layers


def with_agent_column(layers: list[tuple[np.ndarray, np.ndarray]]) -> list[tuple[np.ndarray, np.ndarray]]:
    """The same network with one zero-weight input appended: the single one-hot agent id of the file format."""
    weight, bias = layers[0]
    padded = np.concatenate([weight, np.zeros((weight.shape[0], 1), dtype="<f4")], axis=1)
    return [(padded, bias), *layers[1:]]


def model_name(scenario: str, layout: str, layout_count: int, models: dict[str, str] | None = None) -> str:
    """The model name stage.json gives the layout; without one, a single-layout scenario keeps its own name."""
    if models and layout in models:
        return models[layout]
    return scenario if layout_count == 1 else f"{scenario}_{layout}"


def write_amdl(
    path: str | Path,
    name: str,
    obs_dim: int,
    num_agents: int,
    num_actions: int,
    layers: list[tuple[np.ndarray, np.ndarray]],
) -> None:
    if layers[0][0].shape[1] != obs_dim + num_agents:
        raise ValueError(f"first layer takes {layers[0][0].shape[1]} inputs, expected {obs_dim} + {num_agents}")
    if layers[-1][0].shape[0] != num_actions:
        raise ValueError(f"last layer has {layers[-1][0].shape[0]} outputs, expected {num_actions}")

    encoded = name.encode("utf-8")
    with open(path, "wb") as out:
        out.write(AMDL_MAGIC)
        out.write(struct.pack("<IH", AMDL_VERSION, len(encoded)))
        out.write(encoded)
        out.write(struct.pack("<IIII", obs_dim, num_agents, num_actions, len(layers)))
        for weight, bias in layers:
            out.write(struct.pack("<II", weight.shape[1], weight.shape[0]))
            out.write(np.ascontiguousarray(weight, dtype="<f4").tobytes())
            out.write(np.ascontiguousarray(bias, dtype="<f4").tobytes())


def export_layouts(
    actor_state: dict[str, torch.Tensor], spec: dict, out_dir: str | Path, manifest_dir: str | Path | None = None
) -> list[Path]:
    """Write every layout's model to out_dir/<model name>.amdl, each atomically; returns the files written.

    manifest_dir is the stage's layouts directory (the sim writes <OutputDir>/layouts/<scenario>/): its stage.json
    names the models, and each <model name>.json layout manifest there is copied beside its model, so whoever loads the
    model can check it reads the same layout.
    """
    out_dir = Path(out_dir)
    manifests = Path(manifest_dir) if manifest_dir is not None else Path("layouts") / spec["scenario"]
    stage_path = manifests / STAGE_FILE
    models = model_names(json.loads(stage_path.read_text())) if stage_path.is_file() else {}
    layouts = spec["layouts"]
    written = []
    for index, layout in enumerate(layouts):
        name = model_name(spec["scenario"], layout["name"], len(layouts), models)
        layers = with_agent_column(layout_layers(actor_state, index))
        target = out_dir / f"{name}.amdl"
        partial = out_dir / f".{target.name}.partial"
        try:
            write_amdl(partial, name, layout["obs_dim"], 1, layout["num_actions"], layers)
            os.replace(partial, target)
        except OSError:
            partial.unlink(missing_ok=True)
            raise
        written.append(target)

        manifest = manifests / f"{name}.json"
        if manifest.is_file():
            partial_manifest = out_dir / f".{name}.json.partial"
            partial_manifest.write_bytes(manifest.read_bytes())
            os.replace(partial_manifest, out_dir / f"{name}.json")
    return written


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
    name = data[offset : offset + name_len].decode("utf-8")
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
        "scenario": name,
        "obs_dim": obs_dim,
        "num_agents": num_agents,
        "num_actions": num_actions,
        "layers": layers,
    }


def reference_decide(model: dict, obs: np.ndarray, mask: np.ndarray, agent: int = 0) -> tuple[int, np.ndarray]:
    """The forward pass of an exported model: returns (greedy allowed action, logits)."""
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
    parser.add_argument("--out", required=True, help="directory for the .amdl files, one per layout")
    parser.add_argument(
        "--layouts-dir", default="layouts", help="the sim's layouts directory (AnimusForge.OutputDir/layouts)"
    )
    args = parser.parse_args()

    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifests = Path(args.layouts_dir) / checkpoint["spec"]["scenario"]
    for path in export_layouts(checkpoint["trainer"]["actor"], checkpoint["spec"], out, manifests):
        print(f"Wrote {path} (update {checkpoint.get('update', '?')})")


if __name__ == "__main__":
    main()
