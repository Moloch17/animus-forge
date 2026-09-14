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
#include <array>
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

        /// A random creature entry whose natural level range covers `level` (the nearest range when
        /// none does). 0 if the pool is empty.
        [[nodiscard]] uint32 Random(uint8 level) const;

        /// Tameable, non-exotic beast entries of `count` different random families: a hunter's stable
        /// for one episode. Fewer if the world has fewer families.
        [[nodiscard]] std::vector<uint32> RandomStable(uint32 count) const;

    private:
        OpponentPool();

        std::array<std::vector<uint32>, 81> _byLevel;     // index = level
        std::vector<std::vector<uint32>> _beastsByFamily;
    };

    /// Summon `entry` at the bot's level at a random bearing and distance from the bot, facing a random
    /// direction, hostile to players and aggressive. Returns nullptr on failure.
    Creature* SpawnOpponent(Player* bot, Map* map, uint32 entry);

    /// Hunters: bring the stabled beast `entry` out as the bot's pet (Call Pet for a pet that only
    /// exists in memory; Call Pet itself loads pets from the database). Needs level 10 and no pet out.
    /// Returns false if no pet was created.
    bool CallHunterBeast(Player* bot, uint32 entry);

    /// Order the bot's pet and guardians to attack `target`. Returns true if a pet was ordered.
    bool PetAttack(Player* bot, Unit* target);
}

#endif
