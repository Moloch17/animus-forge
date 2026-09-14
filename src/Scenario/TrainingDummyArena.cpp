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

#include "TrainingDummyArena.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "SummonLevel.h"
#include "TemporarySummon.h"
#include <cmath>
#include <list>

namespace
{
    enum TrainingDummyCreatures : uint32
    {
        // Grandmaster's Training Dummy: rooted, attackable, npc_training_dummy zeroes all damage.
        NPC_TRAINING_DUMMY          = 31144,
    };

    /// Creatures within this radius of the bot that the scenario did not spawn are removed.
    constexpr float ARENA_CLEAR_RADIUS = 60.0f;

    /// The template's HealthModifier is sized for level 80; at low levels it rounds to 0 HP.
    /// The dummy cannot take damage, so any positive value works.
    constexpr uint32 DUMMY_HEALTH = 1000000;
}

void AnimusForge::TrainingDummyArena::ClearArena(Player* bot)
{
    // alive = false: every creature in range, dead or alive.
    std::list<Creature*> creatures;
    bot->GetDeadCreatureListInGrid(creatures, ARENA_CLEAR_RADIUS, false);

    for (Creature* creature : creatures)
        creature->DespawnOrUnsummon(0ms, Seconds(WEEK));

    if (!creatures.empty())
        LOG_WARN("module.animus", "Removed {} creatures from the arena around {}", creatures.size(), bot->GetName());
}

Creature* AnimusForge::TrainingDummyArena::SpawnDummy(Player* bot, Map* map, float distance)
{
    float const facing = bot->GetOrientation();

    Position pos;
    pos.m_positionX = bot->GetPositionX() + distance * std::cos(facing);
    pos.m_positionY = bot->GetPositionY() + distance * std::sin(facing);
    pos.m_positionZ = bot->GetPositionZ();

    float const ground = map->GetHeight(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 2.0f);
    if (ground > INVALID_HEIGHT)
        pos.m_positionZ = ground;

    pos.SetOrientation(Position::NormalizeOrientation(facing + float(M_PI)));

    // Summon the dummy at the bot's level so hit, dodge, glancing and armor tables are those of an
    // even-level fight rather than a level 80 target.
    PendingSummonLevel = bot->GetLevel();
    TempSummon* dummy = map->SummonCreature(NPC_TRAINING_DUMMY, pos);
    PendingSummonLevel = 0;

    if (!dummy)
    {
        LOG_ERROR("module.animus", "Could not summon training dummy {} for bot {}", uint32(NPC_TRAINING_DUMMY),
            bot->GetName());
        return nullptr;
    }

    dummy->SetCreateHealth(DUMMY_HEALTH);
    dummy->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(DUMMY_HEALTH));
    dummy->UpdateMaxHealth();
    dummy->SetFullHealth();
    dummy->SetRegeneratingHealth(false);

    // Face the dummy exactly: the player melee check needs the target inside the frontal arc.
    bot->SetOrientation(bot->GetAngle(dummy));

    return dummy;
}
