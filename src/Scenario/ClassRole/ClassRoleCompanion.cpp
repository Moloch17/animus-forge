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
 * The companion stage of ClassRoleScenario (ArenaMode::Companion), also the owner of the party stage: the scripted
 * owner's lifecycle, follow/assist/guard and owner-heal actions, the owner observations and the role rewards.
 */

#include "ClassRoleScenario.h"
#include "Creature.h"
#include "Env.h"
#include "ForgeBotFactory.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TrainingDummyArena.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr uint32 OWNER_ACCOUNT_OFFSET = 100000;     // owner accounts sit apart from seat accounts
    constexpr int32 OWNER_LEVEL_SPREAD = 2;
    constexpr float OWNER_START_OFFSET = 3.0f;

    // Reward terms added to the gauntlet's.
    constexpr float OWNER_DAMAGE_TAKEN_DPS = 1.0f;      // fraction of the owner's health
    constexpr float OWNER_DAMAGE_TAKEN_PROTECTOR = 2.0f; // tanks and healers exist to prevent it
    constexpr float HEALING = 2.0f;                     // healers: effective healing, fraction of the owner's health
    constexpr float TANK_DAMAGE_REFUND = 0.5f;          // tanks take hits by design: soften the gauntlet's 1.5
    // Per-decision terms are per 50 ms decision (scaled by _decisionScale).
    constexpr float TANK_HOLD = 0.002f;                 // per enemy on the tank, per decision
    constexpr float TANK_LOSE = 0.02f;                  // per enemy on the owner, per decision (tanks)
    constexpr float PULLED_THREAT = 0.004f;             // per enemy on a damage dealer or healer, per decision
    constexpr float SOLO_FIGHT = 0.01f;                 // per decision in combat while the owner is not
    constexpr float FOLLOW_FAR = 0.002f;                // per decision out of combat more than 25 yd away
    constexpr float FOLLOW_NEAR = 0.0005f;              // per decision out of combat within 12 yd
    constexpr float TANK_OWNER_DAMAGE_SHARE = 0.25f;    // a tank owner is hit by design: its damage taken counts this much
    constexpr int32 OWNER_TANK_CHANCE = 25;             // the owner's role: tank, healer, else damage dealer
    constexpr int32 OWNER_HEALER_CHANCE = 25;
    constexpr float OWNER_DEATH = 6.0f;

}

Player* AnimusForge::ClassRoleScenario::FindOwner(EnvData const& data) const
{
    WorldSession* session = data.OwnerSessions[data.OwnerActiveSession];
    return session ? session->GetPlayer() : nullptr;
}

bool AnimusForge::ClassRoleScenario::RebuildOwner(Env& env, Player* anchor, Map* map, uint8 botLevel)
{
    EnvData& data = _data[env.Index];
    Player* oldOwner = FindOwner(data);

    uint8 const level = uint8(std::clamp<int32>(int32(botLevel) + irand(-OWNER_LEVEL_SPREAD, OWNER_LEVEL_SPREAD), 1,
        DEFAULT_MAX_LEVEL));
    // The owner stands in for a player of any role.
    int32 const roll = irand(0, 99);
    Role role = roll < OWNER_TANK_CHANCE ? Role::Tank : roll < OWNER_TANK_CHANCE + OWNER_HEALER_CHANCE ? Role::Heal
        : Role::Dps;
    std::vector<uint8> classes = ClassRoleAssets::ClassesForRole(level, role);
    if (classes.empty())
    {
        role = Role::Dps;
        classes = ClassRoleAssets::ClassesForRole(level, role);
    }
    if (classes.empty())
        return false;

    uint8 const playerClass = classes[urand(0, uint32(classes.size()) - 1)];
    ClassRoleAssets const& assets = ClassRoleAssets::For(*ClassRoleAssets::FindProfile(playerClass, role));

    // Same session and GUID alternation as the seats (see Rebuild).
    uint8 const session = oldOwner ? uint8(1 - data.OwnerActiveSession) : data.OwnerActiveSession;

    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Owner{}{}", env.Index, session ? "b" : "a");
    spec.Race = assets.Races[urand(0, uint32(assets.Races.size()) - 1)];
    spec.Class = playerClass;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = level;
    spec.AccountId = TrainingDummyArena::BOT_ACCOUNT_BASE + OWNER_ACCOUNT_OFFSET + env.Index * 2 + session;
    spec.GuidLow = data.OwnerGuids[session];

    Player* owner = BotFactory::Create(spec, data.OwnerSessions[session]);
    if (!owner)
        return false;

    data.OwnerSessions[session] = owner->GetSession();
    data.OwnerGuids[session] = owner->GetGUID().GetCounter();

    Position start = _arenaPosition;
    start.m_positionX += OWNER_START_OFFSET;
    if (!BotFactory::PlaceInMap(owner, map, start))
    {
        data.OwnerSessions[session] = nullptr;
        BotFactory::DestroyUnplaced(owner);
        return false;
    }

    owner->InitTalentForLevel();
    CompanionOwner::Configure(owner, assets, data.Owner, false);

    // Either faction's races can be paired: give the owner the seats' faction so they are friends (heals and buffs
    // land, neither can attack the other).
    owner->SetFaction(anchor->GetFaction());

    if (oldOwner)
        data.OwnerSessions[data.OwnerActiveSession] = BotFactory::Destroy(oldOwner, true);

    data.OwnerActiveSession = session;
    data.OwnerClass = playerClass;
    data.OwnerRole = role;
    env.Allies = { owner->GetGUID() };
    return true;
}

void AnimusForge::ClassRoleScenario::DestroyOwner(Env& env)
{
    EnvData& data = _data[env.Index];
    if (Player* owner = FindOwner(data))
    {
        BotFactory::Destroy(owner);
        data.OwnerSessions[data.OwnerActiveSession] = nullptr;
    }

    env.Allies.clear();
}

void AnimusForge::ClassRoleScenario::UpdateOwner(Env& env)
{
    EnvData& data = _data[env.Index];
    Player* owner = FindOwner(data);
    if (!owner)
        return;

    std::vector<Unit*> enemies;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Unit* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive())
            enemies.push_back(enemy);

    // The owner plays its role as a party member: a tank owner holds the pull and taunts, a healer owner heals the
    // most hurt of it and the seats, a damage dealer fights the tank's target once a tank has one.
    std::vector<Player*> party = { owner };
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        if (Player* bot = SeatBot(data, seat))
            party.push_back(bot);

    Player* tank = data.OwnerRole == Role::Tank ? owner : HasParty() ? PartyTank(data) : nullptr;
    CompanionOwner::UpdateMember(owner, party, tank, enemies, env.EpisodeElapsedMs, _arenaPosition, data.Owner);
}

float AnimusForge::ClassRoleScenario::CompanionReward(Env& env, uint32 seatIndex, Player* bot)
{
    float reward = PackReward(env, seatIndex, bot);

    EnvData& data = _data[env.Index];
    Seat& seat = data.Seats[seatIndex];
    Player* owner = FindOwner(data);
    if (!bot || !owner)
        return reward;

    AgentStats const& step = env.StepStats[seatIndex];
    Role const role = seat.L->PlayRole();
    float const ownerHealth = float(std::max<uint32>(1, owner->GetMaxHealth()));

    // The owner is ally 0. Its damage taken is the env's: every seat's step stats carry it; count it once.
    if (seatIndex == 0)
        data.OwnerDamageTaken += step.AllyDamageTakenBy[0];
    seat.OwnerHealing += step.AllyHealingBy[0];

    bool const ownerTanks = data.OwnerRole == Role::Tank;
    reward -= (role == Role::Dps ? OWNER_DAMAGE_TAKEN_DPS : OWNER_DAMAGE_TAKEN_PROTECTOR)
        * (ownerTanks ? TANK_OWNER_DAMAGE_SHARE : 1.0f) * float(step.AllyDamageTakenBy[0]) / ownerHealth;

    if (role == Role::Heal)
        reward += HEALING * float(step.AllyHealingBy[0]) / ownerHealth;

    if (role == Role::Tank)
        reward += TANK_DAMAGE_REFUND * seat.LastStepDamageTaken;

    // Who the enemies are fighting.
    uint32 onBot = 0;
    uint32 onOwner = 0;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy || !enemy->IsAlive() || !enemy->IsInCombat())
            continue;

        onBot += enemy->GetVictim() == bot ? 1 : 0;
        onOwner += enemy->GetVictim() == owner ? 1 : 0;
    }

    seat.ThreatOnBot += onBot;
    if (seatIndex == 0)
        data.ThreatOnOwner += onOwner;

    if (role == Role::Tank)
        reward += (TANK_HOLD * float(onBot) - (ownerTanks ? 0.0f : TANK_LOSE * float(onOwner))) * _decisionScale;
    else
        reward -= PULLED_THREAT * float(onBot) * _decisionScale;

    if (owner->IsAlive())
    {
        // Standing again (resurrected, or recovered after a pull): its next death is paid for again.
        seat.OwnerDeathSeen = false;
        if (seatIndex == 0)
            data.OwnerDeathCounted = false;

        // Fighting on its own: the companion pulled something, or kept fighting after the owner stopped (a party's
        // tank pulls first by design).
        if (bot->IsInCombat() && !owner->IsInCombat() && !(IsParty() && role == Role::Tank))
            reward -= SOLO_FIGHT * _decisionScale;

        // Out of combat, stay with the owner.
        if (bot->IsAlive() && !bot->IsInCombat() && !owner->IsInCombat() && owner->IsInMap(bot))
        {
            float const distance = bot->GetDistance(owner);
            if (distance > 25.0f)
                reward -= FOLLOW_FAR * _decisionScale;
            else if (distance < 12.0f)
                reward += FOLLOW_NEAR * _decisionScale;
        }
    }
    else if (!seat.OwnerDeathSeen)
    {
        // Every seat pays for each of the owner's deaths, once.
        seat.OwnerDeathSeen = true;
        data.OwnerDied = true;
        if (!data.OwnerDeathCounted)
        {
            data.OwnerDeathCounted = true;
            ++data.OwnerDeaths;
        }
        reward -= OWNER_DEATH;
    }

    return reward;
}

void AnimusForge::ClassRoleScenario::CompanionEpisodeInfo(Env const& env, uint32 seatIndex, float* info) const
{
    EnvData const& data = _data[env.Index];
    Seat const& seat = data.Seats[seatIndex];

    info[COMPANION_INFO_OWNER_CLASS] = float(data.OwnerClass);
    info[COMPANION_INFO_OWNER_DIED] = data.OwnerDied ? 1.0f : 0.0f;
    info[COMPANION_INFO_OWNER_DAMAGE_TAKEN] = float(data.OwnerDamageTaken);
    info[COMPANION_INFO_OWNER_HEALING] = float(seat.OwnerHealing);
    info[COMPANION_INFO_THREAT_ON_BOT] = float(seat.ThreatOnBot);
    info[COMPANION_INFO_THREAT_ON_OWNER] = float(data.ThreatOnOwner);
    info[COMPANION_INFO_OWNER_ROLE] = float(uint32(data.OwnerRole));
    info[COMPANION_INFO_OWNER_DEATHS] = float(data.OwnerDeaths);
    info[COMPANION_INFO_WIPES] = float(data.Wipes);
    info[COMPANION_INFO_REVIVES] = float(seat.Revives);
}
