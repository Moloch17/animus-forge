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

#include "DuelBlock.h"
#include "DBCStores.h"
#include "EncoderSupport.h"
#include "JsonWriter.h"
#include "Layout.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Supplies.h"
#include <algorithm>
#include <cmath>

namespace
{
    using namespace AnimusForge::ClassRole;

    enum DuelSpells : uint32
    {
        SPELL_CALL_PET          = 883,      // its GCD is applied to calling a stabled beast
    };

    constexpr uint32 DUEL_MOVE_POINT_ID = 1;

    uint32 StableSlots(Layout const& layout)
    {
        return layout.Profile->Class == CLASS_HUNTER ? STABLE_SLOTS : 0;
    }

    bool IsAllowed(SeatView const& view, uint32 action)
    {
        Player* bot = view.Bot;
        Unit* target = view.Target;
        if (!bot->IsAlive())
            return false;

        bool const casting = bot->IsNonMeleeSpellCast(false, false, true);

        // No target needed (between gauntlet pulls too).
        if (action == DuelBlock::ACTION_STOP_CASTING)
            return casting;
        if (action == DuelBlock::ACTION_CANCEL_FORM)
            return Encoding::CancellableForm(bot) != nullptr;

        if (!target || !target->IsAlive())
            return false;
        bool const canMove = !casting && !bot->HasUnitState(Encoding::IMMOBILE_STATES);

        switch (action)
        {
            case DuelBlock::ACTION_MOVE_TO_TARGET:
            case DuelBlock::ACTION_MOVE_BEHIND:
            case DuelBlock::ACTION_MOVE_TO_RANGE:
            case DuelBlock::ACTION_BACK_OFF:
                return canMove;
            case DuelBlock::ACTION_STOP:
                return !bot->movespline->Finalized();
            case DuelBlock::ACTION_START_ATTACK:
                return bot->GetVictim() != target && bot->IsValidAttackTarget(target);
            case DuelBlock::ACTION_PET_ATTACK:
                return std::any_of(bot->m_Controlled.begin(), bot->m_Controlled.end(), [target](Unit* pet)
                {
                    return pet->IsAlive() && pet->IsCreature() && pet->GetVictim() != target;
                });
            default:
                break;
        }

        uint32 const slot = action - DuelBlock::ACTION_CALL_BEAST_FIRST;
        if (slot >= view.StableCount || casting || bot->GetPetGUID() || bot->GetLevel() < HUNTER_PET_LEVEL)
            return false;

        SpellInfo const* callPet = sSpellMgr->GetSpellInfo(SPELL_CALL_PET);
        return !callPet || !bot->GetGlobalCooldownMgr().HasGlobalCooldown(callPet);
    }
}

AnimusForge::ClassRole::BlockSize AnimusForge::ClassRole::DuelBlock::Size(Layout const& layout) const
{
    uint32 const stable = StableSlots(layout);
    return { OBS_COUNT_WITHOUT_STABLE + stable * STABLE_FEATURES, ACTION_COUNT_WITHOUT_STABLE + stable };
}

void AnimusForge::ClassRole::DuelBlock::DescribeManifest(Layout const& layout, JsonWriter& json) const
{
    json.Key("stable_slots").Value(StableSlots(layout)).Key("stable_features").Value(STABLE_FEATURES);
}

void AnimusForge::ClassRole::DuelBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;
    Unit* target = view.Target;

    // The bot's own casting and form, with or without a target.
    if (Spell* cast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        cast && cast->getState() == SPELL_STATE_PREPARING && cast->GetCastTime() > 0)
    {
        float const total = float(cast->GetCastTime());
        float const left = std::clamp(float(cast->GetCastTimeRemaining()), 0.0f, total);
        obs[OBS_CAST_PROGRESS] = 1.0f - left / total;
        obs[OBS_CAST_REMAINING] = std::min(1.0f, left / 3000.0f);
    }
    else if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        channel && channel->getState() == SPELL_STATE_CASTING)
    {
        float const left = float(std::max(0, channel->GetCastTimeRemaining()));
        float const total = std::max(left, float(std::max(1, channel->m_spellInfo->GetMaxDuration())));
        obs[OBS_CAST_PROGRESS] = 1.0f - left / total;
        obs[OBS_CAST_REMAINING] = std::min(1.0f, left / 3000.0f);
    }

    obs[OBS_SHAPESHIFTED] = Encoding::CancellableForm(bot) ? 1.0f : 0.0f;
    obs[OBS_COMBAT_TIME] = view.CombatTime;

    // Hunters: what each stable slot offers, so the policy can find the pet it prefers.
    for (uint32 slot = 0; slot < view.StableCount && slot < STABLE_SLOTS; ++slot)
    {
        CreatureTemplate const* beast = sObjectMgr->GetCreatureTemplate(view.Stable[slot]);
        if (!beast)
            continue;

        float* features = obs + OBS_STABLE_FIRST + slot * STABLE_FEATURES;
        features[0] = 1.0f;
        features[1] = float(beast->family) / 50.0f;

        if (CreatureFamilyEntry const* family = sCreatureFamilyStore.LookupEntry(beast->family))
            if (family->petTalentType >= 0 && family->petTalentType < 3)
                features[2 + family->petTalentType] = 1.0f;     // ferocity, tenacity, cunning
    }

    if (target)
    {
        float const bearing = bot->GetRelativeAngle(target);
        obs[OBS_DISTANCE] = std::min(1.0f, bot->GetDistance(target) / 60.0f);
        obs[OBS_BEARING_SIN] = std::sin(bearing);
        obs[OBS_BEARING_COS] = std::cos(bearing);
        obs[OBS_BEHIND_TARGET] = target->isInBack(bot) ? 1.0f : 0.0f;
        obs[OBS_TARGET_FACING_BOT] = target->HasInArc(float(M_PI), bot) ? 1.0f : 0.0f;
        obs[OBS_TARGET_IN_COMBAT] = target->IsInCombat() ? 1.0f : 0.0f;
        obs[OBS_TARGET_ATTACKS_BOT] = target->GetVictim() == bot ? 1.0f : 0.0f;
        obs[OBS_TARGET_CASTING] = target->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
        obs[OBS_BOT_MOVING] = bot->movespline->Finalized() ? 0.0f : 1.0f;
        obs[OBS_BOT_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
        obs[OBS_BOT_STEALTHED] = bot->HasAuraType(SPELL_AURA_MOD_STEALTH) ? 1.0f : 0.0f;
        obs[OBS_BOT_AUTO_ATTACKING] = bot->GetVictim() == target && bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING)
            ? 1.0f : 0.0f;
        obs[OBS_DAMAGE_TAKEN] = view.LastStepDamageTaken;

        if (Unit* pet = Encoding::FirstPet(bot))
        {
            obs[OBS_PET_OUT] = 1.0f;
            obs[OBS_PET_HEALTH] = pet->GetHealthPct() / 100.0f;
            obs[OBS_PET_ATTACKING] = pet->GetVictim() == target ? 1.0f : 0.0f;
        }
    }

    uint32 const actions = view.L->Slice(BlockId::Duel).ActionCount;
    for (uint32 action = 0; action < actions; ++action)
        mask[action] = IsAllowed(view, action) ? 1 : 0;
}

void AnimusForge::ClassRole::DuelBlock::BeforeApply(SeatView& view) const
{
    // Face the target whenever not running somewhere: casts and swings need it, and turning is not a decision worth
    // learning.
    Player* bot = view.Bot;
    Unit* target = view.Target;
    if (target && bot->IsAlive() && bot->movespline->Finalized() && !bot->HasInArc(float(M_PI) / 2, target))
        bot->SetFacingToObject(target);
}

void AnimusForge::ClassRole::DuelBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (!IsAllowed(view, local))
        return;

    Player* bot = view.Bot;
    Unit* target = view.Target;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    switch (local)
    {
        case ACTION_MOVE_TO_TARGET:
            target->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), 0.5f, target->GetAngle(bot));
            break;
        case ACTION_MOVE_BEHIND:
            target->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), 0.5f,
                Position::NormalizeOrientation(target->GetOrientation() + float(M_PI)));
            break;
        case ACTION_MOVE_TO_RANGE:
            target->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), MOVE_TO_RANGE_DISTANCE, target->GetAngle(bot));
            break;
        case ACTION_BACK_OFF:
            target->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), bot->GetDistance(target) + BACK_OFF_DISTANCE,
                target->GetAngle(bot));
            break;
        case ACTION_STOP:
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            return;
        case ACTION_START_ATTACK:
            bot->Attack(target, true);
            return;
        case ACTION_PET_ATTACK:
            Encoding::PetAttack(bot, target);
            return;
        case ACTION_STOP_CASTING:
            // As CMSG_CANCEL_CAST / CMSG_CANCEL_CHANNELLING: the current cast or channel, cancelled by the caster.
            bot->InterruptNonMeleeSpells(false, 0, false, true);
            return;
        case ACTION_CANCEL_FORM:
            // As CMSG_CANCEL_AURA.
            if (SpellInfo const* form = Encoding::CancellableForm(bot))
                bot->RemoveOwnedAura(form->Id, ObjectGuid::Empty, 0, AURA_REMOVE_BY_CANCEL);
            return;
        default:
            result.CallBeast = view.Stable[local - ACTION_CALL_BEAST_FIRST];
            return;
    }

    Encoding::MoveTo(bot, DUEL_MOVE_POINT_ID, x, y, z);
}
