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

#ifndef MOD_ANIMUS_FORGE_TRAINING_DUMMY_ARENA_H
#define MOD_ANIMUS_FORGE_TRAINING_DUMMY_ARENA_H

#include "Define.h"

class Creature;
class Map;
class Player;

namespace AnimusForge::TrainingDummyArena
{
    /// Bot accounts live far above anything a real realm allocates.
    constexpr uint32 BOT_ACCOUNT_BASE = 0x7F000000;

    /// Remove every creature near the bot that a scenario did not spawn.
    void ClearArena(Player* bot);

    /// Summon a Grandmaster's Training Dummy in front of the bot at the bot's level and turn the
    /// bot to face it. The dummy is rooted, never attacks, and its script zeroes all damage, so
    /// damage has to be measured before that (UnitScript::DealDamage). Returns nullptr on failure.
    /// `distance` is how far in front of the bot it stands (melee by default; casters and hunters
    /// stand further back).
    Creature* SpawnDummy(Player* bot, Map* map, float distance = 2.0f);
}

#endif
