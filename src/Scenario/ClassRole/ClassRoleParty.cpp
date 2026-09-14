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
 * The party is not a core Group (whose create, join and leave write to the character database and allocate
 * persistent ids every episode): single-target heals, assists, taunts and threat all work, but spells that need a
 * group (party buffs, party-wide heals) do not reach the others.
 */

#include "ClassRoleScenario.h"
#include "Creature.h"
#include "Env.h"
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
    constexpr std::array<uint8, 10> PARTY_CLASSES =
    {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_DEATH_KNIGHT, CLASS_SHAMAN,
        CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };

    constexpr uint32 FOLLOW_TANK_MOVE_POINT_ID = 4;
    constexpr float FOLLOW_TANK_DISTANCE = 4.0f;
    constexpr float FOLLOW_TANK_MIN_DISTANCE = 8.0f;

    // Reward terms added to the companion stage's.
    constexpr float TEAMMATE_DAMAGE_TAKEN_DPS = 0.5f;       // fraction of the teammate's health; not for a tank
    constexpr float TEAMMATE_DAMAGE_TAKEN_PROTECTOR = 1.0f;
    constexpr float TEAMMATE_HEALING = 2.0f;                // healers: effective healing, fraction of its health
    constexpr float TANK_LOSE_TEAMMATE = 0.02f;             // tanks: per enemy on a non-tank teammate, per decision
    constexpr float TEAMMATE_DEATH = 3.0f;

    constexpr uint32 IMMOBILE_STATES = UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING;

    bool CanCastOn(Player* bot, SpellInfo const* info, Unit* target)
    {
        Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
        spell->LoadScripts();

        SpellCastTargets targets;
        targets.SetUnitTarget(target);
        spell->InitExplicitTargets(targets);

        SpellCastResult const result = spell->CheckCast(true);
        delete spell;
        return result == SPELL_CAST_OK;
    }

    int32 SlotOf(AnimusForge::Env const& env, Unit const* unit)
    {
        if (!unit)
            return -1;

        for (uint32 slot = 0; slot < env.Targets.size() && slot < AnimusForge::ClassRoleScenario::PACK_SLOTS; ++slot)
            if (env.Targets[slot] == unit->GetGUID())
                return int32(slot);

        return -1;
    }

    /// An enemy slot, other than `except`, whose living enemy attacks `victim`; -1 if none.
    int32 SlotAttacking(AnimusForge::Env const& env, Unit const* victim, uint32 except)
    {
        for (uint32 slot = 0; slot < env.Targets.size() && slot < AnimusForge::ClassRoleScenario::PACK_SLOTS; ++slot)
            if (Unit* enemy = env.FindTargetUnit(slot);
                enemy && enemy->IsAlive() && enemy->GetVictim() == victim && slot != except)
                return int32(slot);

        return -1;
    }
}

Player* AnimusForge::ClassRoleScenario::PartyTank(EnvData const& data) const
{
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        if (data.Seats[seat].L && data.Seats[seat].L->PlayRole() == Role::Tank)
            if (Player* tank = SeatBot(data, seat); tank && tank->IsAlive())
                return tank;

    return nullptr;
}

void AnimusForge::ClassRoleScenario::ObserveParty(Env const& env, uint32 seatIndex, Player* bot, float* obs) const
{
    EnvData const& data = _data[env.Index];
    float* party = obs + data.Seats[seatIndex].L->PartyObsFirst;

    uint32 alive = bot->IsAlive() ? 1 : 0;
    float lowest = 1.0f;
    if (Player* owner = FindOwner(data); owner && owner->IsAlive())
    {
        ++alive;
        lowest = std::min(lowest, owner->GetHealthPct() / 100.0f);
    }

    for (uint32 slot = 0; slot < PARTY_MEMBERS; ++slot)
    {
        uint32 const teammateSeat = TeammateSeat(seatIndex, slot);
        Player* teammate = teammateSeat < _seatCount ? env.FindBot(teammateSeat) : nullptr;
        if (!teammate)
            continue;

        Seat const& other = data.Seats[teammateSeat];
        float* features = party + PARTY_OBS_GLOBAL_COUNT + slot * MEMBER_FEATURES;
        float const bearing = bot->GetRelativeAngle(teammate);

        features[MEMBER_PRESENT] = 1.0f;
        features[MEMBER_ALIVE] = teammate->IsAlive() ? 1.0f : 0.0f;
        features[MEMBER_HEALTH] = teammate->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = teammate->GetMaxPower(POWER_MANA))
            features[MEMBER_MANA] = float(teammate->GetPower(POWER_MANA)) / float(maxMana);
        features[MEMBER_DISTANCE] = std::min(1.0f, bot->GetDistance(teammate) / 40.0f);
        features[MEMBER_BEARING_SIN] = std::sin(bearing);
        features[MEMBER_BEARING_COS] = std::cos(bearing);
        features[MEMBER_IN_COMBAT] = teammate->IsInCombat() ? 1.0f : 0.0f;
        features[MEMBER_ROLE_FIRST + uint32(other.L->PlayRole())] = 1.0f;

        for (uint32 i = 0; i < PARTY_CLASSES.size(); ++i)
            features[MEMBER_CLASS_FIRST + i] = PARTY_CLASSES[i] == other.L->Profile->Class ? 1.0f : 0.0f;

        int32 const target = SlotOf(env, teammate->GetVictim());
        if (target >= 0)
            features[MEMBER_TARGET_FIRST + target] = 1.0f;
        else
            features[MEMBER_NO_TARGET] = 1.0f;

        uint32 attackers = 0;
        for (uint32 enemySlot = 0; enemySlot < env.Targets.size() && enemySlot < PACK_SLOTS; ++enemySlot)
        {
            Unit* enemy = env.FindTargetUnit(enemySlot);
            if (enemy && enemy->IsAlive() && enemy->GetVictim() == teammate)
            {
                features[MEMBER_SLOT_ON_FIRST + enemySlot] = 1.0f;
                ++attackers;
            }
        }
        features[MEMBER_ATTACKERS] = float(attackers) / float(PACK_SLOTS);

        if (teammate->IsAlive())
        {
            ++alive;
            lowest = std::min(lowest, teammate->GetHealthPct() / 100.0f);
            if (other.L->PlayRole() == Role::Tank)
                party[PARTY_OBS_HAS_TANK] = 1.0f;
            if (other.L->PlayRole() == Role::Heal)
                party[PARTY_OBS_HAS_HEALER] = 1.0f;
        }
    }

    party[PARTY_OBS_ALIVE] = float(alive) / float(PARTY_MEMBERS + 2);
    party[PARTY_OBS_LOWEST_HEALTH] = lowest;
}

bool AnimusForge::ClassRoleScenario::IsPartyActionAllowed(Env const& env, uint32 seatIndex, Player* bot,
    uint32 partyAction) const
{
    EnvData const& data = _data[env.Index];
    Seat const& seat = data.Seats[seatIndex];
    if (!bot->IsAlive())
        return false;

    if (partyAction == PARTY_ACTION_FOLLOW_TANK)
    {
        Player* tank = PartyTank(data);
        return tank && tank != bot && !bot->IsNonMeleeSpellCast(false, false, true)
            && !bot->HasUnitState(IMMOBILE_STATES) && bot->GetDistance(tank) > FOLLOW_TANK_MIN_DISTANCE;
    }

    if (partyAction < PARTY_ACTION_GUARD_FIRST)
    {
        uint32 const teammateSeat = TeammateSeat(seatIndex, partyAction - PARTY_ACTION_ASSIST_FIRST);
        Player* teammate = teammateSeat < _seatCount ? env.FindBot(teammateSeat) : nullptr;
        if (!teammate || !teammate->IsAlive())
            return false;

        int32 const slot = SlotOf(env, teammate->GetVictim());
        return slot >= 0 && uint32(slot) != seat.TargetSlot && teammate->GetVictim()->IsAlive();
    }

    if (partyAction < PARTY_ACTION_HEAL_FIRST)
    {
        uint32 const teammateSeat = TeammateSeat(seatIndex, partyAction - PARTY_ACTION_GUARD_FIRST);
        Player* teammate = teammateSeat < _seatCount ? env.FindBot(teammateSeat) : nullptr;
        return teammate && teammate->IsAlive() && SlotAttacking(env, teammate, seat.TargetSlot) >= 0;
    }

    uint32 const heals = uint32(seat.L->AllyHeals.size());
    uint32 const index = partyAction - PARTY_ACTION_HEAL_FIRST;
    uint32 const teammateSeat = heals ? TeammateSeat(seatIndex, index / heals) : _seatCount;
    Player* teammate = teammateSeat < _seatCount ? env.FindBot(teammateSeat) : nullptr;
    if (!teammate || !teammate->IsAlive())
        return false;

    SpellInfo const* info = ActionCatalog::KnownRank(bot, seat.L->AllyHeals[index % heals].FirstRank);
    if (!info || !bot->HasActiveSpell(info->Id) || bot->HasSpellCooldown(info->Id)
        || bot->GetGlobalCooldownMgr().HasGlobalCooldown(info) || bot->IsNonMeleeSpellCast(false, true, true))
        return false;

    if (!bot->movespline->Finalized() && (info->CalcCastTime(bot) || info->IsChanneled()))
        return false;

    return CanCastOn(bot, info, teammate);
}

void AnimusForge::ClassRoleScenario::ApplyPartyAction(Env& env, uint32 seatIndex, Player* bot, uint32 partyAction)
{
    if (!IsPartyActionAllowed(env, seatIndex, bot, partyAction))
        return;

    EnvData& data = _data[env.Index];
    Seat& seat = data.Seats[seatIndex];

    if (partyAction == PARTY_ACTION_FOLLOW_TANK)
    {
        Player* tank = PartyTank(data);
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        tank->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), FOLLOW_TANK_DISTANCE,
            Position::NormalizeOrientation(tank->GetOrientation() + float(M_PI)));
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(FOLLOW_TANK_MOVE_POINT_ID, x, y, z);
        return;
    }

    if (partyAction < PARTY_ACTION_HEAL_FIRST)
    {
        bool const assist = partyAction < PARTY_ACTION_GUARD_FIRST;
        uint32 const teammateSeat = TeammateSeat(seatIndex,
            partyAction - (assist ? PARTY_ACTION_ASSIST_FIRST : PARTY_ACTION_GUARD_FIRST));
        Player* teammate = env.FindBot(teammateSeat);
        int32 const slot = assist ? SlotOf(env, teammate->GetVictim()) : SlotAttacking(env, teammate, seat.TargetSlot);
        Unit* enemy = slot >= 0 ? env.FindTargetUnit(uint32(slot)) : nullptr;
        if (!enemy)
            return;

        seat.TargetSlot = uint32(slot);
        bot->SetSelection(enemy->GetGUID());
        if (bot->GetVictim())
            bot->Attack(enemy, bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING));
        return;
    }

    uint32 const heals = uint32(seat.L->AllyHeals.size());
    uint32 const index = partyAction - PARTY_ACTION_HEAL_FIRST;
    Player* teammate = env.FindBot(TeammateSeat(seatIndex, index / heals));
    SpellInfo const* info = ActionCatalog::KnownRank(bot, seat.L->AllyHeals[index % heals].FirstRank);
    if (!info)
        return;

    SpellCastTargets targets;
    targets.SetUnitTarget(teammate);
    Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
    if (spell->prepare(&targets) == SPELL_CAST_OK)
    {
        ++seat.SpellCasts;
        ++seat.SustainCasts;
    }
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
                reward -= TANK_LOSE_TEAMMATE * float(onTeammate);
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
