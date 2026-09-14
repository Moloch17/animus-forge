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

#ifndef MOD_ANIMUS_FORGE_ENV_H
#define MOD_ANIMUS_FORGE_ENV_H

#include "Define.h"
#include "ObjectGuid.h"
#include <array>
#include <vector>

class Map;
class Player;
class Creature;
class Unit;

namespace AnimusForge
{
    /// Most scripted allies an env can have (Env::Allies): a party's other four members.
    constexpr std::size_t MAX_ALLIES = 4;

    /// Combat totals for one agent. Written only by the map thread that updates the agent's
    /// instance (damage hooks), read by the world thread after MapMgr::Update has joined.
    struct AgentStats
    {
        uint64 Damage = 0;
        uint64 WhiteDamage = 0;
        uint64 SpecialDamage = 0;
        uint32 WhiteHits = 0;
        uint32 SpecialHits = 0;
        uint64 DamageTaken = 0;         // by the agent, from anything
        uint64 AllyDamageTaken = 0;     // by the env's allies (Env::Allies), from anything
        uint64 AllyHealing = 0;         // effective healing the agent (or its pets) did on the env's allies
        std::array<uint64, MAX_ALLIES> AllyDamageTakenBy{};    // the same, per Env::Allies index
        std::array<uint64, MAX_ALLIES> AllyHealingBy{};
        uint32 CastsCompleted = 0;      // the agent's own cast-time spells that finished casting
        uint32 CastsCancelled = 0;      // ... that were cut short (moved, stopped, interrupted, died)
        uint64 CastMsCompleted = 0;     // cast time of the completed casts
        uint64 CastMsWasted = 0;        // cast time already spent on the cancelled casts

        void Add(AgentStats const& other)
        {
            Damage += other.Damage;
            WhiteDamage += other.WhiteDamage;
            SpecialDamage += other.SpecialDamage;
            WhiteHits += other.WhiteHits;
            SpecialHits += other.SpecialHits;
            DamageTaken += other.DamageTaken;
            AllyDamageTaken += other.AllyDamageTaken;
            AllyHealing += other.AllyHealing;
            for (std::size_t ally = 0; ally < MAX_ALLIES; ++ally)
            {
                AllyDamageTakenBy[ally] += other.AllyDamageTakenBy[ally];
                AllyHealingBy[ally] += other.AllyHealingBy[ally];
            }
            CastsCompleted += other.CastsCompleted;
            CastsCancelled += other.CastsCancelled;
            CastMsCompleted += other.CastMsCompleted;
            CastMsWasted += other.CastMsWasted;
        }
    };

    /// One environment: an instance map holding the scenario's bots and target(s).
    ///
    /// Objects are held by GUID and resolved per use, never as raw pointers across ticks.
    struct Env
    {
        uint32 Index = 0;

        uint32 MapId = 0;
        uint32 InstanceId = 0;

        std::vector<ObjectGuid> Bots;       // one per agent, agent order
        std::vector<ObjectGuid> Targets;
        std::vector<ObjectGuid> Allies;     // scripted friendly players the agents fight for (not agents)

        uint32 EpisodeElapsedMs = 0;
        uint32 EpisodeLengthMs = 0;
        uint32 EpisodesCompleted = 0;

        std::vector<AgentStats> StepStats;      // since the last decision
        std::vector<AgentStats> EpisodeStats;   // since the last reset

        [[nodiscard]] Map* FindMap() const;
        [[nodiscard]] Player* FindBot(uint32 agent) const;
        [[nodiscard]] Creature* FindTarget(uint32 target) const;
        [[nodiscard]] Unit* FindTargetUnit(uint32 target) const;  // a creature or a player target
    };
}

#endif
