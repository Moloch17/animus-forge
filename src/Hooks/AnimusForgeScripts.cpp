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

#include "AllCreatureScript.h"
#include "AnimusForge.h"
#include "SummonLevel.h"
#include "UnitScript.h"
#include "WorldScript.h"

namespace
{
    class AnimusForgeWorldScript : public WorldScript
    {
    public:
        AnimusForgeWorldScript() : WorldScript("AnimusForgeWorldScript",
            { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) { }

        void OnStartup() override { sAnimusForge->OnStartup(); }
        void OnUpdate(uint32 diff) override { sAnimusForge->OnUpdate(diff); }
        void OnShutdown() override { sAnimusForge->OnShutdown(); }
    };

    class AnimusForgeUnitScript : public UnitScript
    {
    public:
        AnimusForgeUnitScript() : UnitScript("AnimusForgeUnitScript") { }

        /// Called for every damage event, on map threads, before the victim's AI can change the
        /// amount -- npc_training_dummy zeroes it in DamageTaken, so OnDamage would only see 0.
        uint32 DealDamage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType type) override
        {
            if (AnimusForge::EnvPool* pool = sAnimusForge->ActivePool())
                pool->RecordDamage(attacker, victim, damage, type);

            return damage;
        }
    };

    class AnimusForgeCreatureScript : public AllCreatureScript
    {
    public:
        AnimusForgeCreatureScript() : AllCreatureScript("AnimusForgeCreatureScript") { }

        void OnBeforeCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* /*creature*/,
            uint8& level) override
        {
            if (AnimusForge::PendingSummonLevel)
                level = AnimusForge::PendingSummonLevel;
        }
    };
}

void AddSC_animus_forge()
{
    new AnimusForgeWorldScript();
    new AnimusForgeUnitScript();
    new AnimusForgeCreatureScript();
}
