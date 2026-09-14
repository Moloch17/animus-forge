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
 * The companion stage of ClassRoleScenario (ArenaMode::Companion): the scripted owner's lifecycle,
 * follow/assist/guard and owner-heal actions, the owner observations and the role rewards.
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
    constexpr std::array<uint8, 10> OWNER_CLASSES =
    {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_DEATH_KNIGHT, CLASS_SHAMAN,
        CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };

    constexpr uint32 OWNER_ACCOUNT_OFFSET = 100000;     // owner accounts sit apart from companion accounts
    constexpr int32 OWNER_LEVEL_SPREAD = 2;
    constexpr float OWNER_START_OFFSET = 3.0f;
    constexpr uint32 FOLLOW_MOVE_POINT_ID = 3;
    constexpr float FOLLOW_DISTANCE = 2.0f;
    constexpr float FOLLOW_MIN_DISTANCE = 4.0f;

    // Reward terms added to the gauntlet's.
    constexpr float OWNER_DAMAGE_TAKEN_DPS = 1.0f;      // fraction of the owner's health
    constexpr float OWNER_DAMAGE_TAKEN_PROTECTOR = 2.0f; // tanks and healers exist to prevent it
    constexpr float HEALING = 2.0f;                     // healers: effective healing, fraction of the owner's health
    constexpr float TANK_DAMAGE_REFUND = 0.5f;          // tanks take hits by design: soften the gauntlet's 1.5
    constexpr float TANK_HOLD = 0.01f;                  // per enemy on the tank, per decision
    constexpr float TANK_LOSE = 0.02f;                  // per enemy on the owner, per decision (tanks)
    constexpr float PULLED_THREAT = 0.01f;              // per enemy on a damage dealer or healer, per decision
    constexpr float SOLO_FIGHT = 0.01f;                 // per decision in combat while the owner is not
    constexpr float FOLLOW_FAR = 0.002f;                // per decision out of combat more than 25 yd away
    constexpr float FOLLOW_NEAR = 0.0005f;              // per decision out of combat within 12 yd
    constexpr float OWNER_DEATH = 6.0f;

    constexpr uint32 IMMOBILE_STATES = UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING;

    float CooldownFraction(Player const* bot, SpellInfo const* info)
    {
        uint32 const full = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
        return full ? std::min(1.0f, float(bot->GetSpellCooldownDelay(info->Id)) / float(full)) : 0.0f;
    }

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
}

Player* AnimusForge::ClassRoleScenario::FindOwner(EnvData const& data) const
{
    WorldSession* session = data.OwnerSessions[data.OwnerActiveSession];
    return session ? session->GetPlayer() : nullptr;
}

bool AnimusForge::ClassRoleScenario::RebuildOwner(Env& env, Player* bot, Map* map, EnvData& data) const
{
    CompanionOwner::Templates const& templates = CompanionOwner::Templates::Instance();
    Player* oldOwner = FindOwner(data);

    uint8 const level = uint8(std::clamp<int32>(int32(data.Level) + irand(-OWNER_LEVEL_SPREAD, OWNER_LEVEL_SPREAD), 1,
        DEFAULT_MAX_LEVEL));
    std::vector<uint8> const classes = templates.ClassesForLevel(level);
    if (classes.empty())
        return false;

    uint8 const playerClass = classes[urand(0, uint32(classes.size()) - 1)];
    CompanionOwner::Template const* ownerTemplate = templates.ForClass(playerClass);

    // Same session and GUID alternation as the companion (see Rebuild).
    uint8 const session = oldOwner ? uint8(1 - data.OwnerActiveSession) : data.OwnerActiveSession;

    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Owner{}{}", env.Index, session ? "b" : "a");
    spec.Race = ownerTemplate->Races[urand(0, uint32(ownerTemplate->Races.size()) - 1)];
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
    CompanionOwner::Configure(owner, *ownerTemplate, data.Owner);

    // Either faction's races can be paired: give the owner the companion's faction so they are friends
    // (heals and buffs land, neither can attack the other).
    owner->SetFaction(bot->GetFaction());

    if (oldOwner)
        data.OwnerSessions[data.OwnerActiveSession] = BotFactory::Destroy(oldOwner, true);

    data.OwnerActiveSession = session;
    data.OwnerClass = playerClass;
    env.Allies = { owner->GetGUID() };
    return true;
}

void AnimusForge::ClassRoleScenario::DestroyOwner(Env& env, EnvData& data) const
{
    if (Player* owner = FindOwner(data))
    {
        BotFactory::Destroy(owner);
        data.OwnerSessions[data.OwnerActiveSession] = nullptr;
    }

    env.Allies.clear();
}

void AnimusForge::ClassRoleScenario::UpdateOwner(Env& env, EnvData& data) const
{
    Player* owner = FindOwner(data);
    if (!owner)
        return;

    std::vector<Unit*> enemies;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Unit* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive())
            enemies.push_back(enemy);

    // In a party the owner fights the tank's target once the tank has one.
    Player* tank = HasParty() ? PartyTank(env.FindBot(0), data) : nullptr;
    Unit* preferred = tank && tank != owner && tank->IsAlive() ? tank->GetVictim() : nullptr;
    CompanionOwner::Update(owner, enemies, env.EpisodeElapsedMs, _arenaPosition, data.Owner, preferred);
}

void AnimusForge::ClassRoleScenario::ObserveCompanion(Env const& env, Player* bot, float* obs) const
{
    EnvData const& data = _data[env.Index];
    float* companion = obs + _companionObsFirst;

    Player* owner = FindOwner(data);
    if (owner)
    {
        float const bearing = bot->GetRelativeAngle(owner);
        companion[COMPANION_OBS_OWNER_PRESENT] = 1.0f;
        companion[COMPANION_OBS_OWNER_ALIVE] = owner->IsAlive() ? 1.0f : 0.0f;
        companion[COMPANION_OBS_OWNER_HEALTH] = owner->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = owner->GetMaxPower(POWER_MANA))
            companion[COMPANION_OBS_OWNER_MANA] = float(owner->GetPower(POWER_MANA)) / float(maxMana);
        companion[COMPANION_OBS_OWNER_DISTANCE] = std::min(1.0f, bot->GetDistance(owner) / 40.0f);
        companion[COMPANION_OBS_OWNER_BEARING_SIN] = std::sin(bearing);
        companion[COMPANION_OBS_OWNER_BEARING_COS] = std::cos(bearing);
        companion[COMPANION_OBS_OWNER_IN_COMBAT] = owner->IsInCombat() ? 1.0f : 0.0f;
        companion[COMPANION_OBS_OWNER_MOVING] = owner->movespline->Finalized() ? 0.0f : 1.0f;
        companion[COMPANION_OBS_OWNER_LEVEL_DIFF] = (float(owner->GetLevel()) - float(bot->GetLevel())) / 5.0f;

        for (uint32 i = 0; i < OWNER_CLASSES.size(); ++i)
            companion[COMPANION_OBS_OWNER_CLASS_FIRST + i] = OWNER_CLASSES[i] == owner->getClass() ? 1.0f : 0.0f;

        int32 const ownerTarget = SlotOf(env, owner->GetVictim());
        if (ownerTarget >= 0)
            companion[COMPANION_OBS_OWNER_TARGET_FIRST + ownerTarget] = 1.0f;
        else
            companion[COMPANION_OBS_OWNER_NO_TARGET] = 1.0f;

        uint32 attackers = 0;
        for (uint32 slot = 0; slot < env.Targets.size() && slot < PACK_SLOTS; ++slot)
        {
            Unit* enemy = env.FindTargetUnit(slot);
            if (enemy && enemy->IsAlive() && enemy->GetVictim() == owner)
            {
                companion[COMPANION_OBS_SLOT_ON_OWNER_FIRST + slot] = 1.0f;
                ++attackers;
            }
        }

        companion[COMPANION_OBS_OWNER_ATTACKERS] = float(attackers) / float(PACK_SLOTS);
    }

    float* heals = companion + COMPANION_OBS_GLOBAL_COUNT;
    for (uint32 i = 0; i < _ownerHeals.size(); ++i)
    {
        if (SpellInfo const* info = ActionCatalog::KnownRank(bot, _ownerHeals[i].FirstRank))
        {
            heals[i * 2] = 1.0f;
            heals[i * 2 + 1] = CooldownFraction(bot, info);
        }
    }
}

bool AnimusForge::ClassRoleScenario::IsCompanionActionAllowed(Env const& env, Player* bot, uint32 companionAction) const
{
    EnvData const& data = _data[env.Index];
    Player* owner = FindOwner(data);
    if (!owner || !owner->IsAlive() || !bot->IsAlive())
        return false;

    bool const casting = bot->IsNonMeleeSpellCast(false, false, true);

    switch (companionAction)
    {
        case COMPANION_ACTION_FOLLOW:
            return !casting && !bot->HasUnitState(IMMOBILE_STATES) && bot->GetDistance(owner) > FOLLOW_MIN_DISTANCE;
        case COMPANION_ACTION_ASSIST:
        {
            int32 const slot = SlotOf(env, owner->GetVictim());
            return slot >= 0 && uint32(slot) != data.TargetSlot && owner->GetVictim()->IsAlive();
        }
        case COMPANION_ACTION_GUARD:
        {
            for (uint32 slot = 0; slot < env.Targets.size() && slot < PACK_SLOTS; ++slot)
                if (Unit* enemy = env.FindTargetUnit(slot);
                    enemy && enemy->IsAlive() && enemy->GetVictim() == owner && slot != data.TargetSlot)
                    return true;
            return false;
        }
        default:
            break;
    }

    ActionCatalog::Action const& heal = _ownerHeals[companionAction - COMPANION_ACTION_HEAL_FIRST];
    SpellInfo const* info = ActionCatalog::KnownRank(bot, heal.FirstRank);
    if (!info || !bot->HasActiveSpell(info->Id) || bot->HasSpellCooldown(info->Id)
        || bot->GetGlobalCooldownMgr().HasGlobalCooldown(info) || bot->IsNonMeleeSpellCast(false, true, true))
        return false;

    if (!bot->movespline->Finalized() && (info->CalcCastTime(bot) || info->IsChanneled()))
        return false;

    return CanCastOn(bot, info, owner);
}

void AnimusForge::ClassRoleScenario::ApplyCompanionAction(Env& env, Player* bot, uint32 companionAction,
    EnvData& data) const
{
    if (!IsCompanionActionAllowed(env, bot, companionAction))
        return;

    Player* owner = FindOwner(data);

    switch (companionAction)
    {
        case COMPANION_ACTION_FOLLOW:
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            owner->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), FOLLOW_DISTANCE,
                Position::NormalizeOrientation(owner->GetOrientation() + float(M_PI)));
            bot->GetMotionMaster()->Clear();
            bot->GetMotionMaster()->MovePoint(FOLLOW_MOVE_POINT_ID, x, y, z);
            return;
        }
        case COMPANION_ACTION_ASSIST:
        case COMPANION_ACTION_GUARD:
        {
            int32 slot = -1;
            if (companionAction == COMPANION_ACTION_ASSIST)
                slot = SlotOf(env, owner->GetVictim());
            else
                for (uint32 candidate = 0; candidate < env.Targets.size() && candidate < PACK_SLOTS && slot < 0;
                    ++candidate)
                    if (Unit* enemy = env.FindTargetUnit(candidate); enemy && enemy->IsAlive()
                        && enemy->GetVictim() == owner && candidate != data.TargetSlot)
                        slot = int32(candidate);

            Unit* enemy = slot >= 0 ? env.FindTargetUnit(uint32(slot)) : nullptr;
            if (!enemy)
                return;

            data.TargetSlot = uint32(slot);
            bot->SetSelection(enemy->GetGUID());
            if (bot->GetVictim())
                bot->Attack(enemy, bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING));
            return;
        }
        default:
            break;
    }

    ActionCatalog::Action const& heal = _ownerHeals[companionAction - COMPANION_ACTION_HEAL_FIRST];
    SpellInfo const* info = ActionCatalog::KnownRank(bot, heal.FirstRank);
    if (!info)
        return;

    SpellCastTargets targets;
    targets.SetUnitTarget(owner);
    Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
    if (spell->prepare(&targets) == SPELL_CAST_OK)
    {
        ++data.SpellCasts;
        ++data.SustainCasts;
    }
}

float AnimusForge::ClassRoleScenario::CompanionReward(Env& env, Player* bot, EnvData& data) const
{
    float reward = PackReward(env, bot, data);

    Player* owner = FindOwner(data);
    if (!bot || !owner)
        return reward;

    AgentStats const& step = env.StepStats[0];
    Role const role = _profile.PlayRole;
    float const ownerHealth = float(std::max<uint32>(1, owner->GetMaxHealth()));

    // The owner is ally 0 (a party's other members follow it).
    data.OwnerDamageTaken += step.AllyDamageTakenBy[0];
    data.OwnerHealing += step.AllyHealingBy[0];

    reward -= (role == Role::Dps ? OWNER_DAMAGE_TAKEN_DPS : OWNER_DAMAGE_TAKEN_PROTECTOR)
        * float(step.AllyDamageTakenBy[0]) / ownerHealth;

    if (role == Role::Heal)
        reward += HEALING * float(step.AllyHealingBy[0]) / ownerHealth;

    if (role == Role::Tank)
        reward += TANK_DAMAGE_REFUND * data.LastStepDamageTaken;

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

    data.ThreatOnBot += onBot;
    data.ThreatOnOwner += onOwner;

    if (role == Role::Tank)
        reward += TANK_HOLD * float(onBot) - TANK_LOSE * float(onOwner);
    else
        reward -= PULLED_THREAT * float(onBot);

    if (owner->IsAlive())
    {
        // Fighting on its own: the companion pulled something, or kept fighting after the owner stopped.
        // A party's tank pulls first by design.
        if (bot->IsInCombat() && !owner->IsInCombat() && !(HasParty() && role == Role::Tank))
            reward -= SOLO_FIGHT;

        // Out of combat, stay with the owner.
        if (!bot->IsInCombat() && !owner->IsInCombat())
        {
            float const distance = bot->GetDistance(owner);
            if (distance > 25.0f)
                reward -= FOLLOW_FAR;
            else if (distance < 12.0f)
                reward += FOLLOW_NEAR;
        }
    }
    else if (!data.OwnerDied)
    {
        data.OwnerDied = true;
        reward -= OWNER_DEATH;
    }

    return reward;
}

void AnimusForge::ClassRoleScenario::CompanionEpisodeInfo(Env const& env, float* info) const
{
    EnvData const& data = _data[env.Index];

    info[COMPANION_INFO_OWNER_CLASS] = float(data.OwnerClass);
    info[COMPANION_INFO_OWNER_DIED] = data.OwnerDied ? 1.0f : 0.0f;
    info[COMPANION_INFO_OWNER_DAMAGE_TAKEN] = float(data.OwnerDamageTaken);
    info[COMPANION_INFO_OWNER_HEALING] = float(data.OwnerHealing);
    info[COMPANION_INFO_THREAT_ON_BOT] = float(data.ThreatOnBot);
    info[COMPANION_INFO_THREAT_ON_OWNER] = float(data.ThreatOnOwner);
}
