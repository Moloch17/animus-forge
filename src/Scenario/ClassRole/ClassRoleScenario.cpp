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

#include "ClassRoleScenario.h"
#include "Creature.h"
#include "DuelArena.h"
#include "Env.h"
#include "ForgeBotFactory.h"
#include "ForgeConfig.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TrainingDummyArena.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    using AnimusForge::ActionCatalog;

    constexpr std::array<uint8, 10> PLAYABLE_RACES =
    {
        RACE_HUMAN, RACE_ORC, RACE_DWARF, RACE_NIGHTELF, RACE_UNDEAD_PLAYER, RACE_TAUREN, RACE_GNOME, RACE_TROLL,
        RACE_BLOODELF, RACE_DRAENEI
    };

    constexpr std::array<ShapeshiftForm, 13> TRACKED_FORMS =
    {
        FORM_NONE, FORM_CAT, FORM_TREE, FORM_BEAR, FORM_DIREBEAR, FORM_MOONKIN, FORM_SHADOW, FORM_STEALTH,
        FORM_BATTLESTANCE, FORM_DEFENSIVESTANCE, FORM_BERSERKERSTANCE, FORM_METAMORPHOSIS, FORM_GHOSTWOLF
    };

    enum ClassRoleSpells : uint32
    {
        SPELL_BATTLE_STANCE     = 2457,
        SPELL_DEFENSIVE_STANCE  = 71,
    };

    constexpr float MELEE_DISTANCE = 2.0f;
    constexpr float RANGED_DISTANCE = 20.0f;
    constexpr float GCD_MS = 1500.0f;
    constexpr float RUNE_COOLDOWN_MS = 10000.0f;
    constexpr float TALENT_POINTS_AT_MAX_LEVEL = 71.0f;

    /// Per-decision damage scale: roughly how a well-geared character's damage grows with level, so the
    /// reward has a similar size at every level (about 16 at level 1, 230 at 40, 3500 at 80).
    float DamageScaleForLevel(uint8 level)
    {
        return 15.0f * std::exp(0.068f * float(level));
    }

    float CooldownFraction(Player const* bot, SpellInfo const* info)
    {
        uint32 const full = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
        return full ? std::min(1.0f, float(bot->GetSpellCooldownDelay(info->Id)) / float(full)) : 0.0f;
    }

    float AuraFraction(Unit const* unit, uint32 spellId, ObjectGuid caster, float& stacks)
    {
        Aura const* aura = unit->GetAura(spellId, caster);
        if (!aura)
            return 0.0f;

        stacks = std::max(stacks, std::min(1.0f, float(std::max(aura->GetStackAmount(), aura->GetCharges())) / 5.0f));
        if (aura->GetMaxDuration() <= 0)
            return 1.0f;    // permanent (stances, forms, presences, auras)

        return std::clamp(float(aura->GetDuration()) / float(aura->GetMaxDuration()), 0.0f, 1.0f);
    }

    SpellCastTargets TargetsFor(SpellInfo const* info, Player* bot, Unit* dummy)
    {
        SpellCastTargets targets;

        // No target (between gauntlet pulls): only self-cast spells can succeed.
        if (dummy && (info->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION))
            targets.SetDst(*dummy);

        if (info->NeedsExplicitUnitTarget() && dummy && !info->IsPositive())
            targets.SetUnitTarget(dummy);
        else
            targets.SetUnitTarget(bot);

        return targets;
    }

    /// The core's own cast validation, without casting: cooldown, GCD, power, stance, range, facing,
    /// reagents, reactive requirements. Same pattern as PetAI.
    bool CanCast(Player* bot, SpellInfo const* info, Unit* dummy, Item* castItem = nullptr)
    {
        Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
        spell->m_CastItem = castItem;
        spell->LoadScripts();

        SpellCastTargets targets = TargetsFor(info, bot, dummy);
        spell->InitExplicitTargets(targets);

        SpellCastResult const result = spell->CheckCast(true);
        delete spell;

        return result == SPELL_CAST_OK;
    }
}

std::string AnimusForge::ClassRoleScenario::ScenarioName(ClassRoleProfile const& profile, ArenaMode mode)
{
    switch (mode)
    {
        case ArenaMode::Duel:     return profile.ScenarioName + "_duel";
        case ArenaMode::Pack:     return profile.ScenarioName + "_pack";
        case ArenaMode::Gauntlet: return profile.ScenarioName + "_gauntlet";
        case ArenaMode::Dummy:    break;
    }

    return profile.ScenarioName;
}

AnimusForge::ClassRoleScenario::ClassRoleScenario(ClassRoleProfile const& profile, ForgeConfig const& config,
    ArenaMode mode)
    : _mode(mode), _name(ScenarioName(profile, mode)), _profile(profile), _arenaMapId(config.ArenaMapId),
    _arenaPosition(config.ArenaPosition)
{
    for (uint8 race : PLAYABLE_RACES)
        if (sObjectMgr->GetPlayerInfo(race, profile.Class))
            _races.push_back(race);

    _kit = std::make_unique<ClassKit>(profile.Class);
    _talents = std::make_unique<TalentBuilder>(profile.Class);
    _catalog = std::make_unique<ActionCatalog>(profile.Class, *_kit, *_talents);
    _gear = std::make_unique<GearBuilder>(profile, *_kit);

    uint32 const actions = uint32(_catalog->Actions().size());
    uint32 const talents = uint32(_talents->Talents().size());

    _actionObsFirst = OBS_GLOBAL_COUNT;
    _talentObsFirst = _actionObsFirst + actions * ACTION_FEATURES;
    _treeObsFirst = _talentObsFirst + talents;

    _spec.AgentsPerEnv = 1;
    _spec.ObsDim = _treeObsFirst + TalentBuilder::TREE_COUNT;
    _spec.StateDim = _spec.ObsDim;      // one agent: the critic sees exactly what the actor sees
    _spec.NumActions = actions;
    _spec.EpisodeInfoDim = INFO_COUNT;

    // Every stage keeps the previous stage's layout unchanged and appends its own (see ArenaMode).
    if (HasDuel())
    {
        uint32 const stable = _profile.Class == CLASS_HUNTER ? STABLE_SLOTS : 0;

        _duelObsFirst = _spec.ObsDim;
        _spec.ObsDim += DUEL_OBS_COUNT_WITHOUT_STABLE + stable * STABLE_FEATURES;
        _spec.StateDim = _spec.ObsDim;

        _duelActionFirst = _spec.NumActions;
        _duelActionCount = DUEL_ACTION_COUNT_WITHOUT_STABLE + stable;
        _spec.NumActions += _duelActionCount;

        _duelInfoFirst = _spec.EpisodeInfoDim;
        _spec.EpisodeInfoDim += DUEL_INFO_COUNT;

        DuelArena::OpponentPool::Instance();    // load it at startup rather than on the first episode
    }

    if (HasPack())
    {
        uint32 const tactical = uint32(_catalog->Tactical().size());

        _packObsFirst = _spec.ObsDim;
        _spec.ObsDim += PACK_OBS_GLOBAL_COUNT + PACK_SLOTS * SLOT_FEATURES + tactical * 2;
        _spec.StateDim = _spec.ObsDim;

        _packActionFirst = _spec.NumActions;
        _packActionCount = PACK_SLOTS + tactical;
        _spec.NumActions += _packActionCount;

        _packInfoFirst = _spec.EpisodeInfoDim;
        _spec.EpisodeInfoDim += PACK_INFO_COUNT;
    }

    if (HasGauntlet())
    {
        uint32 const sustain = uint32(_catalog->Sustain().size());

        _gauntletObsFirst = _spec.ObsDim;
        _spec.ObsDim += GAUNTLET_OBS_GLOBAL_COUNT + sustain * 2;
        _spec.StateDim = _spec.ObsDim;

        _gauntletActionFirst = _spec.NumActions;
        _gauntletActionCount = GAUNTLET_ACTION_SUSTAIN_FIRST + sustain;
        _spec.NumActions += _gauntletActionCount;

        _gauntletInfoFirst = _spec.EpisodeInfoDim;
        _spec.EpisodeInfoDim += GAUNTLET_INFO_COUNT;

        DuelArena::ConsumablePool::Instance();
    }

    _data.resize(config.Envs);

    LOG_INFO("module.animus", "{}: {} races, {} specs, {} actions, {} talents, obs {}", Name(), _races.size(),
        profile.Specs.size(), _spec.NumActions, talents, _spec.ObsDim);
}

bool AnimusForge::ClassRoleScenario::IsTerminal(Env const& env) const
{
    EnvData const& data = _data[env.Index];
    switch (_mode)
    {
        case ArenaMode::Duel:
        case ArenaMode::Pack:
            return data.Killed || data.Died;    // pack: Killed = cleared
        case ArenaMode::Gauntlet:
            return data.Died;
        case ArenaMode::Dummy:
            break;
    }

    return false;
}

AnimusForge::ClassRoleScenario::~ClassRoleScenario()
{
    // Sessions kept for the next rebuild. Teardown already destroyed the active one with its bot.
    for (EnvData& data : _data)
        for (WorldSession*& session : data.Sessions)
            delete session;
}

bool AnimusForge::ClassRoleScenario::Setup(Env& env)
{
    if (_races.empty())
    {
        LOG_ERROR("module.animus", "{}: no race can be class {}", Name(), _profile.Class);
        return false;
    }

    if (!Rebuild(env))
        return false;

    _data[env.Index].Fresh = true;
    return true;
}

void AnimusForge::ClassRoleScenario::Reset(Env& env)
{
    EnvData& data = _data[env.Index];
    data.LastStepDamage = 0.0f;
    data.LastStepPowerDelta = 0.0f;
    data.SpellCasts = 0;
    data.TrinketUses = 0;

    data.LastDistance = -1.0f;
    data.KillTimeMs = 0;
    data.DamageTaken = 0;
    data.LastStepDamageTaken = 0.0f;
    data.StealthOpeners = 0;
    data.StepStealthOpener = false;
    data.PetSummoned = false;
    data.Killed = false;
    data.Died = false;

    data.TargetSlot = 0;
    data.PackLinked = false;
    data.PackSize = 0;
    data.PullKills = 0;
    data.Kills = 0;
    data.Interrupts = 0;
    data.PendingInterrupt.Clear();
    data.PullStartMs = 0;
    data.PullDamageTaken = 0;
    data.PullsCleared = 0;
    data.NextPullMs = 0;
    data.EliteOrHigherPull = false;
    data.FoodUsed = 0;
    data.DrinkUsed = 0;
    data.SustainCasts = 0;

    // Setup already built the first episode's character.
    if (data.Fresh)
    {
        data.Fresh = false;
        return;
    }

    if (!Rebuild(env))
        LOG_ERROR("module.animus", "{}: env {} could not build a new character; it keeps the old one", Name(),
            env.Index);
}

bool AnimusForge::ClassRoleScenario::Rebuild(Env& env)
{
    EnvData& data = _data[env.Index];

    // Through the session rather than ObjectAccessor::FindPlayer: a bot that is out of the world (a far
    // teleport in progress) is still the env's bot and still has to be destroyed.
    WorldSession* activeSession = data.Sessions[data.ActiveSession];
    Player* oldBot = activeSession ? activeSession->GetPlayer() : nullptr;

    std::vector<Creature*> oldTargets;
    for (uint32 target = 0; target < env.Targets.size(); ++target)
        if (Creature* creature = env.FindTarget(target))
            oldTargets.push_back(creature);

    data.Race = _races[urand(0, uint32(_races.size()) - 1)];
    data.Level = uint8(urand(_kit->MinLevel(), DEFAULT_MAX_LEVEL));
    data.Spec = uint8(urand(0, uint32(_profile.Specs.size()) - 1));
    data.StartHealth = frand(0.2f, 1.0f);
    data.EndHealth = frand(0.0f, data.StartHealth);
    data.DamageScale = DamageScaleForLevel(data.Level);

    // The new bot goes on the idle session and into the map before the old one leaves, so the
    // instance always has a bound player.
    uint8 const session = oldBot ? uint8(1 - data.ActiveSession) : data.ActiveSession;

    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Forge{}{}", env.Index, session ? "b" : "a");
    spec.Race = data.Race;
    spec.Class = _profile.Class;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = data.Level;
    spec.AccountId = TrainingDummyArena::BOT_ACCOUNT_BASE + env.Index * 2 + session;
    spec.GuidLow = data.Guids[session];

    Player* bot = BotFactory::Create(spec, data.Sessions[session]);
    if (!bot)
        return false;

    data.Sessions[session] = bot->GetSession();
    data.Guids[session] = bot->GetGUID().GetCounter();

    Map* map = oldBot ? env.FindMap() : nullptr;
    if (map ? !BotFactory::PlaceInMap(bot, map, _arenaPosition)
        : !(map = BotFactory::PlaceInNewInstance(bot, _arenaMapId, _arenaPosition)))
    {
        data.Sessions[session] = nullptr;
        BotFactory::DestroyUnplaced(bot);
        return false;
    }

    // Talent points depend on the map for death knights (Ebon Hold, where Create put the bot, only
    // counts quest-rewarded points); recompute them on the arena map.
    bot->InitTalentForLevel();
    Configure(bot, data);

    for (Creature* creature : oldTargets)
        creature->DespawnOrUnsummon();

    if (oldBot)
        data.Sessions[data.ActiveSession] = BotFactory::Destroy(oldBot, true);
    else
        TrainingDummyArena::ClearArena(bot);

    data.ActiveSession = session;

    env.MapId = map->GetId();
    env.InstanceId = map->GetInstanceId();
    env.Bots = { bot->GetGUID() };
    env.Targets.clear();

    if (HasPack())
        return StartPack(env, bot, map, data);

    if (_mode == ArenaMode::Duel)
    {
        data.OpponentEntry = DuelArena::OpponentPool::Instance().Random(data.Level);
        Creature* opponent = data.OpponentEntry ? DuelArena::SpawnOpponent(bot, map, data.OpponentEntry) : nullptr;
        if (!opponent)
            return false;

        env.Targets = { opponent->GetGUID() };
        StartDuel(bot, opponent, data);
        return true;
    }

    SpecProfile const& specProfile = _profile.Specs[data.Spec];
    float const distance = specProfile.Range == RangeBand::Melee ? MELEE_DISTANCE : RANGED_DISTANCE;

    Creature* dummy = TrainingDummyArena::SpawnDummy(bot, map, distance);
    if (!dummy)
        return false;

    env.Targets = { dummy->GetGUID() };
    StartFight(bot, dummy, data);
    return true;
}

void AnimusForge::ClassRoleScenario::Configure(Player* bot, EnvData& data) const
{
    SpecProfile const& spec = _profile.Specs[data.Spec];

    GearBuilder::LearnProficiencies(bot);

    data.Build = _talents->Random(spec.TabPage, bot->GetFreeTalentPoints());
    data.UnspentTalentPoints = _talents->Apply(bot, data.Build);

    _kit->Learn(bot);
    _gear->Equip(bot, spec);

    data.EquippedItems = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++data.EquippedItems;

    bot->UpdateAllStats();
    bot->SetFullHealth();
    bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
    bot->SetPower(POWER_ENERGY, bot->GetMaxPower(POWER_ENERGY));
    bot->SetPower(POWER_RAGE, 0);
    bot->SetPower(POWER_RUNIC_POWER, 0);
}

void AnimusForge::ClassRoleScenario::StartFight(Player* bot, Creature* dummy, EnvData const& data) const
{
    SpecProfile const& spec = _profile.Specs[data.Spec];

    bot->SetOrientation(bot->GetAngle(dummy));

    // A warrior has no stance until one is cast (a first login casts it), and nothing works without one.
    if (_profile.Class == CLASS_WARRIOR)
    {
        uint32 const stance = _profile.PlayRole == Role::Tank && bot->HasSpell(SPELL_DEFENSIVE_STANCE)
            ? SPELL_DEFENSIVE_STANCE : SPELL_BATTLE_STANCE;
        bot->CastSpell(bot, stance, true);
    }

    bool const melee = spec.Range == RangeBand::Melee;
    bot->Attack(dummy, melee);

    // Random swing phase so episodes do not all start on the same swing boundary.
    if (melee)
        bot->setAttackTimer(BASE_ATTACK, int32(urand(0, bot->GetAttackTime(BASE_ATTACK))));

    dummy->SetHealth(std::max<uint32>(1, uint32(float(dummy->GetMaxHealth()) * data.StartHealth)));
}

SpellInfo const* AnimusForge::ClassRoleScenario::ResolveSpell(Player const* bot, uint32 action) const
{
    return ActionCatalog::KnownRank(bot, _catalog->Actions()[action].FirstRank);
}

SpellInfo const* AnimusForge::ClassRoleScenario::TrinketSpell(Item const* item)
{
    if (!item)
        return nullptr;

    for (_Spell const& spellData : item->GetTemplate()->Spells)
        if (spellData.SpellId > 0 && spellData.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            return sSpellMgr->GetSpellInfo(spellData.SpellId);

    return nullptr;
}

bool AnimusForge::ClassRoleScenario::IsActionAllowed(Player* bot, Creature* dummy, uint32 action) const
{
    ActionCatalog::Action const& def = _catalog->Actions()[action];

    switch (def.Type)
    {
        case ActionCatalog::Kind::Noop:
            return true;
        case ActionCatalog::Kind::CancelQueued:
            return bot->GetCurrentSpell(CURRENT_MELEE_SPELL) != nullptr;
        case ActionCatalog::Kind::Trinket:
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, def.EquipmentSlot);
            SpellInfo const* info = TrinketSpell(item);
            return info && !bot->HasSpellCooldown(info->Id) && CanCast(bot, info, dummy, item);
        }
        case ActionCatalog::Kind::Spell:
            break;
    }

    return IsSpellActionAllowed(bot, dummy, def);
}

bool AnimusForge::ClassRoleScenario::IsSpellActionAllowed(Player* bot, Unit* target,
    ActionCatalog::Action const& def) const
{
    // Cheap rejections before the full cast check.
    SpellInfo const* info = ActionCatalog::KnownRank(bot, def.FirstRank);
    if (!info || !bot->HasActiveSpell(info->Id) || bot->HasSpellCooldown(info->Id))
        return false;

    if (def.NextSwing && bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
        return false;

    if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
        return false;

    // Server-driven movement does not set the movement flags CheckCast looks at: no cast-time or
    // channeled spell while running.
    if (HasDuel() && !bot->movespline->Finalized() && (info->CalcCastTime(bot) || info->IsChanneled()))
        return false;

    return CanCast(bot, info, target);
}

bool AnimusForge::ClassRoleScenario::ApplySpellAction(Player* bot, Unit* target, ActionCatalog::Action const& def,
    EnvData& data) const
{
    if (def.NextSwing && bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
        return false;

    SpellInfo const* info = ActionCatalog::KnownRank(bot, def.FirstRank);
    if (!info || !bot->HasActiveSpell(info->Id))
        return false;

    // Same path as CMSG_CAST_SPELL. prepare() runs the full cast validation again, so a masked action
    // from a misbehaving client simply fails. The spell owns and frees itself.
    SpellCastTargets targets = TargetsFor(info, bot, target);
    bool const stealthed = bot->HasAuraType(SPELL_AURA_MOD_STEALTH);
    bool const targetCasting = target && target->IsNonMeleeSpellCast(false);
    Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
    if (spell->prepare(&targets) != SPELL_CAST_OK)
        return false;

    ++data.SpellCasts;

    // A stealth opener (Ambush, Garrote, Cheap Shot, Ravage, Pounce, ...) on the opponent.
    if (HasDuel() && stealthed && info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED)
        && info->NeedsExplicitUnitTarget() && !info->IsPositive())
    {
        data.StepStealthOpener = true;
        ++data.StealthOpeners;
    }

    // An interrupt attempt on a casting enemy; the reward checks next decision whether the cast stopped.
    if (HasPack() && targetCasting && target != bot && ActionCatalog::IsInterruptingSpell(info))
        data.PendingInterrupt = target->GetGUID();

    return true;
}

void AnimusForge::ClassRoleScenario::UpdateDummyHealth(Env const& env, Creature* dummy) const
{
    EnvData const& data = _data[env.Index];
    float const progress = env.EpisodeLengthMs
        ? std::min(1.0f, float(env.EpisodeElapsedMs) / float(env.EpisodeLengthMs)) : 0.0f;
    float const fraction = data.StartHealth + (data.EndHealth - data.StartHealth) * progress;

    dummy->SetHealth(std::max<uint32>(1, uint32(float(dummy->GetMaxHealth()) * fraction)));
}

void AnimusForge::ClassRoleScenario::ApplyActions(Env& env, int32 const* actions)
{
    Player* bot = env.FindBot(0);
    if (!bot)
        return;

    int32 const action = actions[0];
    EnvData& data = _data[env.Index];

    // Pack upkeep first (linked pulls, the gauntlet's next pull), so the target below is current.
    if (HasPack())
        UpdatePack(env, bot, data);

    Creature* dummy = CurrentTarget(env);

    // Only the gauntlet has moments without a target (between pulls).
    if (!dummy && !HasGauntlet())
        return;

    if (_mode == ArenaMode::Dummy)
        UpdateDummyHealth(env, dummy);
    else
    {
        // Face the opponent whenever not running somewhere: casts and swings need it, and turning is
        // not a decision worth learning.
        if (dummy && bot->IsAlive() && bot->movespline->Finalized() && !bot->HasInArc(float(M_PI) / 2, dummy))
            bot->SetFacingToObject(dummy);

        if (HasGauntlet() && action >= int32(_gauntletActionFirst))
        {
            ApplyGauntletAction(bot, dummy, uint32(action) - _gauntletActionFirst, data);
            return;
        }

        if (HasPack() && action >= int32(_packActionFirst) && action < int32(_packActionFirst + _packActionCount))
        {
            ApplyPackAction(env, bot, dummy, uint32(action) - _packActionFirst, data);
            return;
        }

        if (action >= int32(_duelActionFirst) && action < int32(_duelActionFirst + _duelActionCount))
        {
            if (dummy)
                ApplyDuelAction(bot, dummy, uint32(action) - _duelActionFirst, data);
            return;
        }
    }

    if (action <= 0 || action >= int32(_catalog->Actions().size()))
        return;

    ActionCatalog::Action const& def = _catalog->Actions()[action];

    switch (def.Type)
    {
        case ActionCatalog::Kind::Noop:
            return;
        case ActionCatalog::Kind::CancelQueued:
            if (bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
                bot->InterruptSpell(CURRENT_MELEE_SPELL);
            return;
        case ActionCatalog::Kind::Trinket:
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, def.EquipmentSlot);
            SpellInfo const* info = TrinketSpell(item);
            if (!info || bot->HasSpellCooldown(info->Id))
                return;

            bot->CastItemUseSpell(item, TargetsFor(info, bot, dummy), 1, 0);
            if (bot->HasSpellCooldown(info->Id))
                ++data.TrinketUses;
            return;
        }
        case ActionCatalog::Kind::Spell:
            break;
    }

    ApplySpellAction(bot, dummy, def, data);
}

void AnimusForge::ClassRoleScenario::Observe(Env& env, float* obs, float* state, uint8* mask)
{
    std::fill(obs, obs + _spec.ObsDim, 0.0f);
    std::fill(mask, mask + _spec.NumActions, 0);
    mask[0] = 1;

    Player* bot = env.FindBot(0);
    Creature* dummy = CurrentTarget(env);      // may be null between gauntlet pulls
    EnvData const& data = _data[env.Index];

    obs[OBS_LEVEL] = float(data.Level) / float(DEFAULT_MAX_LEVEL);
    for (uint32 i = 0; i < PLAYABLE_RACES.size(); ++i)
        obs[OBS_RACE_FIRST + i] = PLAYABLE_RACES[i] == data.Race ? 1.0f : 0.0f;
    obs[OBS_SPEC_FIRST + std::min<uint32>(data.Spec, MAX_SPECS - 1)] = 1.0f;

    if (bot && (dummy || HasGauntlet()))
    {
        ObjectGuid const botGuid = bot->GetGUID();
        uint8 const level = bot->GetLevel();

        obs[OBS_HEALTH] = bot->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = bot->GetMaxPower(POWER_MANA))
            obs[OBS_MANA] = float(bot->GetPower(POWER_MANA)) / float(maxMana);
        obs[OBS_RAGE] = float(bot->GetPower(POWER_RAGE)) / 1000.0f;
        if (uint32 const maxEnergy = bot->GetMaxPower(POWER_ENERGY))
            obs[OBS_ENERGY] = float(bot->GetPower(POWER_ENERGY)) / float(maxEnergy);
        obs[OBS_RUNIC_POWER] = float(bot->GetPower(POWER_RUNIC_POWER)) / 1000.0f;

        if (bot->getClass() == CLASS_DEATH_KNIGHT)
            for (uint8 rune = 0; rune < MAX_RUNES; ++rune)
                obs[OBS_RUNE_FIRST + rune] = 1.0f
                    - std::min(1.0f, float(bot->GetRuneCooldown(rune)) / RUNE_COOLDOWN_MS);

        if (dummy)
            obs[OBS_COMBO_POINTS] = float(bot->GetComboPoints(dummy)) / 5.0f;

        ShapeshiftForm const form = bot->GetShapeshiftForm();
        for (uint32 i = 0; i < TRACKED_FORMS.size(); ++i)
            obs[OBS_FORM_FIRST + i] = TRACKED_FORMS[i] == form ? 1.0f : 0.0f;

        obs[OBS_CASTING] = bot->IsNonMeleeSpellCast(false, false, true) ? 1.0f : 0.0f;
        obs[OBS_QUEUED_NEXT_SWING] = bot->GetCurrentSpell(CURRENT_MELEE_SPELL) ? 1.0f : 0.0f;

        for (auto const& [index, attack] : { std::pair{ OBS_MAIN_HAND_SWING, BASE_ATTACK },
            std::pair{ OBS_OFF_HAND_SWING, OFF_ATTACK }, std::pair{ OBS_RANGED_SWING, RANGED_ATTACK } })
        {
            if (uint32 const attackTime = bot->GetAttackTime(attack))
                obs[index] = std::clamp(float(bot->getAttackTimer(attack)) / float(attackTime), 0.0f, 1.0f);
        }

        obs[OBS_MAIN_HAND_SPEED] = float(bot->GetAttackTime(BASE_ATTACK)) / 4000.0f;
        if (dummy)
        {
            obs[OBS_TARGET_HEALTH] = dummy->GetHealthPct() / 100.0f;
            obs[OBS_TARGET_DISTANCE] = std::min(1.0f, bot->GetDistance(dummy) / 40.0f);
            obs[OBS_IN_MELEE_FRONT] = bot->IsWithinMeleeRange(dummy) && bot->HasInArc(2 * float(M_PI) / 3, dummy)
                ? 1.0f : 0.0f;
        }

        obs[OBS_ATTACK_POWER] = bot->GetTotalAttackPowerValue(BASE_ATTACK) / (100.0f + 50.0f * level);
        obs[OBS_SPELL_POWER] = float(bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC)) / (50.0f + 30.0f * level);
        obs[OBS_MELEE_CRIT] = bot->GetFloatValue(PLAYER_CRIT_PERCENTAGE) / 100.0f;

        float spellCrit = 0.0f;
        for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
            spellCrit = std::max(spellCrit, bot->GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + school));
        obs[OBS_SPELL_CRIT] = spellCrit / 100.0f;

        obs[OBS_MELEE_HASTE] = bot->GetRatingBonusValue(CR_HASTE_MELEE) / 100.0f;
        obs[OBS_SPELL_HASTE] = bot->GetRatingBonusValue(CR_HASTE_SPELL) / 100.0f;
        obs[OBS_MELEE_HIT] = bot->GetRatingBonusValue(CR_HIT_MELEE) / 100.0f;
        obs[OBS_SPELL_HIT] = bot->GetRatingBonusValue(CR_HIT_SPELL) / 100.0f;
        obs[OBS_EXPERTISE] = float(bot->GetUInt32Value(PLAYER_EXPERTISE)) / 30.0f;
        obs[OBS_ARMOR_PENETRATION] = bot->GetRatingBonusValue(CR_ARMOR_PENETRATION) / 100.0f;
        obs[OBS_LAST_STEP_DAMAGE] = data.LastStepDamage;
        obs[OBS_LAST_STEP_POWER_DELTA] = data.LastStepPowerDelta;

        std::vector<ActionCatalog::Action> const& actions = _catalog->Actions();
        for (uint32 action = 0; action < actions.size(); ++action)
        {
            SpellInfo const* info = nullptr;
            if (actions[action].Type == ActionCatalog::Kind::Spell)
                info = ResolveSpell(bot, action);
            else if (actions[action].Type == ActionCatalog::Kind::Trinket)
                info = TrinketSpell(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, actions[action].EquipmentSlot));

            if (info)
            {
                float* features = obs + _actionObsFirst + action * ACTION_FEATURES;
                float stacks = 0.0f;
                features[0] = 1.0f;
                features[1] = CooldownFraction(bot, info);
                features[2] = dummy ? AuraFraction(dummy, info->Id, botGuid, stacks) : 0.0f;
                features[3] = AuraFraction(bot, info->Id, botGuid, stacks);
                features[4] = stacks;

                if (!obs[OBS_GCD] && info->StartRecoveryTime)
                    obs[OBS_GCD] = std::min(1.0f, float(bot->GetGlobalCooldownMgr().GetGlobalCooldown(info)) / GCD_MS);
            }

            if (action > 0)
                mask[action] = IsActionAllowed(bot, dummy, action) ? 1 : 0;
        }

        if (HasDuel() && dummy)
        {
            ObserveDuel(env, bot, dummy, obs);
            for (uint32 duelAction = 0; duelAction < _duelActionCount; ++duelAction)
                mask[_duelActionFirst + duelAction] = IsDuelActionAllowed(bot, dummy, duelAction, data) ? 1 : 0;
        }

        if (HasPack())
        {
            ObservePack(env, bot, obs);
            for (uint32 packAction = 0; packAction < _packActionCount; ++packAction)
            {
                uint32 const tactical = packAction - PACK_SLOTS;
                mask[_packActionFirst + packAction] = packAction < PACK_SLOTS
                    ? IsPackActionAllowed(env, bot, packAction)
                    : dummy && IsSpellActionAllowed(bot, dummy, _catalog->Tactical()[tactical]);
            }
        }

        if (HasGauntlet())
        {
            ObserveGauntlet(env, bot, obs);
            for (uint32 gauntletAction = 0; gauntletAction < _gauntletActionCount; ++gauntletAction)
                mask[_gauntletActionFirst + gauntletAction] =
                    IsGauntletActionAllowed(bot, dummy, gauntletAction, data) ? 1 : 0;
        }
    }

    std::vector<TalentBuilder::Talent> const& talents = _talents->Talents();
    for (uint32 i = 0; i < talents.size() && i < data.Build.Ranks.size(); ++i)
        obs[_talentObsFirst + i] = float(data.Build.Ranks[i]) / float(std::max<uint8>(1, talents[i].MaxRank));

    for (uint32 tree = 0; tree < TalentBuilder::TREE_COUNT; ++tree)
        obs[_treeObsFirst + tree] = float(data.Build.TreePoints[tree]) / TALENT_POINTS_AT_MAX_LEVEL;

    std::memcpy(state, obs, _spec.ObsDim * sizeof(float));
}

void AnimusForge::ClassRoleScenario::Reward(Env& env, float* reward)
{
    EnvData& data = _data[env.Index];

    float const scaled = float(env.StepStats[0].Damage) / data.DamageScale;
    reward[0] = scaled;
    data.LastStepDamage = scaled;

    if (_mode == ArenaMode::Duel)
        reward[0] = DuelReward(env, env.FindBot(0), env.FindTarget(0), data);
    else if (HasPack())
        reward[0] = PackReward(env, env.FindBot(0), data);

    if (Player* bot = env.FindBot(0))
    {
        Powers const power = bot->getPowerType();
        uint32 const current = bot->GetPower(power);
        float const maxPower = float(std::max<uint32>(1, bot->GetMaxPower(power)));
        data.LastStepPowerDelta = (float(current) - float(data.LastPower)) / maxPower;
        data.LastPower = current;
    }
}

void AnimusForge::ClassRoleScenario::EpisodeInfo(Env const& env, float* info) const
{
    AgentStats const& stats = env.EpisodeStats[0];
    EnvData const& data = _data[env.Index];
    float const seconds = std::max(0.001f, float(env.EpisodeElapsedMs) / 1000.0f);

    info[INFO_DAMAGE] = float(stats.Damage);
    info[INFO_DPS] = float(stats.Damage) / seconds;
    info[INFO_WHITE_DAMAGE] = float(stats.WhiteDamage);
    info[INFO_SPECIAL_DAMAGE] = float(stats.SpecialDamage);
    info[INFO_LEVEL] = float(data.Level);
    info[INFO_RACE] = float(data.Race);
    info[INFO_SPEC] = float(data.Spec);
    info[INFO_UNSPENT_TALENT_POINTS] = float(data.UnspentTalentPoints);
    info[INFO_EQUIPPED_ITEMS] = float(data.EquippedItems);
    info[INFO_SPELL_CASTS] = float(data.SpellCasts);
    info[INFO_TRINKET_USES] = float(data.TrinketUses);

    if (HasDuel())
        DuelEpisodeInfo(env, info + _duelInfoFirst);
    if (HasPack())
        PackEpisodeInfo(env, info + _packInfoFirst);
    if (HasGauntlet())
        GauntletEpisodeInfo(env, info + _gauntletInfoFirst);
}

std::vector<std::string> AnimusForge::ClassRoleScenario::EpisodeInfoNames() const
{
    std::vector<std::string> names = { "damage", "dps", "white_damage", "special_damage", "level", "race", "spec",
        "unspent_talent_points", "equipped_items", "spell_casts", "trinket_uses" };

    if (HasDuel())
        names.insert(names.end(), { "killed", "died", "time_to_kill", "damage_taken", "health_left",
            "stealth_openers", "pet_summoned", "opponent" });

    if (HasPack())
        names.insert(names.end(), { "kills", "interrupts", "pack_size", "linked" });

    if (HasGauntlet())
        names.insert(names.end(), { "pulls_cleared", "food_used", "drink_used", "sustain_casts" });

    return names;
}

Creature* AnimusForge::ClassRoleScenario::CurrentTarget(Env const& env)
{
    if (!HasPack())
        return env.FindTarget(0);

    EnvData& data = _data[env.Index];
    if (Creature* selected = env.FindTarget(data.TargetSlot); selected && selected->IsAlive())
        return selected;

    // The selection died or despawned: the nearest living enemy, like a player tabbing to the next one.
    Player* bot = env.FindBot(0);
    Creature* nearest = nullptr;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Creature* enemy = env.FindTarget(slot);
        if (!enemy || !enemy->IsAlive())
            continue;

        if (!nearest || (bot && bot->GetDistance(enemy) < bot->GetDistance(nearest)))
        {
            nearest = enemy;
            data.TargetSlot = slot;
        }
    }

    return nearest;
}

bool AnimusForge::ClassRoleScenario::ScriptedAction(std::string const& policy, float const* obs,
    uint8 const* mask, int32& action) const
{
    // Smoke-test baselines. "greedy": the first usable spell or trinket in catalog order. "fight" (duel
    // stage on): also start attacking, run to the target, and eat or drink between gauntlet pulls.
    if (policy != "greedy" && !(policy == "fight" && HasDuel()))
        return false;

    action = 0;

    if (policy == "fight")
    {
        float const* duel = obs + _duelObsFirst;
        bool const hasTarget = duel[DUEL_OBS_DISTANCE] > 0.0f;

        if (HasGauntlet() && !hasTarget && obs[OBS_HEALTH] < 0.8f && mask[_gauntletActionFirst + GAUNTLET_ACTION_EAT])
        {
            action = int32(_gauntletActionFirst + GAUNTLET_ACTION_EAT);
            return true;
        }

        if (HasGauntlet() && !hasTarget && obs[OBS_MANA] > 0.0f && obs[OBS_MANA] < 0.8f
            && mask[_gauntletActionFirst + GAUNTLET_ACTION_DRINK])
        {
            action = int32(_gauntletActionFirst + GAUNTLET_ACTION_DRINK);
            return true;
        }

        if (mask[_duelActionFirst + DUEL_ACTION_START_ATTACK])
        {
            action = int32(_duelActionFirst + DUEL_ACTION_START_ATTACK);
            return true;
        }

        if (hasTarget && duel[DUEL_OBS_DISTANCE] * 60.0f > 4.0f && duel[DUEL_OBS_BOT_MOVING] == 0.0f
            && mask[_duelActionFirst + DUEL_ACTION_MOVE_TO_TARGET])
        {
            action = int32(_duelActionFirst + DUEL_ACTION_MOVE_TO_TARGET);
            return true;
        }
    }

    for (uint32 i = 2; i < _catalog->Actions().size(); ++i)
    {
        if (mask[i])
        {
            action = int32(i);
            break;
        }
    }

    return true;
}

void AnimusForge::ClassRoleScenario::Teardown(Env& env)
{
    EnvData& data = _data[env.Index];

    for (uint32 target = 0; target < env.Targets.size(); ++target)
        if (Creature* creature = env.FindTarget(target))
            creature->DespawnOrUnsummon();

    WorldSession* activeSession = data.Sessions[data.ActiveSession];
    if (Player* bot = activeSession ? activeSession->GetPlayer() : nullptr)
    {
        BotFactory::Destroy(bot);
        data.Sessions[data.ActiveSession] = nullptr;
    }

    env.Bots.clear();
    env.Targets.clear();
}
