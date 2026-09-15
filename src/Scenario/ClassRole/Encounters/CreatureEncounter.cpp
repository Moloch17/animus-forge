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

#include "Encounters.h"
#include "CombatReward.h"
#include "Creature.h"
#include "Env.h"
#include "Opponents.h"
#include "Player.h"

std::vector<AnimusForge::ClassRole::RewardTerm> AnimusForge::ClassRole::CreatureEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::DamageDealt, RewardTerm::DamageTaken, RewardTerm::Casting,
        RewardTerm::Approach, RewardTerm::StealthOpener, RewardTerm::Kill, RewardTerm::HealthKept, RewardTerm::Death };
}

bool AnimusForge::ClassRole::CreatureEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);
    Player* bot = _scenario.SeatBot(env, 0);

    data.OpponentEntry = Opponents::OpponentPool::Instance().Random(data.Seats[0].Level);
    Creature* opponent = data.OpponentEntry ? Opponents::SpawnOpponent(bot, map, data.OpponentEntry) : nullptr;
    if (!opponent)
        return false;

    env.Targets = { opponent->GetGUID() };

    // No pet and no attack: summoning one, stealthing and approaching are all the policy's to learn.
    _scenario.PrepareFighter(bot, data.Seats[0]);
    return true;
}

void AnimusForge::ClassRole::CreatureEncounter::Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger)
{
    CombatReward::OneOnOne(_scenario, env, seat, bot, env.FindTargetUnit(0), ledger);
}

bool AnimusForge::ClassRole::CreatureEncounter::IsTerminal(Env const& env) const
{
    // A death ends it once no resurrection of its own is left to wait for.
    return _scenario.Data(env).Seats[0].Combat.Killed || _scenario.DeadForGood(env, 0);
}
