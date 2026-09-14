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

#ifndef ANIMUS_ENV_POOL_H
#define ANIMUS_ENV_POOL_H

#include "Env.h"
#include "Scenario.h"
#include <string>
#include <unordered_map>
#include <vector>

class Unit;
enum DamageEffectType : uint8;

namespace Animus
{
    struct ForgeConfig;

    /// Every env of one scenario, plus the flat structure-of-arrays buffers the bridge sends.
    ///
    /// Buffer layout is the STEP payload layout in Protocol.h, env-major.
    class EnvPool
    {
    public:
        EnvPool(Scenario& scenario, ForgeConfig const& config);

        bool Setup();
        void Teardown();

        /// Every world tick: advance each env's episode clock.
        void AdvanceClock(uint32 diff);

        /// Reset every env and write fresh observations with zero reward and done.
        void ResetAll();

        /// Decision step: score the transition that just ended, auto-reset finished envs, observe.
        void Collect();

        /// Fill Actions from a local policy ("random" or a scenario scripted policy).
        bool ChooseLocalActions(std::string const& policy);

        void ApplyActions();

        /// Damage hook, called from map worker threads. Only touches the stats of the env whose
        /// instance the calling thread is updating.
        void RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type);

        [[nodiscard]] Scenario const& GetScenario() const { return _scenario; }
        [[nodiscard]] ScenarioSpec const& Spec() const { return _spec; }
        [[nodiscard]] uint32 NumEnvs() const { return static_cast<uint32>(_envs.size()); }

        std::vector<float> Obs;
        std::vector<float> State;
        std::vector<uint8> Mask;
        std::vector<float> Rewards;
        std::vector<uint8> Done;
        std::vector<uint8> Terminated;
        std::vector<float> FinalObs;
        std::vector<float> FinalState;
        std::vector<float> EpisodeInfo;
        std::vector<int32> Actions;

    private:
        struct AgentSlot
        {
            uint32 Env;
            uint32 Agent;
        };

        void ResetEnv(Env& env);
        void ReportEpisode(uint32 envIndex);

        Scenario& _scenario;
        ScenarioSpec _spec;
        uint32 _episodeLengthMs;
        uint32 _reportEpisodes;

        std::vector<Env> _envs;

        /// Bot GUID -> env/agent. Built in Setup and read-only afterwards, so the concurrent
        /// lookups from map threads need no lock.
        std::unordered_map<ObjectGuid, AgentSlot> _agents;

        std::vector<uint8> _scratchMask;

        std::vector<double> _reportInfoSum;
        uint32 _reportedEpisodes = 0;
    };
}

#endif
