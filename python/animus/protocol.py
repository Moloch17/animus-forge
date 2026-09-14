"""Lock-step wire protocol, mirrored field for field from src/Bridge/Protocol.h.

Change both files together and bump PROTOCOL_VERSION.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum

import numpy as np

PROTOCOL_VERSION = 1
SCENARIO_NAME_SIZE = 32


class MsgType(IntEnum):
    HELLO = 1
    SPEC = 2
    STEP = 3
    ACT = 4
    CLOSE = 5


HEADER = struct.Struct("<II")  # type, payload length
HELLO = struct.Struct("<I")  # version
SPEC = struct.Struct(f"<10I{SCENARIO_NAME_SIZE}s")
STEP_HEADER = struct.Struct("<Q")  # decision counter


@dataclass(frozen=True)
class Spec:
    version: int
    num_envs: int
    agents_per_env: int
    obs_dim: int
    state_dim: int
    num_actions: int
    episode_info_dim: int
    tick_ms: int
    decision_ticks: int
    episode_seconds: int
    scenario: str
    episode_info_names: tuple[str, ...] = field(default_factory=tuple)

    @property
    def decision_ms(self) -> int:
        return self.tick_ms * self.decision_ticks

    def step_layout(self) -> list[tuple[str, np.dtype, tuple[int, ...]]]:
        """STEP payload arrays after the header, in wire order: (name, dtype, shape)."""
        e, a = self.num_envs, self.agents_per_env
        f32, u8 = np.dtype("<f4"), np.dtype("u1")
        return [
            ("obs", f32, (e, a, self.obs_dim)),
            ("state", f32, (e, self.state_dim)),
            ("mask", u8, (e, a, self.num_actions)),
            ("reward", f32, (e, a)),
            ("done", u8, (e,)),
            ("terminated", u8, (e,)),
            ("final_obs", f32, (e, a, self.obs_dim)),
            ("final_state", f32, (e, self.state_dim)),
            ("episode_info", f32, (e, self.episode_info_dim)),
        ]

    def step_payload_size(self) -> int:
        size = STEP_HEADER.size
        for _, dtype, shape in self.step_layout():
            size += dtype.itemsize * int(np.prod(shape))
        return size

    def act_payload_size(self) -> int:
        return 4 * self.num_envs * self.agents_per_env


@dataclass
class Step:
    decision: int
    obs: np.ndarray  # [E, A, O] float32
    state: np.ndarray  # [E, S] float32
    mask: np.ndarray  # [E, A, N] bool
    reward: np.ndarray  # [E, A] float32
    done: np.ndarray  # [E] bool
    terminated: np.ndarray  # [E] bool
    final_obs: np.ndarray  # [E, A, O] float32, valid where done
    final_state: np.ndarray  # [E, S] float32, valid where done
    episode_info: np.ndarray  # [E, K] float32, valid where done


def encode_spec(spec: Spec) -> bytes:
    body = SPEC.pack(
        spec.version,
        spec.num_envs,
        spec.agents_per_env,
        spec.obs_dim,
        spec.state_dim,
        spec.num_actions,
        spec.episode_info_dim,
        spec.tick_ms,
        spec.decision_ticks,
        spec.episode_seconds,
        spec.scenario.encode("ascii"),
    )
    return body + ",".join(spec.episode_info_names).encode("ascii")


def decode_spec(payload: bytes) -> Spec:
    fields = SPEC.unpack_from(payload)
    names = payload[SPEC.size :].decode("ascii")
    return Spec(
        *fields[:10],
        scenario=fields[10].split(b"\0", 1)[0].decode("ascii"),
        episode_info_names=tuple(names.split(",")) if names else (),
    )


def encode_step(spec: Spec, step: Step) -> bytes:
    parts = [STEP_HEADER.pack(step.decision)]
    for name, dtype, shape in spec.step_layout():
        array = np.ascontiguousarray(getattr(step, name), dtype=dtype).reshape(shape)
        parts.append(array.tobytes())
    return b"".join(parts)


def decode_step(spec: Spec, payload: bytes | bytearray | memoryview) -> Step:
    """Decode a STEP payload. Arrays are copies, so the receive buffer can be reused."""
    (decision,) = STEP_HEADER.unpack_from(payload)
    offset = STEP_HEADER.size
    arrays = {}
    for name, dtype, shape in spec.step_layout():
        count = int(np.prod(shape))
        array = np.frombuffer(payload, dtype=dtype, count=count, offset=offset).reshape(shape).copy()
        offset += dtype.itemsize * count
        if dtype == np.dtype("u1"):
            array = array.astype(bool)
        arrays[name] = array
    return Step(decision=decision, **arrays)


def encode_header(msg_type: MsgType, length: int) -> bytes:
    return HEADER.pack(int(msg_type), length)
