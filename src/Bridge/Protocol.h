/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Lock-step wire protocol between the sim (server) and the Python learner (client).
 * Mirrored field for field in python/animus/protocol.py -- change both together and bump
 * PROTOCOL_VERSION.
 *
 * Every message is a MsgHeader followed by `length` payload bytes. Little-endian, no padding.
 *
 *   client -> server  HELLO  { u32 version }
 *   server -> client  SPEC   SpecMsg, then the episode info column names as comma-separated ASCII
 *                            filling the rest of the payload (no terminator)
 *   server -> client  STEP   { u64 decision } then, in order, with E envs, A agents per env,
 *                            O obs dim, S state dim, N actions, K episode info dim:
 *                              f32 obs[E*A*O]         observation after any auto-reset
 *                              f32 state[E*S]         critic state after any auto-reset
 *                              u8  mask[E*A*N]        1 = action allowed
 *                              f32 reward[E*A]        reward for the transition that just ended
 *                              u8  done[E]            1 = episode ended on this transition
 *                              u8  terminated[E]      1 = it ended in a terminal state (no
 *                                                     bootstrap); done without terminated is a
 *                                                     time-limit truncation
 *                              f32 final_obs[E*A*O]   last obs of the ended episode (valid if done)
 *                              f32 final_state[E*S]   last state of the ended episode (valid if done)
 *                              f32 episode_info[E*K]  totals for the ended episode (valid if done)
 *   client -> server  ACT    { i32 actions[E*A] }
 *   client -> server  CLOSE  {}  (instead of ACT) -- server drops the client and waits for a new one
 *
 * The first STEP after SPEC carries freshly reset envs: its reward and done arrays are zero and
 * must not be recorded as a transition. A truncated episode (done, not terminated) bootstraps from
 * final_state; a terminated one does not.
 */

#ifndef MOD_ANIMUS_FORGE_PROTOCOL_H
#define MOD_ANIMUS_FORGE_PROTOCOL_H

#include "Define.h"
#include <bit>

namespace AnimusForge
{
    constexpr uint32 PROTOCOL_VERSION = 1;
    constexpr uint32 SCENARIO_NAME_SIZE = 32;

    static_assert(std::endian::native == std::endian::little, "the wire protocol is little-endian");

    enum class MsgType : uint32
    {
        Hello = 1,
        Spec  = 2,
        Step  = 3,
        Act   = 4,
        Close = 5,
    };

#pragma pack(push, 1)
    struct MsgHeader
    {
        uint32 Type;
        uint32 Length;
    };

    struct HelloMsg
    {
        uint32 Version;
    };

    struct SpecMsg
    {
        uint32 Version;
        uint32 NumEnvs;
        uint32 AgentsPerEnv;
        uint32 ObsDim;
        uint32 StateDim;
        uint32 NumActions;
        uint32 EpisodeInfoDim;
        uint32 TickMs;
        uint32 DecisionTicks;
        uint32 EpisodeSeconds;
        char Scenario[SCENARIO_NAME_SIZE];
    };

    struct StepHeader
    {
        uint64 Decision;
    };
#pragma pack(pop)
}

#endif
