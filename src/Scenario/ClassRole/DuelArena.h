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

#ifndef MOD_ANIMUS_FORGE_DUEL_ARENA_H
#define MOD_ANIMUS_FORGE_DUEL_ARENA_H

#include "Define.h"
#include "Position.h"
#include <array>
#include <utility>
#include <vector>

class Creature;
class Map;
class Player;
class Unit;

namespace AnimusForge::DuelArena
{
    /// Distance band the opponent spawns at: beyond the aggro radius of a same-level creature (about
    /// 20 yd), so the bot always has to close in.
    constexpr float SPAWN_DISTANCE_MIN = 40.0f;
    constexpr float SPAWN_DISTANCE_MAX = 50.0f;

    /// Real creatures fit to be a fair same-level opponent: normal rank, attackable, no script, no
    /// NPC services, not civilian/guard/trigger/vehicle, and spawned somewhere in the world. Loaded
    /// once and bucketed by the levels each creature naturally has.
    class OpponentPool
    {
    public:
        static OpponentPool const& Instance();

        /// A random default-AI creature entry whose natural level range covers `level` (the nearest range
        /// when none does). 0 if the pool is empty. The duel stage's opponents.
        [[nodiscard]] uint32 Random(uint8 level) const;

        /// Like Random, but also creatures whose SmartAI only casts spells or talks (casters and ability
        /// users). The pack and gauntlet stages' creatures.
        [[nodiscard]] uint32 RandomPackMember(uint8 level) const;

        /// An elite creature (default AI or casting SmartAI) for the level, or 0.
        [[nodiscard]] uint32 RandomElite(uint8 level) const;

    private:
        OpponentPool();

        [[nodiscard]] static uint32 PickNear(std::array<std::vector<uint32>, 81> const& byLevel, uint8 level);

        std::array<std::vector<uint32>, 81> _byLevel;         // index = level
        std::array<std::vector<uint32>, 81> _packByLevel;
        std::array<std::vector<uint32>, 81> _elitesByLevel;
    };

    /// A random spot 40-50 yd from the bot, in line of sight on roughly level ground, with a random facing.
    [[nodiscard]] Position FindSpawnPoint(Player* bot, Map* map);

    /// Summon `entry` at `pos` and `level`, hostile to players and aggressive. Returns nullptr on failure.
    Creature* SummonOpponent(Player* bot, Map* map, uint32 entry, Position const& pos, uint8 level);

    /// Summon `entry` at the bot's level at a random bearing and distance from the bot, facing a random
    /// direction, hostile to players and aggressive. Returns nullptr on failure.
    Creature* SpawnOpponent(Player* bot, Map* map, uint32 entry);

    /// Summon a pack of `entries` at `level`, clustered around one spawn point, each facing its own way.
    std::vector<Creature*> SpawnPack(Player* bot, Map* map, std::vector<uint32> const& entries, uint8 level);


}

#endif
