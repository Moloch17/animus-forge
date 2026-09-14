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
#include "AllSpellScript.h"
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

        /// Called for every heal, on map threads, with the health actually gained (overhealing excluded).
        void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override
        {
            if (AnimusForge::EnvPool* pool = sAnimusForge->ActivePool())
                pool->RecordHeal(healer, receiver, gain);
        }
    };

    class AnimusForgeSpellScript : public AllSpellScript
    {
    public:
        AnimusForgeSpellScript() : AllSpellScript("AnimusForgeSpellScript",
            { ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL }) { }

        /// Called on map threads once a spell's cast time is over and it goes off.
        void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool /*skipCheck*/) override
        {
            if (AnimusForge::EnvPool* pool = sAnimusForge->ActivePool())
                pool->RecordCastCompleted(caster, spell);
        }

        /// Called on map threads when a cast or channel is cancelled, with the spell still in its old state.
        void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool bySelf) override
        {
            if (AnimusForge::EnvPool* pool = sAnimusForge->ActivePool())
                pool->RecordCastCancelled(caster, spell, bySelf);
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
    new AnimusForgeSpellScript();
    new AnimusForgeCreatureScript();
}
