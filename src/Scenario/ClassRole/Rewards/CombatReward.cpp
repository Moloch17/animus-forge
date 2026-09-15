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

#include "CombatReward.h"
#include "ClassRoleScenario.h"
#include "EncoderSupport.h"
#include "Env.h"
#include "Player.h"
#include <algorithm>

std::string_view AnimusForge::ClassRole::RewardTermName(RewardTerm term)
{
    switch (term)
    {
        case RewardTerm::DamageDealt:           return "damage_dealt";
        case RewardTerm::DamageTaken:           return "damage_taken";
        case RewardTerm::StepCost:              return "step_cost";
        case RewardTerm::Casting:               return "casting";
        case RewardTerm::Approach:              return "approach";
        case RewardTerm::StealthOpener:         return "stealth_opener";
        case RewardTerm::Interrupt:             return "interrupt";
        case RewardTerm::Kill:                  return "kill";
        case RewardTerm::Clear:                 return "clear";
        case RewardTerm::HealthKept:            return "health_kept";
        case RewardTerm::Death:                 return "death";
        case RewardTerm::OwnerDamageTaken:      return "owner_damage_taken";
        case RewardTerm::OwnerHealing:          return "owner_healing";
        case RewardTerm::TankDamageRefund:      return "tank_damage_refund";
        case RewardTerm::Threat:                return "threat";
        case RewardTerm::SoloFight:             return "solo_fight";
        case RewardTerm::Follow:                return "follow";
        case RewardTerm::OwnerDeath:            return "owner_death";
        case RewardTerm::TeammateDamageTaken:   return "teammate_damage_taken";
        case RewardTerm::TeammateHealing:       return "teammate_healing";
        case RewardTerm::TeammateThreat:        return "teammate_threat";
        case RewardTerm::TeammateDeath:         return "teammate_death";
        case RewardTerm::Revive:                return "revive";
        case RewardTerm::Count:                 break;
    }

    return "unknown";
}

float AnimusForge::ClassRole::CombatReward::DesiredRange(SeatState const& seat, ClassRoleTuning::DuelTuning const& duel)
{
    return seat.L->Profile->Specs[seat.Spec].Range == RangeBand::Melee ? duel.MeleeRange : duel.RangedRange;
}

float AnimusForge::ClassRole::CombatReward::TimeLeft(Env const& env)
{
    return env.EpisodeLengthMs
        ? 1.0f - std::min(1.0f, float(env.EpisodeElapsedMs) / float(env.EpisodeLengthMs)) : 0.0f;
}

void AnimusForge::ClassRole::CombatReward::Casting(Player* bot, AgentStats const& step, CombatTally& tally,
    ClassRoleTuning::CastingTuning const& tuning, RewardLedger& ledger)
{
    tally.CastsCompleted += step.CastsCompleted;
    tally.CastsCancelled += step.CastsCancelled;
    tally.CastMsWasted += step.CastMsWasted;
    tally.CastsStopped += step.CastsStopped;
    tally.CastsMoved += step.CastsMoved;
    tally.CastsTargetLost += step.CastsTargetLost;
    tally.CastsOther += step.CastsOther;

    // Neither forces anything: cutting a cast short stays the policy's call when something else is worth more.
    float reward = -tuning.TimeWasted * float(step.CastMsWasted) / 1000.0f;

    // Only in combat, so casting long spells at nothing is not a way to earn it.
    if (bot->IsInCombat())
        reward += tuning.TimeCompleted * float(step.CastMsCompleted) / 1000.0f;

    ledger.Add(RewardTerm::Casting, reward);
}

void AnimusForge::ClassRole::CombatReward::Approach(Player* bot, Unit* target, float desiredRange, float weight,
    CombatTally& tally, RewardLedger& ledger)
{
    if (!target)
    {
        tally.LastDistance = -1.0f;
        return;
    }

    float const excess = std::max(0.0f, bot->GetDistance(target) - desiredRange);
    if (tally.LastDistance >= 0.0f)
        ledger.Add(RewardTerm::Approach, weight * (tally.LastDistance - excess) / 40.0f);
    tally.LastDistance = excess;
}

void AnimusForge::ClassRole::CombatReward::OneOnOne(ClassRoleScenario& scenario, Env const& env,
    uint32 seatIndex, Player* bot, Unit* opponent, RewardLedger& ledger)
{
    ClassRoleTuning::DuelTuning const& tuning = scenario.Tuning().Duel;
    SeatState& seat = scenario.Data(env).Seats[seatIndex];
    CombatTally& tally = seat.Combat;

    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * scenario.DecisionScale());
    if (!bot || !opponent)
        return;

    AgentStats const& step = env.StepStats[seatIndex];
    float const opponentHealth = float(std::max<uint32>(1, opponent->GetMaxHealth()));
    float const botHealth = float(std::max<uint32>(1, bot->GetMaxHealth()));

    // Damage is a fraction of the opponent's health, so a kill is worth DamageDealt in damage at any level.
    ledger.Add(RewardTerm::DamageDealt, tuning.DamageDealt * float(step.Damage) / opponentHealth);

    tally.DamageTaken += step.DamageTaken;
    ledger.Add(RewardTerm::DamageTaken, -tuning.DamageTaken * seat.LastStepDamageTaken);

    Casting(bot, step, tally, scenario.Tuning().Casting, ledger);
    Approach(bot, opponent, DesiredRange(seat, tuning), tuning.Approach, tally, ledger);

    if (tally.StepStealthOpener)
    {
        ledger.Add(RewardTerm::StealthOpener, tuning.StealthOpener);
        tally.StepStealthOpener = false;
    }

    if (bot->GetPetGUID() || Encoding::FirstPet(bot))
        tally.PetSummoned = true;

    if (!tally.Killed && !opponent->IsAlive())
    {
        tally.Killed = true;
        tally.KillTimeMs = env.EpisodeElapsedMs;

        float const healthKept = 1.0f - std::min(1.0f, float(tally.DamageTaken) / botHealth);
        ledger.Add(RewardTerm::Kill, tuning.Kill + tuning.FastKill * TimeLeft(env));
        ledger.Add(RewardTerm::HealthKept, tuning.HealthKept * healthKept);
    }

    // Every death costs, including one after resurrecting itself.
    if (!tally.DeathCounted && !bot->IsAlive())
    {
        tally.DeathCounted = true;
        tally.Died = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -tuning.Death);
    }
}
