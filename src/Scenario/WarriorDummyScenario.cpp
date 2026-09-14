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

#include "WarriorDummyScenario.h"
#include "ForgeConfig.h"
#include "ForgeBotFactory.h"
#include "Creature.h"
#include "Env.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TrainingDummyArena.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    enum WarriorDummySpells : uint32
    {
        SPELL_HEROIC_STRIKE_RANK_1  = 78,
        SPELL_BATTLE_STANCE         = 2457,
    };
}

AnimusForge::WarriorDummyScenario::WarriorDummyScenario(ForgeConfig const& config)
    : _arenaMapId(config.ArenaMapId), _arenaPosition(config.ArenaPosition), _hsRageThreshold(config.HsRageThreshold),
    _agentAccountBase(TrainingDummyArena::BOT_ACCOUNT_BASE)
{
    _data.resize(config.Envs);
}

AnimusForge::ScenarioSpec AnimusForge::WarriorDummyScenario::Spec() const
{
    ScenarioSpec spec;
    spec.AgentsPerEnv = 1;
    spec.ObsDim = OBS_COUNT;
    spec.StateDim = OBS_COUNT;          // one agent: the critic sees exactly what the actor sees
    spec.NumActions = ACTION_COUNT;
    spec.EpisodeInfoDim = INFO_COUNT;
    return spec;
}

bool AnimusForge::WarriorDummyScenario::Setup(Env& env)
{
    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Forgewarrior{}", env.Index);
    spec.Race = RACE_HUMAN;
    spec.Class = CLASS_WARRIOR;
    spec.Gender = GENDER_MALE;
    spec.Level = 1;
    spec.AccountId = _agentAccountBase + env.Index;

    Player* bot = BotFactory::Create(spec);
    if (!bot)
        return false;

    Map* map = BotFactory::PlaceInNewInstance(bot, _arenaMapId, _arenaPosition);
    if (!map)
        return false;

    env.MapId = map->GetId();
    env.InstanceId = map->GetInstanceId();
    env.Bots = { bot->GetGUID() };

    // A first login casts the class's start spells (playercreateinfo_cast_spell); Create does not.
    bot->CastSpell(bot, SPELL_BATTLE_STANCE, true);

    TrainingDummyArena::ClearArena(bot);

    Creature* dummy = TrainingDummyArena::SpawnDummy(bot, map);
    if (!dummy)
        return false;

    env.Targets = { dummy->GetGUID() };

    EnvData& data = _data[env.Index];
    data.Home = _arenaPosition;
    data.DamageScale = std::max(1.0f, bot->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE));

    return true;
}

void AnimusForge::WarriorDummyScenario::Reset(Env& env)
{
    EnvData& data = _data[env.Index];
    data.LastRage = 0;
    data.LastStepDamage = 0.0f;
    data.LastStepRageDelta = 0.0f;

    Player* bot = env.FindBot(0);
    Creature* dummy = env.FindTarget(0);
    if (!bot || !dummy)
    {
        LOG_ERROR("module.animus", "Env {} lost its bot or dummy", env.Index);
        return;
    }

    if (!bot->IsAlive())
        bot->ResurrectPlayer(1.0f);

    bot->InterruptSpell(CURRENT_MELEE_SPELL);
    bot->AttackStop();
    bot->CombatStop(true);
    dummy->CombatStop(true);

    bot->RemoveAllSpellCooldown();
    bot->SetFullHealth();
    bot->SetPower(POWER_RAGE, 0);
    dummy->SetFullHealth();

    if (bot->GetExactDist(&data.Home) > 0.1f)
        bot->UpdatePosition(data.Home, true);

    bot->SetOrientation(bot->GetAngle(dummy));
    bot->Attack(dummy, true);

    // Random swing phase so episodes do not all start on the same swing boundary.
    bot->setAttackTimer(BASE_ATTACK, int32(urand(0, bot->GetAttackTime(BASE_ATTACK))));
}

void AnimusForge::WarriorDummyScenario::ApplyActions(Env& env, int32 const* actions)
{
    Player* bot = env.FindBot(0);
    Creature* dummy = env.FindTarget(0);
    if (!bot || !dummy)
        return;

    switch (actions[0])
    {
        case ACTION_QUEUE_HEROIC_STRIKE:
        {
            if (!CanQueueHeroicStrike(bot))
                break;

            // Same path as CMSG_CAST_SPELL: an untriggered cast that parks in CURRENT_MELEE_SPELL
            // and replaces the next main-hand swing. The spell owns and frees itself.
            SpellCastTargets targets;
            targets.SetUnitTarget(dummy);

            Spell* spell = new Spell(bot, sSpellMgr->GetSpellInfo(SPELL_HEROIC_STRIKE_RANK_1), TRIGGERED_NONE);
            spell->prepare(&targets);
            break;
        }
        case ACTION_CANCEL_QUEUED:
            if (IsHeroicStrikeQueued(bot))
                bot->InterruptSpell(CURRENT_MELEE_SPELL);
            break;
        default:
            break;
    }
}

void AnimusForge::WarriorDummyScenario::Observe(Env& env, float* obs, float* state, uint8* mask)
{
    std::fill(obs, obs + OBS_COUNT, 0.0f);
    std::fill(mask, mask + ACTION_COUNT, 0);
    mask[ACTION_NOOP] = 1;

    EnvData const& data = _data[env.Index];
    Player* bot = env.FindBot(0);
    Creature* dummy = env.FindTarget(0);

    if (bot && dummy)
    {
        float const maxRage = float(std::max<uint32>(1, bot->GetMaxPower(POWER_RAGE)));
        float const attackTime = float(std::max<uint32>(1, bot->GetAttackTime(BASE_ATTACK)));
        bool const queued = IsHeroicStrikeQueued(bot);

        obs[OBS_RAGE] = float(bot->GetPower(POWER_RAGE)) / maxRage;
        obs[OBS_SWING_REMAINING] = float(std::max(0, bot->getAttackTimer(BASE_ATTACK))) / attackTime;
        obs[OBS_WEAPON_SPEED] = attackTime / 4000.0f;
        obs[OBS_HEROIC_STRIKE_QUEUED] = queued ? 1.0f : 0.0f;
        obs[OBS_AUTO_ATTACKING] = bot->GetVictim() == dummy ? 1.0f : 0.0f;
        obs[OBS_IN_MELEE_FRONT] = bot->IsWithinMeleeRange(dummy) && bot->HasInArc(2 * float(M_PI) / 3, dummy)
            ? 1.0f : 0.0f;
        obs[OBS_HEROIC_STRIKE_COST] = float(HeroicStrikeCost(bot)) / maxRage;
        obs[OBS_LAST_STEP_DAMAGE] = data.LastStepDamage;
        obs[OBS_LAST_STEP_RAGE_DELTA] = data.LastStepRageDelta;

        mask[ACTION_QUEUE_HEROIC_STRIKE] = CanQueueHeroicStrike(bot) ? 1 : 0;
        mask[ACTION_CANCEL_QUEUED] = queued ? 1 : 0;
    }

    std::memcpy(state, obs, OBS_COUNT * sizeof(float));
}

void AnimusForge::WarriorDummyScenario::Reward(Env& env, float* reward)
{
    EnvData& data = _data[env.Index];

    float const scaled = float(env.StepStats[0].Damage) / data.DamageScale;
    reward[0] = scaled;
    data.LastStepDamage = scaled;

    if (Player* bot = env.FindBot(0))
    {
        uint32 const rage = bot->GetPower(POWER_RAGE);
        float const maxRage = float(std::max<uint32>(1, bot->GetMaxPower(POWER_RAGE)));
        data.LastStepRageDelta = (float(rage) - float(data.LastRage)) / maxRage;
        data.LastRage = rage;
    }
}

void AnimusForge::WarriorDummyScenario::EpisodeInfo(Env const& env, float* info) const
{
    AgentStats const& stats = env.EpisodeStats[0];
    float const seconds = std::max(0.001f, float(env.EpisodeElapsedMs) / 1000.0f);

    info[INFO_DAMAGE] = float(stats.Damage);
    info[INFO_DPS] = float(stats.Damage) / seconds;
    info[INFO_WHITE_HITS] = float(stats.WhiteHits);
    info[INFO_SPECIAL_HITS] = float(stats.SpecialHits);
    info[INFO_WHITE_DAMAGE] = float(stats.WhiteDamage);
    info[INFO_SPECIAL_DAMAGE] = float(stats.SpecialDamage);
}

std::vector<std::string> AnimusForge::WarriorDummyScenario::EpisodeInfoNames() const
{
    return { "damage", "dps", "white_hits", "special_hits", "white_damage", "special_damage" };
}

bool AnimusForge::WarriorDummyScenario::ScriptedAction(std::string const& policy, float const* obs, uint8 const* mask,
    int32& action) const
{
    if (policy == "never_hs")
    {
        action = mask[ACTION_CANCEL_QUEUED] ? ACTION_CANCEL_QUEUED : ACTION_NOOP;
        return true;
    }

    if (policy == "hs_at_threshold")
    {
        // OBS_RAGE is a fraction of 100 rage (max power is stored as 1000 tenths of rage).
        float const rage = obs[OBS_RAGE] * 100.0f;
        action = mask[ACTION_QUEUE_HEROIC_STRIKE] && rage >= float(_hsRageThreshold)
            ? ACTION_QUEUE_HEROIC_STRIKE : ACTION_NOOP;
        return true;
    }

    return false;
}

void AnimusForge::WarriorDummyScenario::Teardown(Env& env)
{
    if (Creature* dummy = env.FindTarget(0))
        dummy->DespawnOrUnsummon();

    if (Player* bot = env.FindBot(0))
        BotFactory::Destroy(bot);

    env.Bots.clear();
    env.Targets.clear();
}

bool AnimusForge::WarriorDummyScenario::IsHeroicStrikeQueued(Player const* bot)
{
    Spell const* spell = bot->GetCurrentSpell(CURRENT_MELEE_SPELL);
    return spell && spell->m_spellInfo->Id == SPELL_HEROIC_STRIKE_RANK_1;
}

uint32 AnimusForge::WarriorDummyScenario::HeroicStrikeCost(Player* bot)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_HEROIC_STRIKE_RANK_1);
    return info ? uint32(std::max(0, info->CalcPowerCost(bot, info->GetSchoolMask()))) : 0;
}

bool AnimusForge::WarriorDummyScenario::CanQueueHeroicStrike(Player* bot)
{
    return bot->HasActiveSpell(SPELL_HEROIC_STRIKE_RANK_1) && !bot->GetCurrentSpell(CURRENT_MELEE_SPELL)
        && bot->GetPower(POWER_RAGE) >= HeroicStrikeCost(bot);
}
