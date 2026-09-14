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

/*
 * The duel stage of ClassRoleScenario (ArenaMode::Duel): movement, pet, stop-casting and cancel-form
 * actions, the duel observations and the kill-fast, take-little-damage, finish-your-casts reward.
 */

#include "ClassRoleScenario.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DuelArena.h"
#include "Env.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"
#include <algorithm>
#include <cmath>

namespace
{
    enum DuelSpells : uint32
    {
        SPELL_CALL_PET          = 883,      // its GCD is applied to calling a stabled beast
        SPELL_BATTLE_STANCE     = 2457,
        SPELL_DEFENSIVE_STANCE  = 71,
    };

    constexpr uint32 DUEL_MOVE_POINT_ID = 1;
    constexpr float MELEE_DESIRED_RANGE = 3.5f;
    constexpr float RANGED_DESIRED_RANGE = 25.0f;
    constexpr float BACK_OFF_DISTANCE = 10.0f;
    constexpr uint32 CALL_BEAST_GCD_MS = 1500;
    constexpr uint8 HUNTER_PET_LEVEL = 10;

    // Reward terms. Damage is a fraction of the opponent's health, so a kill is worth DAMAGE_DEALT in
    // damage at any level; taken damage is a fraction of the bot's own health.
    constexpr float DAMAGE_DEALT = 2.0f;
    constexpr float DAMAGE_TAKEN = 1.0f;
    constexpr float APPROACH = 0.5f;            // shaping toward the spec's range, per 40 yd closed
    constexpr float STEALTH_OPENER = 0.5f;
    constexpr float STEP_COST = 0.0002f;
    constexpr float KILL = 2.0f;
    constexpr float FAST_KILL = 3.0f;           // times the fraction of the episode still left
    constexpr float HEALTH_KEPT = 2.0f;         // times the fraction of the bot's health not lost
    constexpr float DEATH = 3.0f;
    // Casting: a cast-time spell cut short (by moving, stopping, an interrupt or death) costs the cast time
    // already spent, and one that finishes in combat earns a little per second of cast time. Neither forces
    // anything: cutting a cast short stays the policy's call when something else is worth more.
    constexpr float CAST_TIME_WASTED = 0.05f;   // per second spent on a cast that did not finish
    constexpr float CAST_TIME_COMPLETED = 0.02f; // per second of cast time of a cast that finished, in combat

    constexpr uint32 IMMOBILE_STATES = UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING;

    Unit* FirstPet(Player* bot)
    {
        if (Pet* pet = bot->GetPet())
            return pet;

        for (Unit* controlled : bot->m_Controlled)
            if (controlled->IsAlive() && !controlled->IsTotem())
                return controlled;

        return nullptr;
    }

    /// A shapeshift the player could cancel from the client (druid forms, Shadowform, Ghost Wolf, Stealth);
    /// stances and presences cannot be.
    SpellInfo const* CancellableForm(Player const* bot)
    {
        for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_MOD_SHAPESHIFT))
        {
            SpellInfo const* info = effect->GetSpellInfo();
            if (!info->HasAttribute(SPELL_ATTR0_NO_AURA_CANCEL) && info->IsPositive() && !info->IsPassive())
                return info;
        }

        return nullptr;
    }
}

float AnimusForge::ClassRoleScenario::DesiredRange(EnvData const& data) const
{
    return _profile.Specs[data.Spec].Range == RangeBand::Melee ? MELEE_DESIRED_RANGE : RANGED_DESIRED_RANGE;
}

void AnimusForge::ClassRoleScenario::StartDuel(Player* bot, Unit* /*opponent*/, EnvData& data) const
{
    // Levelling up mid-episode would change the character under the policy.
    bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);

    data.Stable = _profile.Class == CLASS_HUNTER
        ? DuelArena::OpponentPool::Instance().RandomStable(STABLE_SLOTS) : std::vector<uint32>();

    // A warrior has no stance until one is cast (a first login casts it), and nothing works without one.
    if (_profile.Class == CLASS_WARRIOR)
    {
        uint32 const stance = _profile.PlayRole == Role::Tank && bot->HasSpell(SPELL_DEFENSIVE_STANCE)
            ? SPELL_DEFENSIVE_STANCE : SPELL_BATTLE_STANCE;
        bot->CastSpell(bot, stance, true);
    }

    // No pet and no attack: summoning one, stealthing and approaching are all the policy's to learn.
}

bool AnimusForge::ClassRoleScenario::IsDuelActionAllowed(Player* bot, Unit* opponent, uint32 duelAction,
    EnvData const& data) const
{
    if (!bot->IsAlive())
        return false;

    bool const casting = bot->IsNonMeleeSpellCast(false, false, true);

    // No target needed (between gauntlet pulls too).
    if (duelAction == DUEL_ACTION_STOP_CASTING)
        return casting;
    if (duelAction == DUEL_ACTION_CANCEL_FORM)
        return CancellableForm(bot) != nullptr;

    if (!opponent || !opponent->IsAlive())
        return false;
    bool const canMove = !casting && !bot->HasUnitState(IMMOBILE_STATES);

    switch (duelAction)
    {
        case DUEL_ACTION_MOVE_TO_TARGET:
        case DUEL_ACTION_MOVE_BEHIND:
        case DUEL_ACTION_MOVE_TO_RANGE:
        case DUEL_ACTION_BACK_OFF:
            return canMove;
        case DUEL_ACTION_STOP:
            return !bot->movespline->Finalized();
        case DUEL_ACTION_START_ATTACK:
            return bot->GetVictim() != opponent && bot->IsValidAttackTarget(opponent);
        case DUEL_ACTION_PET_ATTACK:
            return std::any_of(bot->m_Controlled.begin(), bot->m_Controlled.end(), [opponent](Unit* pet)
            {
                return pet->IsAlive() && pet->IsCreature() && pet->GetVictim() != opponent;
            });
        default:
            break;
    }

    uint32 const slot = duelAction - DUEL_ACTION_CALL_BEAST_FIRST;
    if (slot >= data.Stable.size() || casting || bot->GetPetGUID() || bot->GetLevel() < HUNTER_PET_LEVEL)
        return false;

    SpellInfo const* callPet = sSpellMgr->GetSpellInfo(SPELL_CALL_PET);
    return !callPet || !bot->GetGlobalCooldownMgr().HasGlobalCooldown(callPet);
}

void AnimusForge::ClassRoleScenario::ApplyDuelAction(Player* bot, Unit* opponent, uint32 duelAction,
    EnvData& data) const
{
    if (!IsDuelActionAllowed(bot, opponent, duelAction, data))
        return;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    switch (duelAction)
    {
        case DUEL_ACTION_MOVE_TO_TARGET:
            opponent->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), 0.5f, opponent->GetAngle(bot));
            break;
        case DUEL_ACTION_MOVE_BEHIND:
            opponent->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), 0.5f,
                Position::NormalizeOrientation(opponent->GetOrientation() + float(M_PI)));
            break;
        case DUEL_ACTION_MOVE_TO_RANGE:
            opponent->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), RANGED_DESIRED_RANGE - 1.0f,
                opponent->GetAngle(bot));
            break;
        case DUEL_ACTION_BACK_OFF:
            opponent->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), bot->GetDistance(opponent) + BACK_OFF_DISTANCE,
                opponent->GetAngle(bot));
            break;
        case DUEL_ACTION_STOP:
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            return;
        case DUEL_ACTION_START_ATTACK:
            bot->Attack(opponent, true);
            return;
        case DUEL_ACTION_PET_ATTACK:
            DuelArena::PetAttack(bot, opponent);
            return;
        case DUEL_ACTION_STOP_CASTING:
            // As CMSG_CANCEL_CAST / CMSG_CANCEL_CHANNELLING: the current cast or channel, cancelled by the caster.
            bot->InterruptNonMeleeSpells(false, 0, false, true);
            return;
        case DUEL_ACTION_CANCEL_FORM:
            // As CMSG_CANCEL_AURA.
            if (SpellInfo const* form = CancellableForm(bot))
                bot->RemoveOwnedAura(form->Id, ObjectGuid::Empty, 0, AURA_REMOVE_BY_CANCEL);
            return;
        default:
        {
            uint32 const slot = duelAction - DUEL_ACTION_CALL_BEAST_FIRST;
            if (DuelArena::CallHunterBeast(bot, data.Stable[slot]))
                if (SpellInfo const* callPet = sSpellMgr->GetSpellInfo(SPELL_CALL_PET))
                    bot->GetGlobalCooldownMgr().AddGlobalCooldown(callPet, CALL_BEAST_GCD_MS);
            return;
        }
    }

    bot->GetMotionMaster()->Clear();
    bot->GetMotionMaster()->MovePoint(DUEL_MOVE_POINT_ID, x, y, z);
}

void AnimusForge::ClassRoleScenario::ObserveDuel(Env const& env, Player* bot, Unit* opponent, float* obs) const
{
    EnvData const& data = _data[env.Index];
    float* duel = obs + _duelObsFirst;

    // The bot's own casting and form, with or without a target.
    if (Spell* cast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        cast && cast->getState() == SPELL_STATE_PREPARING && cast->GetCastTime() > 0)
    {
        float const total = float(cast->GetCastTime());
        float const left = std::clamp(float(cast->GetCastTimeRemaining()), 0.0f, total);
        duel[DUEL_OBS_CAST_PROGRESS] = 1.0f - left / total;
        duel[DUEL_OBS_CAST_REMAINING] = std::min(1.0f, left / 3000.0f);
    }
    else if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        channel && channel->getState() == SPELL_STATE_CASTING)
    {
        float const left = float(std::max(0, channel->GetCastTimeRemaining()));
        float const total = std::max(left, float(std::max(1, channel->m_spellInfo->GetMaxDuration())));
        duel[DUEL_OBS_CAST_PROGRESS] = 1.0f - left / total;
        duel[DUEL_OBS_CAST_REMAINING] = std::min(1.0f, left / 3000.0f);
    }

    duel[DUEL_OBS_SHAPESHIFTED] = CancellableForm(bot) ? 1.0f : 0.0f;

    duel[DUEL_OBS_EPISODE_TIME] = env.EpisodeLengthMs
        ? std::min(1.0f, float(env.EpisodeElapsedMs) / float(env.EpisodeLengthMs)) : 0.0f;

    // Hunters: what each stable slot offers, so the policy can find the pet it prefers.
    for (uint32 slot = 0; slot < data.Stable.size() && slot < STABLE_SLOTS; ++slot)
    {
        CreatureTemplate const* beast = sObjectMgr->GetCreatureTemplate(data.Stable[slot]);
        if (!beast)
            continue;

        float* features = duel + DUEL_OBS_STABLE_FIRST + slot * STABLE_FEATURES;
        features[0] = 1.0f;
        features[1] = float(beast->family) / 50.0f;

        if (CreatureFamilyEntry const* family = sCreatureFamilyStore.LookupEntry(beast->family))
            if (family->petTalentType >= 0 && family->petTalentType < 3)
                features[2 + family->petTalentType] = 1.0f;     // ferocity, tenacity, cunning
    }

    if (!opponent)
        return;

    float const bearing = bot->GetRelativeAngle(opponent);
    duel[DUEL_OBS_DISTANCE] = std::min(1.0f, bot->GetDistance(opponent) / 60.0f);
    duel[DUEL_OBS_BEARING_SIN] = std::sin(bearing);
    duel[DUEL_OBS_BEARING_COS] = std::cos(bearing);
    duel[DUEL_OBS_BEHIND_TARGET] = opponent->isInBack(bot) ? 1.0f : 0.0f;
    duel[DUEL_OBS_TARGET_FACING_BOT] = opponent->HasInArc(float(M_PI), bot) ? 1.0f : 0.0f;
    duel[DUEL_OBS_TARGET_IN_COMBAT] = opponent->IsInCombat() ? 1.0f : 0.0f;
    duel[DUEL_OBS_TARGET_ATTACKS_BOT] = opponent->GetVictim() == bot ? 1.0f : 0.0f;
    duel[DUEL_OBS_TARGET_CASTING] = opponent->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
    duel[DUEL_OBS_BOT_MOVING] = bot->movespline->Finalized() ? 0.0f : 1.0f;
    duel[DUEL_OBS_BOT_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
    duel[DUEL_OBS_BOT_STEALTHED] = bot->HasAuraType(SPELL_AURA_MOD_STEALTH) ? 1.0f : 0.0f;
    duel[DUEL_OBS_BOT_AUTO_ATTACKING] = bot->GetVictim() == opponent
        && bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) ? 1.0f : 0.0f;
    duel[DUEL_OBS_DAMAGE_TAKEN] = data.LastStepDamageTaken;

    if (Unit* pet = FirstPet(bot))
    {
        duel[DUEL_OBS_PET_OUT] = 1.0f;
        duel[DUEL_OBS_PET_HEALTH] = pet->GetHealthPct() / 100.0f;
        duel[DUEL_OBS_PET_ATTACKING] = pet->GetVictim() == opponent ? 1.0f : 0.0f;
    }
}

float AnimusForge::ClassRoleScenario::DuelReward(Env const& env, Player* bot, Unit* opponent, EnvData& data,
    int8 opponentDead) const
{
    float reward = -STEP_COST;
    if (!bot || !opponent)
        return reward;

    AgentStats const& step = env.StepStats[0];
    float const opponentHealth = float(std::max<uint32>(1, opponent->GetMaxHealth()));
    float const botHealth = float(std::max<uint32>(1, bot->GetMaxHealth()));

    reward += DAMAGE_DEALT * float(step.Damage) / opponentHealth;

    data.DamageTaken += step.DamageTaken;
    data.LastStepDamageTaken = float(step.DamageTaken) / botHealth;
    reward -= DAMAGE_TAKEN * data.LastStepDamageTaken;
    reward += CastReward(bot, step, data);

    // Potential-based shaping on the distance still to close to the spec's range: it pays for getting
    // there and takes it back for leaving, so it cannot be farmed.
    float const excess = std::max(0.0f, bot->GetDistance(opponent) - DesiredRange(data));
    if (data.LastDistance >= 0.0f)
        reward += APPROACH * (data.LastDistance - excess) / 40.0f;
    data.LastDistance = excess;

    if (data.StepStealthOpener)
    {
        reward += STEALTH_OPENER;
        data.StepStealthOpener = false;
    }

    if (bot->GetPetGUID() || FirstPet(bot))
        data.PetSummoned = true;

    bool const opponentDown = opponentDead >= 0 ? opponentDead == 1 : !opponent->IsAlive();
    if (!data.Killed && opponentDown)
    {
        data.Killed = true;
        data.KillTimeMs = env.EpisodeElapsedMs;

        float const timeLeft = env.EpisodeLengthMs
            ? 1.0f - std::min(1.0f, float(env.EpisodeElapsedMs) / float(env.EpisodeLengthMs)) : 0.0f;
        float const healthKept = 1.0f - std::min(1.0f, float(data.DamageTaken) / botHealth);

        reward += KILL + FAST_KILL * timeLeft + HEALTH_KEPT * healthKept;
    }

    if (!data.Died && !bot->IsAlive())
    {
        data.Died = true;
        reward -= DEATH;
    }

    return reward;
}

float AnimusForge::ClassRoleScenario::CastReward(Player* bot, AgentStats const& step, EnvData& data)
{
    data.CastsCompleted += step.CastsCompleted;
    data.CastsCancelled += step.CastsCancelled;
    data.CastMsWasted += step.CastMsWasted;

    float reward = -CAST_TIME_WASTED * float(step.CastMsWasted) / 1000.0f;

    // Only in combat, so casting long spells at nothing is not a way to earn it.
    if (bot->IsInCombat())
        reward += CAST_TIME_COMPLETED * float(step.CastMsCompleted) / 1000.0f;

    return reward;
}

void AnimusForge::ClassRoleScenario::DuelEpisodeInfo(Env const& env, float* info) const
{
    EnvData const& data = _data[env.Index];
    Player* bot = env.FindBot(0);

    info[DUEL_INFO_KILLED] = data.Killed ? 1.0f : 0.0f;
    info[DUEL_INFO_DIED] = data.Died ? 1.0f : 0.0f;
    info[DUEL_INFO_TIME_TO_KILL] = float(data.Killed ? data.KillTimeMs : env.EpisodeElapsedMs) / 1000.0f;
    info[DUEL_INFO_DAMAGE_TAKEN] = float(data.DamageTaken);
    info[DUEL_INFO_HEALTH_LEFT] = bot ? bot->GetHealthPct() / 100.0f : 0.0f;
    info[DUEL_INFO_STEALTH_OPENERS] = float(data.StealthOpeners);
    info[DUEL_INFO_PET_SUMMONED] = data.PetSummoned ? 1.0f : 0.0f;
    info[DUEL_INFO_OPPONENT] = float(data.OpponentEntry);
    info[DUEL_INFO_CASTS_COMPLETED] = float(data.CastsCompleted);
    info[DUEL_INFO_CASTS_CANCELLED] = float(data.CastsCancelled);
    info[DUEL_INFO_CAST_TIME_WASTED] = float(data.CastMsWasted) / 1000.0f;
}
