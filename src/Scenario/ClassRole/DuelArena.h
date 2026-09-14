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

        /// Tameable, non-exotic beast entries of `count` different random families: a hunter's stable
        /// for one episode. Fewer if the world has fewer families.
        [[nodiscard]] std::vector<uint32> RandomStable(uint32 count) const;

    private:
        OpponentPool();

        [[nodiscard]] static uint32 PickNear(std::array<std::vector<uint32>, 81> const& byLevel, uint8 level);

        std::array<std::vector<uint32>, 81> _byLevel;         // index = level
        std::array<std::vector<uint32>, 81> _packByLevel;
        std::array<std::vector<uint32>, 81> _elitesByLevel;
        std::vector<std::vector<uint32>> _beastsByFamily;
    };

    /// Vendor-sold food (health regeneration) and drink (mana regeneration) by required level.
    class ConsumablePool
    {
    public:
        static ConsumablePool const& Instance();

        /// The best food / drink a character of `level` can use, or 0.
        [[nodiscard]] uint32 Food(uint8 level) const { return Best(_food, level); }
        [[nodiscard]] uint32 Drink(uint8 level) const { return Best(_drink, level); }

    private:
        ConsumablePool();

        [[nodiscard]] static uint32 Best(std::vector<std::pair<uint8, uint32>> const& items, uint8 level);

        std::vector<std::pair<uint8, uint32>> _food;     // (required level, item), sorted
        std::vector<std::pair<uint8, uint32>> _drink;
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

    /// Hunters: bring the stabled beast `entry` out as the bot's pet (Call Pet for a pet that only
    /// exists in memory; Call Pet itself loads pets from the database). Needs level 10 and no pet out.
    /// Returns false if no pet was created.
    bool CallHunterBeast(Player* bot, uint32 entry);

    /// Order the bot's pet and guardians to attack `target`. Returns true if a pet was ordered.
    bool PetAttack(Player* bot, Unit* target);
}

#endif
