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

    constexpr float MELEE_DESIRED_RANGE = 3.5f;
    constexpr float RANGED_DESIRED_RANGE = 25.0f;

    // Reward terms. Damage is a fraction of the opponent's health, so a kill is worth DAMAGE_DEALT in
    // damage at any level; taken damage is a fraction of the bot's own health.
    constexpr float DAMAGE_DEALT = 2.0f;
    constexpr float DAMAGE_TAKEN = 1.0f;
    constexpr float APPROACH = 0.5f;            // shaping toward the spec's range, per 40 yd closed
    constexpr float STEALTH_OPENER = 0.5f;
    constexpr float STEP_COST = 0.0002f;         // per 50 ms decision
    constexpr float KILL = 2.0f;
    constexpr float FAST_KILL = 3.0f;           // times the fraction of the episode still left
    constexpr float HEALTH_KEPT = 2.0f;         // times the fraction of the bot's health not lost
    constexpr float DEATH = 3.0f;
    // Casting: a cast-time spell cut short (by moving, stopping, an interrupt or death) costs the cast time
    // already spent, and one that finishes in combat earns a little per second of cast time. Neither forces
    // anything: cutting a cast short stays the policy's call when something else is worth more.
    constexpr float CAST_TIME_WASTED = 0.03f;   // per second spent on a cast that did not finish
    constexpr float CAST_TIME_COMPLETED = 0.03f; // per second of cast time of a cast that finished, in combat

    Unit* FirstPet(Player* bot)
    {
        if (Pet* pet = bot->GetPet())
            return pet;

        for (Unit* controlled : bot->m_Controlled)
            if (controlled->IsAlive() && !controlled->IsTotem())
                return controlled;

        return nullptr;
    }
}

float AnimusForge::ClassRoleScenario::DesiredRange(Seat const& seat) const
{
    return seat.L->Profile->Specs[seat.Spec].Range == RangeBand::Melee ? MELEE_DESIRED_RANGE : RANGED_DESIRED_RANGE;
}

void AnimusForge::ClassRoleScenario::StartDuel(Player* bot, Seat& seat) const
{
    // Levelling up mid-episode would change the character under the policy.
    bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);

    seat.Stable = seat.L->Profile->Class == CLASS_HUNTER
        ? StablePool::Instance().Random(STABLE_SLOTS) : std::vector<uint32>();

    // A warrior has no stance until one is cast (a first login casts it), and nothing works without one.
    if (seat.L->Profile->Class == CLASS_WARRIOR)
    {
        uint32 const stance = seat.L->PlayRole() == Role::Tank && bot->HasSpell(SPELL_DEFENSIVE_STANCE)
            ? SPELL_DEFENSIVE_STANCE : SPELL_BATTLE_STANCE;
        bot->CastSpell(bot, stance, true);
    }

    // No pet and no attack: summoning one, stealthing and approaching are all the policy's to learn.
}

float AnimusForge::ClassRoleScenario::DuelReward(Env const& env, uint32 seatIndex, Player* bot, Unit* opponent,
    int8 opponentDead)
{
    Seat& seat = _data[env.Index].Seats[seatIndex];
    float reward = -STEP_COST * _decisionScale;
    if (!bot || !opponent)
        return reward;

    AgentStats const& step = env.StepStats[seatIndex];
    float const opponentHealth = float(std::max<uint32>(1, opponent->GetMaxHealth()));
    float const botHealth = float(std::max<uint32>(1, bot->GetMaxHealth()));

    reward += DAMAGE_DEALT * float(step.Damage) / opponentHealth;

    seat.DamageTaken += step.DamageTaken;
    seat.LastStepDamageTaken = float(step.DamageTaken) / botHealth;
    reward -= DAMAGE_TAKEN * seat.LastStepDamageTaken;
    reward += CastReward(bot, step, seat);

    // Potential-based shaping on the distance still to close to the spec's range: it pays for getting
    // there and takes it back for leaving, so it cannot be farmed.
    float const excess = std::max(0.0f, bot->GetDistance(opponent) - DesiredRange(seat));
    if (seat.LastDistance >= 0.0f)
        reward += APPROACH * (seat.LastDistance - excess) / 40.0f;
    seat.LastDistance = excess;

    if (seat.StepStealthOpener)
    {
        reward += STEALTH_OPENER;
        seat.StepStealthOpener = false;
    }

    if (bot->GetPetGUID() || FirstPet(bot))
        seat.PetSummoned = true;

    bool const opponentDown = opponentDead >= 0 ? opponentDead == 1 : !opponent->IsAlive();
    if (!seat.Killed && opponentDown)
    {
        seat.Killed = true;
        seat.KillTimeMs = env.EpisodeElapsedMs;

        float const timeLeft = env.EpisodeLengthMs
            ? 1.0f - std::min(1.0f, float(env.EpisodeElapsedMs) / float(env.EpisodeLengthMs)) : 0.0f;
        float const healthKept = 1.0f - std::min(1.0f, float(seat.DamageTaken) / botHealth);

        reward += KILL + FAST_KILL * timeLeft + HEALTH_KEPT * healthKept;
    }

    if (!seat.Died && !bot->IsAlive())
    {
        seat.Died = true;
        reward -= DEATH;
    }

    return reward;
}

float AnimusForge::ClassRoleScenario::CastReward(Player* bot, AgentStats const& step, Seat& seat)
{
    seat.CastsCompleted += step.CastsCompleted;
    seat.CastsCancelled += step.CastsCancelled;
    seat.CastMsWasted += step.CastMsWasted;

    float reward = -CAST_TIME_WASTED * float(step.CastMsWasted) / 1000.0f;

    // Only in combat, so casting long spells at nothing is not a way to earn it.
    if (bot->IsInCombat())
        reward += CAST_TIME_COMPLETED * float(step.CastMsCompleted) / 1000.0f;

    return reward;
}

void AnimusForge::ClassRoleScenario::DuelEpisodeInfo(Env const& env, uint32 seatIndex, float* info) const
{
    EnvData const& data = _data[env.Index];
    Seat const& seat = data.Seats[seatIndex];
    Player* bot = env.FindBot(seatIndex);

    info[DUEL_INFO_KILLED] = seat.Killed ? 1.0f : 0.0f;
    info[DUEL_INFO_DIED] = seat.Died ? 1.0f : 0.0f;
    info[DUEL_INFO_TIME_TO_KILL] = float(seat.Killed ? seat.KillTimeMs : env.EpisodeElapsedMs) / 1000.0f;
    info[DUEL_INFO_DAMAGE_TAKEN] = float(seat.DamageTaken);
    info[DUEL_INFO_HEALTH_LEFT] = bot ? bot->GetHealthPct() / 100.0f : 0.0f;
    info[DUEL_INFO_STEALTH_OPENERS] = float(seat.StealthOpeners);
    info[DUEL_INFO_PET_SUMMONED] = seat.PetSummoned ? 1.0f : 0.0f;
    info[DUEL_INFO_OPPONENT] = float(data.OpponentEntry);
    info[DUEL_INFO_CASTS_COMPLETED] = float(seat.CastsCompleted);
    info[DUEL_INFO_CASTS_CANCELLED] = float(seat.CastsCancelled);
    info[DUEL_INFO_CAST_TIME_WASTED] = float(seat.CastMsWasted) / 1000.0f;
}
