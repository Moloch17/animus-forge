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
 * The party stage of ClassRoleScenario (ArenaMode::Party): three scripted members that complete a five-player
 * party with the bot and the companion's owner, their lifecycle and scripts, per-member observations,
 * assist/guard/heal actions and the party rewards.
 *
 * The party is not a core Group (whose create, join and leave write to the character database and allocate
 * persistent ids every episode): single-target heals, assists, taunts and threat all work, but spells that
 * need a group (party buffs, party-wide heals) do not reach the other members.
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
    constexpr std::array<uint8, 10> PARTY_CLASSES =
    {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_DEATH_KNIGHT, CLASS_SHAMAN,
        CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };

    constexpr uint32 MEMBER_ACCOUNT_OFFSET = 200000;    // apart from companion (0) and owner (100000) accounts
    constexpr int32 MEMBER_LEVEL_SPREAD = 2;
    constexpr uint32 FOLLOW_TANK_MOVE_POINT_ID = 4;
    constexpr float FOLLOW_TANK_DISTANCE = 4.0f;
    constexpr float FOLLOW_TANK_MIN_DISTANCE = 8.0f;

    // Reward terms added to the companion stage's.
    constexpr float MEMBER_DAMAGE_TAKEN_DPS = 0.5f;     // fraction of the member's health; not for a tank member
    constexpr float MEMBER_DAMAGE_TAKEN_PROTECTOR = 1.0f;
    constexpr float MEMBER_HEALING = 2.0f;              // healers: effective healing, fraction of the member's health
    constexpr float TANK_LOSE_MEMBER = 0.02f;           // tanks: per enemy on a member, per decision
    constexpr float MEMBER_DEATH = 3.0f;

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

Player* AnimusForge::ClassRoleScenario::FindMember(EnvData const& data, uint32 member) const
{
    EnvData::Member const& slot = data.Members[member];
    WorldSession* session = slot.Sessions[slot.ActiveSession];
    return session ? session->GetPlayer() : nullptr;
}

Player* AnimusForge::ClassRoleScenario::PartyTank(Player* bot, EnvData const& data) const
{
    if (_profile.PlayRole == Role::Tank)
        return bot;

    for (uint32 member = 0; member < PARTY_MEMBERS; ++member)
        if (data.Members[member].PlayRole == Role::Tank)
            return FindMember(data, member);

    return nullptr;
}

bool AnimusForge::ClassRoleScenario::RebuildMembers(Env& env, Player* bot, Map* map, EnvData& data) const
{
    CompanionOwner::Templates const& templates = CompanionOwner::Templates::Instance();

    // A tank, a healer and three damage dealers, minus the bot's role and the owner (a damage dealer).
    std::vector<Role> roles = { Role::Tank, Role::Heal, Role::Dps, Role::Dps, Role::Dps };
    roles.erase(std::find(roles.begin(), roles.end(), _profile.PlayRole));
    roles.erase(std::find(roles.begin(), roles.end(), Role::Dps));

    for (uint32 member = 0; member < PARTY_MEMBERS; ++member)
    {
        EnvData::Member& slot = data.Members[member];
        Player* old = FindMember(data, member);

        uint8 const level = uint8(std::clamp<int32>(int32(data.Level) + irand(-MEMBER_LEVEL_SPREAD, MEMBER_LEVEL_SPREAD),
            1, DEFAULT_MAX_LEVEL));

        Role role = roles[member];
        std::vector<uint8> classes = templates.ClassesForRole(level, role);
        if (classes.empty())
        {
            role = Role::Dps;
            classes = templates.ClassesForRole(level, role);
        }
        if (classes.empty())
            return false;

        uint8 const playerClass = classes[urand(0, uint32(classes.size()) - 1)];
        CompanionOwner::Template const* memberTemplate = templates.ForClassRole(playerClass, role);

        // Same session and GUID alternation as the companion (see Rebuild).
        uint8 const session = old ? uint8(1 - slot.ActiveSession) : slot.ActiveSession;

        BotFactory::BotSpec spec;
        spec.Name = Acore::StringFormat("Party{}m{}{}", env.Index, member, session ? "b" : "a");
        spec.Race = memberTemplate->Races[urand(0, uint32(memberTemplate->Races.size()) - 1)];
        spec.Class = playerClass;
        spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
        spec.Level = level;
        spec.AccountId = TrainingDummyArena::BOT_ACCOUNT_BASE + MEMBER_ACCOUNT_OFFSET + env.Index * 2 * PARTY_MEMBERS
            + member * 2 + session;
        spec.GuidLow = slot.Guids[session];

        Player* player = BotFactory::Create(spec, slot.Sessions[session]);
        if (!player)
            return false;

        slot.Sessions[session] = player->GetSession();
        slot.Guids[session] = player->GetGUID().GetCounter();

        Position start = _arenaPosition;
        start.m_positionX -= 3.0f;
        start.m_positionY += (float(member) - 1.0f) * 3.0f;
        if (!BotFactory::PlaceInMap(player, map, start))
        {
            slot.Sessions[session] = nullptr;
            BotFactory::DestroyUnplaced(player);
            return false;
        }

        player->InitTalentForLevel();
        CompanionOwner::Configure(player, *memberTemplate, slot.Script);
        player->SetFaction(bot->GetFaction());

        if (old)
            slot.Sessions[slot.ActiveSession] = BotFactory::Destroy(old, true);

        slot.ActiveSession = session;
        slot.Class = playerClass;
        slot.PlayRole = role;
        slot.Died = false;
        env.Allies.push_back(player->GetGUID());
    }

    return true;
}

void AnimusForge::ClassRoleScenario::DestroyMembers(Env& env, EnvData& data) const
{
    for (uint32 member = 0; member < PARTY_MEMBERS; ++member)
    {
        EnvData::Member& slot = data.Members[member];
        if (Player* player = FindMember(data, member))
        {
            BotFactory::Destroy(player);
            slot.Sessions[slot.ActiveSession] = nullptr;
        }
    }

    if (env.Allies.size() > 1)
        env.Allies.resize(1);
}

void AnimusForge::ClassRoleScenario::ScheduleMembers(Env const& env, EnvData& data) const
{
    // The tank opens, the healer follows, damage dealers wait for threat. When the bot is the tank, the others
    // hold back long enough for it to pull.
    bool const botTanks = _profile.PlayRole == Role::Tank;
    uint32 const now = env.EpisodeElapsedMs;
    uint32 const dpsDelay = botTanks ? urand(4000, 7000) : urand(3000, 5500);

    data.Owner.EngageMs = now + dpsDelay;
    for (EnvData::Member& slot : data.Members)
    {
        switch (slot.PlayRole)
        {
            case Role::Tank: slot.Script.EngageMs = now + urand(1000, 2500); break;
            case Role::Heal: slot.Script.EngageMs = now + urand(1500, 3000); break;
            case Role::Dps: slot.Script.EngageMs = now + dpsDelay + urand(0, 1000); break;
        }
    }
}

void AnimusForge::ClassRoleScenario::UpdateMembers(Env& env, Player* bot, EnvData& data) const
{
    std::vector<Unit*> enemies;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Unit* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive())
            enemies.push_back(enemy);

    std::vector<Player*> party = { bot, FindOwner(data) };
    for (uint32 member = 0; member < PARTY_MEMBERS; ++member)
        party.push_back(FindMember(data, member));

    Player* tank = PartyTank(bot, data);
    for (uint32 member = 0; member < PARTY_MEMBERS; ++member)
        CompanionOwner::UpdateMember(FindMember(data, member), party, tank, enemies, env.EpisodeElapsedMs,
            _arenaPosition, data.Members[member].Script);
}

void AnimusForge::ClassRoleScenario::ObserveParty(Env const& env, Player* bot, float* obs) const
{
    EnvData const& data = _data[env.Index];
    float* party = obs + _partyObsFirst;

    uint32 alive = bot->IsAlive() ? 1 : 0;
    float lowest = 1.0f;
    if (Player* owner = FindOwner(data); owner && owner->IsAlive())
    {
        ++alive;
        lowest = std::min(lowest, owner->GetHealthPct() / 100.0f);
    }

    for (uint32 member = 0; member < PARTY_MEMBERS; ++member)
    {
        Player* player = FindMember(data, member);
        if (!player)
            continue;

        EnvData::Member const& slot = data.Members[member];
        float* features = party + PARTY_OBS_GLOBAL_COUNT + member * MEMBER_FEATURES;
        float const bearing = bot->GetRelativeAngle(player);

        features[MEMBER_PRESENT] = 1.0f;
        features[MEMBER_ALIVE] = player->IsAlive() ? 1.0f : 0.0f;
        features[MEMBER_HEALTH] = player->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = player->GetMaxPower(POWER_MANA))
            features[MEMBER_MANA] = float(player->GetPower(POWER_MANA)) / float(maxMana);
        features[MEMBER_DISTANCE] = std::min(1.0f, bot->GetDistance(player) / 40.0f);
        features[MEMBER_BEARING_SIN] = std::sin(bearing);
        features[MEMBER_BEARING_COS] = std::cos(bearing);
        features[MEMBER_IN_COMBAT] = player->IsInCombat() ? 1.0f : 0.0f;
        features[MEMBER_ROLE_FIRST + uint32(slot.PlayRole)] = 1.0f;

        for (uint32 i = 0; i < PARTY_CLASSES.size(); ++i)
            features[MEMBER_CLASS_FIRST + i] = PARTY_CLASSES[i] == slot.Class ? 1.0f : 0.0f;

        int32 const target = SlotOf(env, player->GetVictim());
        if (target >= 0)
            features[MEMBER_TARGET_FIRST + target] = 1.0f;
        else
            features[MEMBER_NO_TARGET] = 1.0f;

        uint32 attackers = 0;
        for (uint32 enemySlot = 0; enemySlot < env.Targets.size() && enemySlot < PACK_SLOTS; ++enemySlot)
        {
            Unit* enemy = env.FindTargetUnit(enemySlot);
            if (enemy && enemy->IsAlive() && enemy->GetVictim() == player)
            {
                features[MEMBER_SLOT_ON_FIRST + enemySlot] = 1.0f;
                ++attackers;
            }
        }
        features[MEMBER_ATTACKERS] = float(attackers) / float(PACK_SLOTS);

        if (player->IsAlive())
        {
            ++alive;
            lowest = std::min(lowest, player->GetHealthPct() / 100.0f);
            party[PARTY_OBS_HAS_TANK] = std::max(party[PARTY_OBS_HAS_TANK], slot.PlayRole == Role::Tank ? 1.0f : 0.0f);
            party[PARTY_OBS_HAS_HEALER] = std::max(party[PARTY_OBS_HAS_HEALER],
                slot.PlayRole == Role::Heal ? 1.0f : 0.0f);
        }
    }

    party[PARTY_OBS_ALIVE] = float(alive) / float(PARTY_MEMBERS + 2);
    party[PARTY_OBS_LOWEST_HEALTH] = lowest;
}

bool AnimusForge::ClassRoleScenario::IsPartyActionAllowed(Env const& env, Player* bot, uint32 partyAction) const
{
    EnvData const& data = _data[env.Index];
    if (!bot->IsAlive())
        return false;

    if (partyAction == PARTY_ACTION_FOLLOW_TANK)
    {
        Player* tank = PartyTank(bot, data);
        return tank && tank != bot && tank->IsAlive() && !bot->IsNonMeleeSpellCast(false, false, true)
            && !bot->HasUnitState(IMMOBILE_STATES) && bot->GetDistance(tank) > FOLLOW_TANK_MIN_DISTANCE;
    }

    if (partyAction < PARTY_ACTION_GUARD_FIRST)
    {
        Player* member = FindMember(data, partyAction - PARTY_ACTION_ASSIST_FIRST);
        if (!member || !member->IsAlive())
            return false;

        int32 const slot = SlotOf(env, member->GetVictim());
        return slot >= 0 && uint32(slot) != data.TargetSlot && member->GetVictim()->IsAlive();
    }

    if (partyAction < PARTY_ACTION_HEAL_FIRST)
    {
        Player* member = FindMember(data, partyAction - PARTY_ACTION_GUARD_FIRST);
        return member && member->IsAlive() && SlotAttacking(env, member, data.TargetSlot) >= 0;
    }

    uint32 const heals = uint32(_ownerHeals.size());
    uint32 const index = partyAction - PARTY_ACTION_HEAL_FIRST;
    Player* member = heals ? FindMember(data, index / heals) : nullptr;
    if (!member || !member->IsAlive())
        return false;

    SpellInfo const* info = ActionCatalog::KnownRank(bot, _ownerHeals[index % heals].FirstRank);
    if (!info || !bot->HasActiveSpell(info->Id) || bot->HasSpellCooldown(info->Id)
        || bot->GetGlobalCooldownMgr().HasGlobalCooldown(info) || bot->IsNonMeleeSpellCast(false, true, true))
        return false;

    if (!bot->movespline->Finalized() && (info->CalcCastTime(bot) || info->IsChanneled()))
        return false;

    return CanCastOn(bot, info, member);
}

void AnimusForge::ClassRoleScenario::ApplyPartyAction(Env& env, Player* bot, uint32 partyAction, EnvData& data) const
{
    if (!IsPartyActionAllowed(env, bot, partyAction))
        return;

    if (partyAction == PARTY_ACTION_FOLLOW_TANK)
    {
        Player* tank = PartyTank(bot, data);
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
        Player* member = FindMember(data, partyAction - (assist ? PARTY_ACTION_ASSIST_FIRST : PARTY_ACTION_GUARD_FIRST));
        int32 const slot = assist ? SlotOf(env, member->GetVictim()) : SlotAttacking(env, member, data.TargetSlot);
        Unit* enemy = slot >= 0 ? env.FindTargetUnit(uint32(slot)) : nullptr;
        if (!enemy)
            return;

        data.TargetSlot = uint32(slot);
        bot->SetSelection(enemy->GetGUID());
        if (bot->GetVictim())
            bot->Attack(enemy, bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING));
        return;
    }

    uint32 const heals = uint32(_ownerHeals.size());
    uint32 const index = partyAction - PARTY_ACTION_HEAL_FIRST;
    Player* member = FindMember(data, index / heals);
    SpellInfo const* info = ActionCatalog::KnownRank(bot, _ownerHeals[index % heals].FirstRank);
    if (!info)
        return;

    SpellCastTargets targets;
    targets.SetUnitTarget(member);
    Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
    if (spell->prepare(&targets) == SPELL_CAST_OK)
    {
        ++data.SpellCasts;
        ++data.SustainCasts;
    }
}

float AnimusForge::ClassRoleScenario::PartyReward(Env& env, Player* bot, EnvData& data) const
{
    float reward = CompanionReward(env, bot, data);
    if (!bot)
        return reward;

    AgentStats const& step = env.StepStats[0];
    Role const role = _profile.PlayRole;

    for (uint32 member = 0; member < PARTY_MEMBERS; ++member)
    {
        Player* player = FindMember(data, member);
        if (!player)
            continue;

        EnvData::Member& slot = data.Members[member];
        float const health = float(std::max<uint32>(1, player->GetMaxHealth()));
        uint64 const taken = step.AllyDamageTakenBy[member + 1];
        uint64 const healed = step.AllyHealingBy[member + 1];

        data.MemberDamageTaken += taken;
        data.MemberHealing += healed;

        // A tank member is there to be hit; everyone else being hit is what the party wants to avoid.
        if (slot.PlayRole != Role::Tank)
            reward -= (role == Role::Dps ? MEMBER_DAMAGE_TAKEN_DPS : MEMBER_DAMAGE_TAKEN_PROTECTOR)
                * float(taken) / health;

        if (role == Role::Heal)
            reward += MEMBER_HEALING * float(healed) / health;

        if (slot.PlayRole != Role::Tank && player->IsAlive())
        {
            uint32 onMember = 0;
            for (uint32 enemySlot = 0; enemySlot < env.Targets.size(); ++enemySlot)
                if (Unit* enemy = env.FindTargetUnit(enemySlot);
                    enemy && enemy->IsAlive() && enemy->IsInCombat() && enemy->GetVictim() == player)
                    ++onMember;

            data.ThreatOnMembers += onMember;
            if (role == Role::Tank)
                reward -= TANK_LOSE_MEMBER * float(onMember);
        }

        if (!player->IsAlive() && !slot.Died)
        {
            slot.Died = true;
            ++data.MembersDied;
            reward -= MEMBER_DEATH;
        }
    }

    return reward;
}

void AnimusForge::ClassRoleScenario::PartyEpisodeInfo(Env const& env, float* info) const
{
    EnvData const& data = _data[env.Index];

    info[PARTY_INFO_MEMBERS_DIED] = float(data.MembersDied);
    info[PARTY_INFO_MEMBER_DAMAGE_TAKEN] = float(data.MemberDamageTaken);
    info[PARTY_INFO_MEMBER_HEALING] = float(data.MemberHealing);
    info[PARTY_INFO_THREAT_ON_MEMBERS] = float(data.ThreatOnMembers);

    for (EnvData::Member const& slot : data.Members)
    {
        if (slot.PlayRole == Role::Tank)
            info[PARTY_INFO_TANK_CLASS] = float(slot.Class);
        if (slot.PlayRole == Role::Heal)
            info[PARTY_INFO_HEALER_CLASS] = float(slot.Class);
    }
}
