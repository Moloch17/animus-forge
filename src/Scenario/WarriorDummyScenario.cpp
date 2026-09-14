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
#include "AnimusConfig.h"
#include "BotFactory.h"
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
#include "SummonLevel.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>

namespace
{
    enum WarriorDummySpells : uint32
    {
        SPELL_HEROIC_STRIKE_RANK_1  = 78,
        SPELL_BATTLE_STANCE         = 2457,
    };

    enum WarriorDummyCreatures : uint32
    {
        // Grandmaster's Training Dummy: rooted, attackable, npc_training_dummy zeroes all damage.
        NPC_TRAINING_DUMMY          = 31144,
    };

    /// Distance from the bot to the dummy, well inside melee range for both models.
    constexpr float DUMMY_DISTANCE = 2.0f;

    /// Creatures within this radius of the bot that the scenario did not spawn are removed.
    constexpr float ARENA_CLEAR_RADIUS = 60.0f;

    /// The template's HealthModifier is sized for level 80; at level 1 it rounds to 0 HP.
    /// The dummy cannot take damage, so any positive value works.
    constexpr uint32 DUMMY_HEALTH = 1000000;

    /// Bot accounts live far above anything a real realm allocates.
    constexpr uint32 BOT_ACCOUNT_BASE = 0x7F000000;
}

Animus::WarriorDummyScenario::WarriorDummyScenario(ForgeConfig const& config)
    : _arenaMapId(config.ArenaMapId), _arenaPosition(config.ArenaPosition), _hsRageThreshold(config.HsRageThreshold),
    _agentAccountBase(BOT_ACCOUNT_BASE)
{
    _data.resize(config.Envs);
}

Animus::ScenarioSpec Animus::WarriorDummyScenario::Spec() const
{
    ScenarioSpec spec;
    spec.AgentsPerEnv = 1;
    spec.ObsDim = OBS_COUNT;
    spec.StateDim = OBS_COUNT;          // one agent: the critic sees exactly what the actor sees
    spec.NumActions = ACTION_COUNT;
    spec.EpisodeInfoDim = INFO_COUNT;
    return spec;
}

bool Animus::WarriorDummyScenario::Setup(Env& env)
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

    ClearArena(bot);

    if (!SpawnDummy(env, bot, map))
        return false;

    EnvData& data = _data[env.Index];
    data.Home = _arenaPosition;
    data.DamageScale = std::max(1.0f, bot->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE));

    return true;
}

void Animus::WarriorDummyScenario::ClearArena(Player* bot) const
{
    // alive = false: every creature in range, dead or alive.
    std::list<Creature*> creatures;
    bot->GetDeadCreatureListInGrid(creatures, ARENA_CLEAR_RADIUS, false);

    for (Creature* creature : creatures)
        creature->DespawnOrUnsummon(0ms, Seconds(WEEK));

    if (!creatures.empty())
        LOG_WARN("module.animus", "Removed {} creatures from the arena around {}", creatures.size(), bot->GetName());
}

bool Animus::WarriorDummyScenario::SpawnDummy(Env& env, Player* bot, Map* map)
{
    float const facing = bot->GetOrientation();

    Position pos;
    pos.m_positionX = bot->GetPositionX() + DUMMY_DISTANCE * std::cos(facing);
    pos.m_positionY = bot->GetPositionY() + DUMMY_DISTANCE * std::sin(facing);
    pos.m_positionZ = bot->GetPositionZ();

    float const ground = map->GetHeight(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 2.0f);
    if (ground > INVALID_HEIGHT)
        pos.m_positionZ = ground;

    pos.SetOrientation(Position::NormalizeOrientation(facing + float(M_PI)));

    // Summon the dummy at the bot's level so hit, dodge, glancing and armor tables are those of an
    // even-level fight rather than a level 80 target.
    PendingSummonLevel = bot->GetLevel();
    TempSummon* dummy = map->SummonCreature(NPC_TRAINING_DUMMY, pos);
    PendingSummonLevel = 0;

    if (!dummy)
    {
        LOG_ERROR("module.animus", "Could not summon training dummy {} for env {}", uint32(NPC_TRAINING_DUMMY),
            env.Index);
        return false;
    }

    dummy->SetCreateHealth(DUMMY_HEALTH);
    dummy->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(DUMMY_HEALTH));
    dummy->UpdateMaxHealth();
    dummy->SetFullHealth();
    dummy->SetRegeneratingHealth(false);

    env.Targets = { dummy->GetGUID() };

    // Face the dummy exactly: the player melee check needs the target inside the frontal arc.
    bot->SetOrientation(bot->GetAngle(dummy));

    return true;
}

void Animus::WarriorDummyScenario::Reset(Env& env)
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

void Animus::WarriorDummyScenario::ApplyActions(Env& env, int32 const* actions)
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

void Animus::WarriorDummyScenario::Observe(Env& env, float* obs, float* state, uint8* mask)
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

void Animus::WarriorDummyScenario::Reward(Env& env, float* reward)
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

void Animus::WarriorDummyScenario::EpisodeInfo(Env const& env, float* info) const
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

std::vector<std::string> Animus::WarriorDummyScenario::EpisodeInfoNames() const
{
    return { "damage", "dps", "white_hits", "special_hits", "white_damage", "special_damage" };
}

bool Animus::WarriorDummyScenario::ScriptedAction(std::string const& policy, float const* obs, uint8 const* mask,
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

void Animus::WarriorDummyScenario::Teardown(Env& env)
{
    if (Creature* dummy = env.FindTarget(0))
        dummy->DespawnOrUnsummon();

    if (Player* bot = env.FindBot(0))
        BotFactory::Destroy(bot);

    env.Bots.clear();
    env.Targets.clear();
}

bool Animus::WarriorDummyScenario::IsHeroicStrikeQueued(Player const* bot)
{
    Spell const* spell = bot->GetCurrentSpell(CURRENT_MELEE_SPELL);
    return spell && spell->m_spellInfo->Id == SPELL_HEROIC_STRIKE_RANK_1;
}

uint32 Animus::WarriorDummyScenario::HeroicStrikeCost(Player* bot)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_HEROIC_STRIKE_RANK_1);
    return info ? uint32(std::max(0, info->CalcPowerCost(bot, info->GetSchoolMask()))) : 0;
}

bool Animus::WarriorDummyScenario::CanQueueHeroicStrike(Player* bot)
{
    return bot->HasActiveSpell(SPELL_HEROIC_STRIKE_RANK_1) && !bot->GetCurrentSpell(CURRENT_MELEE_SPELL)
        && bot->GetPower(POWER_RAGE) >= HeroicStrikeCost(bot);
}
