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

#include "SpellChecks.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include <algorithm>

float AnimusForge::SpellChecks::CooldownFraction(Player const* bot, SpellInfo const* info)
{
    if (!info)
        return 0.0f;

    uint32 const full = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
    return full ? std::min(1.0f, float(bot->GetSpellCooldownDelay(info->Id)) / float(full)) : 0.0f;
}

float AnimusForge::SpellChecks::AuraFraction(Unit const* unit, uint32 spellId, ObjectGuid caster, float* stacks)
{
    Aura const* aura = unit->GetAura(spellId, caster);
    if (!aura)
        return 0.0f;

    if (stacks)
        *stacks = std::max(*stacks, std::min(1.0f, float(std::max(aura->GetStackAmount(), aura->GetCharges())) / 5.0f));

    if (aura->GetMaxDuration() <= 0)
        return 1.0f;    // permanent (stances, forms, presences, auras)

    return std::clamp(float(aura->GetDuration()) / float(aura->GetMaxDuration()), 0.0f, 1.0f);
}

bool AnimusForge::SpellChecks::CheckCast(Player* bot, SpellInfo const* info, SpellCastTargets const& targets,
    Item* castItem)
{
    // Build the spell, validate it, throw it away.
    Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
    spell->m_CastItem = castItem;
    spell->LoadScripts();
    spell->InitExplicitTargets(targets);

    SpellCastResult const result = spell->CheckCast(true);
    delete spell;
    return result == SPELL_CAST_OK;
}
