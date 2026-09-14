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
 * The party stage of ClassRoleScenario (ArenaMode::Party): four learned seats -- a tank, a healer and two damage
 * dealers -- and the scripted owner. Each seat observes its three teammates, can assist, guard and heal them, and is
 * rewarded for the party: teammates' damage taken and deaths, healing on them, threat kept off them.
 *
 * The owner and the seats form a real core Group every episode, so party buffs, auras, party-wide heals and every
 * "party member" check work as in play. It is flagged as a sim group (Group::SetSimGroup): it lives only in memory,
 * with no group rows, character cache entries or instance bind changes, so rebuilding it every episode costs no
 * database writes.
 */

#include "ClassRoleScenario.h"
#include "Creature.h"
#include "Env.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <cmath>

namespace
{
    // Reward terms added to the companion stage's.
    constexpr float TEAMMATE_DAMAGE_TAKEN_DPS = 0.5f;       // fraction of the teammate's health; not for a tank
    constexpr float TEAMMATE_DAMAGE_TAKEN_PROTECTOR = 1.0f;
    constexpr float TEAMMATE_HEALING = 2.0f;                // healers: effective healing, fraction of its health
    constexpr float TANK_LOSE_TEAMMATE = 0.02f;             // tanks: per enemy on a non-tank teammate, per decision
    constexpr float TEAMMATE_DEATH = 3.0f;

}

Player* AnimusForge::ClassRoleScenario::PartyTank(EnvData const& data) const
{
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        if (data.Seats[seat].L && data.Seats[seat].L->PlayRole() == Role::Tank)
            if (Player* tank = SeatBot(data, seat); tank && tank->IsAlive())
                return tank;

    return nullptr;
}

void AnimusForge::ClassRoleScenario::FormParty(Env& env)
{
    EnvData& data = _data[env.Index];
    Player* owner = FindOwner(data);
    if (!owner || data.PartyGroup)
        return;

    // The owner stands in for the player whose party the companions join: it leads.
    Group* group = new Group();
    group->SetSimGroup(true);
    if (!group->Create(owner))
    {
        LOG_ERROR("module.animus", "{}: env {} could not create its party", Name(), env.Index);
        delete group;
        return;
    }

    sGroupMgr->AddGroup(group);
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        if (Player* bot = SeatBot(data, seat); bot && !group->AddMember(bot))
            LOG_ERROR("module.animus", "{}: env {} could not add seat {} to its party", Name(), env.Index, seat);

    data.PartyGroup = group;
}

void AnimusForge::ClassRoleScenario::DisbandParty(Env& env)
{
    EnvData& data = _data[env.Index];
    if (!data.PartyGroup)
        return;

    // Disband removes it from the group manager and deletes it.
    data.PartyGroup->Disband(true);
    data.PartyGroup = nullptr;
}

float AnimusForge::ClassRoleScenario::PartyReward(Env& env, uint32 seatIndex, Player* bot)
{
    float reward = CompanionReward(env, seatIndex, bot);
    if (!bot)
        return reward;

    EnvData& data = _data[env.Index];
    Seat& seat = data.Seats[seatIndex];
    AgentStats const& step = env.StepStats[seatIndex];
    Role const role = seat.L->PlayRole();

    for (uint32 slot = 0; slot < PARTY_MEMBERS; ++slot)
    {
        uint32 const teammateSeat = TeammateSeat(seatIndex, slot);
        Player* teammate = teammateSeat < _seatCount ? env.FindBot(teammateSeat) : nullptr;
        if (!teammate)
            continue;

        Role const teammateRole = data.Seats[teammateSeat].L->PlayRole();
        float const health = float(std::max<uint32>(1, teammate->GetMaxHealth()));
        uint64 const taken = env.StepStats[teammateSeat].DamageTaken;
        uint64 const healed = step.AgentHealingBy[teammateSeat];

        seat.TeammateDamageTaken += taken;
        seat.TeammateHealing += healed;

        // A tank is there to be hit; everyone else being hit is what the party wants to avoid.
        if (teammateRole != Role::Tank)
            reward -= (role == Role::Dps ? TEAMMATE_DAMAGE_TAKEN_DPS : TEAMMATE_DAMAGE_TAKEN_PROTECTOR)
                * float(taken) / health;

        if (role == Role::Heal)
            reward += TEAMMATE_HEALING * float(healed) / health;

        if (teammateRole != Role::Tank && teammate->IsAlive())
        {
            uint32 onTeammate = 0;
            for (uint32 enemySlot = 0; enemySlot < env.Targets.size(); ++enemySlot)
                if (Unit* enemy = env.FindTargetUnit(enemySlot);
                    enemy && enemy->IsAlive() && enemy->IsInCombat() && enemy->GetVictim() == teammate)
                    ++onTeammate;

            seat.ThreatOnTeammates += onTeammate;
            if (role == Role::Tank)
                reward -= TANK_LOSE_TEAMMATE * float(onTeammate) * _decisionScale;
        }

        if (!teammate->IsAlive() && !seat.TeammateDeathSeen[teammateSeat])
        {
            seat.TeammateDeathSeen[teammateSeat] = true;
            ++seat.TeammatesDied;
            reward -= TEAMMATE_DEATH;
        }
    }

    return reward;
}

void AnimusForge::ClassRoleScenario::PartyEpisodeInfo(Env const& env, uint32 seatIndex, float* info) const
{
    Seat const& seat = _data[env.Index].Seats[seatIndex];

    info[PARTY_INFO_TEAMMATES_DIED] = float(seat.TeammatesDied);
    info[PARTY_INFO_TEAMMATE_DAMAGE_TAKEN] = float(seat.TeammateDamageTaken);
    info[PARTY_INFO_TEAMMATE_HEALING] = float(seat.TeammateHealing);
    info[PARTY_INFO_THREAT_ON_TEAMMATES] = float(seat.ThreatOnTeammates);
    info[PARTY_INFO_SEAT] = float(seatIndex);
}
