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
#include <vector>

class Map;
class Player;
class Creature;

namespace AnimusForge
{
    /// Combat totals for one agent. Written only by the map thread that updates the agent's
    /// instance (damage hooks), read by the world thread after MapMgr::Update has joined.
    struct AgentStats
    {
        uint64 Damage = 0;
        uint64 WhiteDamage = 0;
        uint64 SpecialDamage = 0;
        uint32 WhiteHits = 0;
        uint32 SpecialHits = 0;

        void Add(AgentStats const& other)
        {
            Damage += other.Damage;
            WhiteDamage += other.WhiteDamage;
            SpecialDamage += other.SpecialDamage;
            WhiteHits += other.WhiteHits;
            SpecialHits += other.SpecialHits;
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

        uint32 EpisodeElapsedMs = 0;
        uint32 EpisodeLengthMs = 0;
        uint32 EpisodesCompleted = 0;

        std::vector<AgentStats> StepStats;      // since the last decision
        std::vector<AgentStats> EpisodeStats;   // since the last reset

        [[nodiscard]] Map* FindMap() const;
        [[nodiscard]] Player* FindBot(uint32 agent) const;
        [[nodiscard]] Creature* FindTarget(uint32 target) const;
    };
}

#endif
