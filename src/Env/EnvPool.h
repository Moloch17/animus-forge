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

#ifndef MOD_ANIMUS_FORGE_ENV_POOL_H
#define MOD_ANIMUS_FORGE_ENV_POOL_H

#include "Env.h"
#include "Scenario.h"
#include <string>
#include <unordered_map>
#include <vector>

class Spell;
class Unit;
enum DamageEffectType : uint8;

namespace AnimusForge
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

        /// Evaluation (see ModeMsg in Protocol.h): hand seed indexes 0..episodes-1 to envs as they reset, each
        /// env rebuilt right after reseeding the world thread's random numbers from (seedBase, index). With a
        /// baseline policy name, EvalBaseline() tells the caller to run it instead of the learner's actions.
        /// Takes effect at the next reset; call ResetAll to start every env on it.
        void SetEvaluation(bool enabled, uint32 seedBase, uint32 episodes, std::string const& baseline);
        [[nodiscard]] bool IsEvaluating() const { return _evaluating; }
        [[nodiscard]] std::string const& EvalBaseline() const { return _evalBaseline; }

        /// Damage hook, called from map worker threads. Only touches the stats of the env whose
        /// instance the calling thread is updating.
        void RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type);

        /// Heal hook, called from map threads with the health actually gained. Counts healing an agent
        /// (or its pets) does on its env's allies.
        void RecordHeal(Unit const* healer, Unit const* receiver, uint32 gain);

        /// Spell hooks, called from map threads when an agent's cast-time spell finishes casting or is
        /// cancelled before it does. Triggered spells and channels are not counted.
        void RecordCastCompleted(Unit const* caster, Spell* spell);
        void RecordCastCancelled(Unit const* caster, Spell* spell);

        [[nodiscard]] Scenario const& GetScenario() const { return _scenario; }
        [[nodiscard]] ScenarioSpec const& Spec() const { return _spec; }
        [[nodiscard]] uint32 NumEnvs() const { return static_cast<uint32>(_envs.size()); }

        /// Episodes finished by every env since Setup.
        [[nodiscard]] uint64 CompletedEpisodes() const;

        std::vector<float> Obs;
        std::vector<float> State;
        std::vector<uint8> Mask;
        std::vector<float> Rewards;
        std::vector<uint8> Done;
        std::vector<uint8> Terminated;
        std::vector<float> FinalObs;
        std::vector<float> FinalState;
        std::vector<uint16> Layout;             // per agent: index into Spec().Layouts
        std::vector<float> EpisodeInfo;         // per agent
        std::vector<uint32> EpisodeSeed;        // per env: seed index of the episode that just ended
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

        /// Bot GUID -> env/agent. Built in Setup and changed only when a reset rebuilds an env's bots,
        /// on the world thread while no map updates, so the concurrent lookups from map threads need
        /// no lock.
        std::unordered_map<ObjectGuid, AgentSlot> _agents;

        /// Ally GUID -> env and index in Env::Allies. Maintained like _agents.
        std::unordered_map<ObjectGuid, AgentSlot> _allies;

        std::vector<uint8> _scratchMask;

        bool _evaluating = false;
        uint32 _evalSeedBase = 0;
        uint32 _evalEpisodes = 0;
        uint32 _evalNextSeed = 0;
        std::string _evalBaseline;
        std::vector<uint32> _envSeed;           // per env: seed index of the running episode

        std::vector<double> _reportInfoSum;
        uint32 _reportedEpisodes = 0;
    };
}

#endif
