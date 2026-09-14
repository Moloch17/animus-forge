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
#include <filesystem>
#include <fstream>

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
    constexpr float PARTY_SPACING = 3.0f;
    constexpr uint32 SEAT_ACCOUNT_SLOTS = AnimusForge::ClassRoleScenario::MAX_SEATS * 2;

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

    SpellCastTargets TargetsFor(SpellInfo const* info, Player* bot, Unit* target)
    {
        SpellCastTargets targets;

        // No target (between gauntlet pulls): only self-cast spells can succeed.
        if (target && (info->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION))
            targets.SetDst(*target);

        if (info->NeedsExplicitUnitTarget() && target && !info->IsPositive())
            targets.SetUnitTarget(target);
        else
            targets.SetUnitTarget(bot);

        return targets;
    }

    /// A cast in its cast time (channels excluded): the client refuses to start another spell or use an item
    /// meanwhile. The core only checks this for client casts (m_cast_count), so the bot's actions check it
    /// here -- otherwise a new cast would silently cancel the one in progress. Stopping it is its own action.
    bool CastInProgress(Player const* bot)
    {
        return bot->IsNonMeleeSpellCast(false, true, true);
    }

    /// The core's own cast validation, without casting: cooldown, GCD, power, stance, range, facing,
    /// reagents, reactive requirements. Same pattern as PetAI.
    bool CanCast(Player* bot, SpellInfo const* info, Unit* target, Item* castItem = nullptr)
    {
        Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
        spell->m_CastItem = castItem;
        spell->LoadScripts();

        SpellCastTargets targets = TargetsFor(info, bot, target);
        spell->InitExplicitTargets(targets);

        SpellCastResult const result = spell->CheckCast(true);
        delete spell;

        return result == SPELL_CAST_OK;
    }
}

void AnimusForge::ClassRoleScenario::Seat::ResetEpisode()
{
    LastStepDamage = 0.0f;
    LastStepPowerDelta = 0.0f;
    SpellCasts = 0;
    TrinketUses = 0;

    LastDistance = -1.0f;
    KillTimeMs = 0;
    DamageTaken = 0;
    LastStepDamageTaken = 0.0f;
    StealthOpeners = 0;
    StepStealthOpener = false;
    PetSummoned = false;
    CastsCompleted = 0;
    CastsCancelled = 0;
    CastMsWasted = 0;
    Killed = false;
    Died = false;

    TargetSlot = 0;
    Interrupts = 0;
    PendingInterrupt.Clear();
    PullDamageTaken = 0;

    FoodUsed = 0;
    DrinkUsed = 0;
    SustainCasts = 0;

    OwnerHealing = 0;
    ThreatOnBot = 0;
    OwnerDeathSeen = false;

    TeammateDamageTaken = 0;
    TeammateHealing = 0;
    ThreatOnTeammates = 0;
    TeammatesDied = 0;
    TeammateDeathSeen.fill(false);
}

std::string AnimusForge::ClassRoleScenario::ScenarioName(ArenaMode mode)
{
    return Animus::ClassRole::StageScenarioName(mode);
}

AnimusForge::ClassRoleScenario::ClassRoleScenario(ForgeConfig const& config, ArenaMode mode)
    : _mode(mode), _name(ScenarioName(mode)), _arenaMapId(config.ArenaMapId), _arenaPosition(config.ArenaPosition)
{
    _seatCount = IsParty() ? MAX_SEATS : IsArena() ? 2 : 1;

    // The class/roles this run plays: AnimusForge.ClassRoles, or all of them.
    for (ClassRoleProfile const& profile : ClassRoleProfiles())
    {
        if (!config.ClassRoles.empty()
            && std::find(config.ClassRoles.begin(), config.ClassRoles.end(), profile.ScenarioName)
                == config.ClassRoles.end())
            continue;

        if (ClassRoleAssets::For(profile).Races.empty())
            continue;

        Layout layout = Layout::Build(profile, _mode);
        layout.Index = uint16(_layouts.size());
        _layouts.push_back(std::move(layout));
    }

    _spec.AgentsPerEnv = _seatCount;
    for (Layout const& layout : _layouts)
    {
        _spec.ObsDim = std::max(_spec.ObsDim, layout.ObsDim);
        _spec.NumActions = std::max(_spec.NumActions, layout.NumActions);
        _spec.Layouts.push_back(LayoutSpec{ layout.Profile->ScenarioName, layout.ObsDim, layout.NumActions });
    }

    _spec.StateDim = STATE_GLOBAL_COUNT + MAX_SEATS * STATE_SEAT_FEATURES + PACK_SLOTS * STATE_ENEMY_FEATURES;

    _spec.EpisodeInfoDim = INFO_COUNT;
    if (HasDuel())
    {
        _duelInfoFirst = _spec.EpisodeInfoDim;
        _spec.EpisodeInfoDim += DUEL_INFO_COUNT;
        DuelArena::OpponentPool::Instance();    // load it at startup rather than on the first episode
    }
    if (HasPack())
    {
        _packInfoFirst = _spec.EpisodeInfoDim;
        _spec.EpisodeInfoDim += PACK_INFO_COUNT;
    }
    if (HasGauntlet())
    {
        _gauntletInfoFirst = _spec.EpisodeInfoDim;
        _spec.EpisodeInfoDim += GAUNTLET_INFO_COUNT;
        DuelArena::ConsumablePool::Instance();
    }
    if (HasCompanion())
    {
        _companionInfoFirst = _spec.EpisodeInfoDim;
        _spec.EpisodeInfoDim += COMPANION_INFO_COUNT;
    }
    if (HasParty())
    {
        _partyInfoFirst = _spec.EpisodeInfoDim;
        _spec.EpisodeInfoDim += PARTY_INFO_COUNT;
    }
    if (HasPvp())
    {
        _pvpInfoFirst = _spec.EpisodeInfoDim;
        _spec.EpisodeInfoDim += PVP_INFO_COUNT;
    }

    _data.resize(config.Envs);

    // Each layout's manifest, for the learner to publish beside the model (see Layout::Manifest).
    std::filesystem::path const manifests = std::filesystem::path(config.LearnerWorkDir) / "layouts" / _name;
    std::error_code error;
    std::filesystem::create_directories(manifests, error);
    for (Layout const& layout : _layouts)
    {
        std::ofstream file(manifests / (layout.ModelName() + ".json"), std::ios::trunc);
        file << layout.Manifest() << '\n';
        if (!file)
            LOG_WARN("module.animus", "{}: could not write the {} layout manifest to {}", Name(), layout.ModelName(),
                manifests.string());
    }

    LOG_INFO("module.animus", "{}: {} seats per env, {} class/role layouts (obs up to {}, actions up to {}), state {}",
        Name(), _seatCount, _layouts.size(), _spec.ObsDim, _spec.NumActions, _spec.StateDim);
}

AnimusForge::ClassRoleScenario::Layout const& AnimusForge::ClassRoleScenario::PickLayout(Role role,
    uint8 maxMinLevel) const
{
    // A class/role of the role if the run has one; any otherwise (AnimusForge.ClassRoles may leave roles out).
    std::vector<Layout const*> candidates;
    for (Layout const& layout : _layouts)
        if (layout.PlayRole() == role && layout.Assets->Kit->MinLevel() <= maxMinLevel)
            candidates.push_back(&layout);

    if (candidates.empty())
        for (Layout const& layout : _layouts)
            candidates.push_back(&layout);

    return *candidates[urand(0, uint32(candidates.size()) - 1)];
}

bool AnimusForge::ClassRoleScenario::IsTerminal(Env const& env) const
{
    EnvData const& data = _data[env.Index];
    auto const allDied = [&]()
    {
        for (uint32 seat = 0; seat < _seatCount; ++seat)
            if (!data.Seats[seat].Died)
                return false;
        return true;
    };

    switch (_mode)
    {
        case ArenaMode::Duel:
        case ArenaMode::Pack:
            return data.Seats[0].Killed || data.Seats[0].Died;     // pack: Killed = cleared
        case ArenaMode::Gauntlet:
            return data.Seats[0].Died;
        case ArenaMode::Companion:
            return data.Seats[0].Died || data.OwnerDied;
        case ArenaMode::Party:
            return data.OwnerDied || allDied();
        case ArenaMode::Pvp:
            return data.Seats[0].Died || data.Seats[0].Killed;
        case ArenaMode::Arena:
            return data.Seats[0].Died || data.Seats[1].Died;
        case ArenaMode::Dummy:
            break;
    }

    return false;
}

AnimusForge::ClassRoleScenario::~ClassRoleScenario()
{
    // Sessions kept for the next rebuild. Teardown already destroyed the active ones with their bots.
    for (EnvData& data : _data)
    {
        for (Seat& seat : data.Seats)
            for (WorldSession*& session : seat.Sessions)
                delete session;
        for (WorldSession*& session : data.OwnerSessions)
            delete session;
        for (WorldSession*& session : data.OpponentSessions)
            delete session;
    }
}

bool AnimusForge::ClassRoleScenario::Setup(Env& env)
{
    if (_layouts.empty())
    {
        LOG_ERROR("module.animus", "{}: no class/role to play (check AnimusForge.ClassRoles)", Name());
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
    for (Seat& seat : data.Seats)
        seat.ResetEpisode();

    data.PackLinked = false;
    data.PackSize = 0;
    data.PullKills = 0;
    data.Kills = 0;
    data.PullStartMs = 0;
    data.PullCleared = false;
    data.NewKills = 0;
    data.PullsCleared = 0;
    data.NextPullMs = 0;
    data.EliteOrHigherPull = false;
    data.OwnerDied = false;
    data.OwnerDamageTaken = 0;
    data.ThreatOnOwner = 0;

    // Setup already built the first episode's characters.
    if (data.Fresh)
    {
        data.Fresh = false;
        return;
    }

    if (!Rebuild(env))
        LOG_ERROR("module.animus", "{}: env {} could not build new characters; it keeps the old ones", Name(),
            env.Index);
}

Player* AnimusForge::ClassRoleScenario::SeatBot(EnvData const& data, uint32 seat) const
{
    // Through the session rather than ObjectAccessor::FindPlayer: a bot that is out of the world (a far teleport in
    // progress) is still the seat's bot and still has to be destroyed.
    Seat const& slot = data.Seats[seat];
    WorldSession* session = slot.Sessions[slot.ActiveSession];
    return session ? session->GetPlayer() : nullptr;
}

bool AnimusForge::ClassRoleScenario::Rebuild(Env& env)
{
    EnvData& data = _data[env.Index];

    std::vector<Creature*> oldTargets;
    for (uint32 target = 0; target < env.Targets.size(); ++target)
        if (Creature* creature = env.FindTarget(target))
            oldTargets.push_back(creature);

    std::array<Player*, MAX_SEATS> oldBots{};
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        oldBots[seat] = SeatBot(data, seat);

    // The old party goes before its members do.
    if (IsParty())
        DisbandParty(env);

    // The class/roles first (a party: tank, healer, two damage dealers), then one level they can all be.
    static constexpr std::array<Role, MAX_SEATS> PARTY_ROLES = { Role::Tank, Role::Heal, Role::Dps, Role::Dps };
    uint8 minLevel = 1;
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        Role const role = IsParty() ? PARTY_ROLES[seat] : _layouts[urand(0, uint32(_layouts.size()) - 1)].PlayRole();
        data.Seats[seat].L = &PickLayout(role, DEFAULT_MAX_LEVEL);
        minLevel = std::max(minLevel, data.Seats[seat].L->Assets->Kit->MinLevel());
    }

    uint8 const level = uint8(urand(minLevel, DEFAULT_MAX_LEVEL));
    Map* map = oldBots[0] ? env.FindMap() : nullptr;

    // The new bots go on idle sessions and into the map before the old ones leave, so the instance always has a
    // bound player.
    std::array<uint8, MAX_SEATS> newSessions{};
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        Position start = _arenaPosition;
        if (IsParty())
        {
            start.m_positionX += (seat % 2 ? -PARTY_SPACING : PARTY_SPACING) * float(1 + seat / 2);
            start.m_positionY += (seat % 2 ? PARTY_SPACING : -PARTY_SPACING);
        }
        else if (IsArena() && seat == 1)
        {
            // Out of range of the first seat's new bot, at a random bearing, facing a random way.
            WorldSession* firstSession = data.Seats[0].Sessions[newSessions[0]];
            if (Player* first = firstSession ? firstSession->GetPlayer() : nullptr)
                start = DuelArena::FindSpawnPoint(first, map);
            start.SetOrientation(frand(0.0f, 2.0f * float(M_PI)));
        }

        if (!BuildSeat(env, seat, map, level, start, newSessions[seat]))
            return false;
    }

    for (Creature* creature : oldTargets)
        creature->DespawnOrUnsummon();

    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        Seat& slot = data.Seats[seat];
        if (oldBots[seat])
            slot.Sessions[slot.ActiveSession] = BotFactory::Destroy(oldBots[seat], true);
        slot.ActiveSession = newSessions[seat];
    }

    Player* lead = SeatBot(data, 0);
    if (!oldBots[0])
        TrainingDummyArena::ClearArena(lead);

    env.MapId = map->GetId();
    env.InstanceId = map->GetInstanceId();
    env.Bots.clear();
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        env.Bots.push_back(SeatBot(data, seat)->GetGUID());
    env.Targets.clear();

    // Teammates are friends whatever their races; arena opponents are made enemies by StartPvp.
    if (IsParty())
        for (uint32 seat = 1; seat < _seatCount; ++seat)
            SeatBot(data, seat)->SetFaction(lead->GetFaction());

    if (HasPvp())
        return StartPvp(env, map);

    // The owner comes before the first pull, which spawns around it.
    if (HasCompanion() && !RebuildOwner(env, lead, map, level))
        return false;

    if (IsParty())
        FormParty(env);

    if (HasPack())
    {
        for (uint32 seat = 0; seat < _seatCount; ++seat)
            StartSeatPack(SeatBot(data, seat), data.Seats[seat]);
        return SpawnPull(env, map);
    }

    Seat& seat = data.Seats[0];
    if (_mode == ArenaMode::Duel)
    {
        data.OpponentEntry = DuelArena::OpponentPool::Instance().Random(seat.Level);
        Creature* opponent = data.OpponentEntry ? DuelArena::SpawnOpponent(lead, map, data.OpponentEntry) : nullptr;
        if (!opponent)
            return false;

        env.Targets = { opponent->GetGUID() };
        StartDuel(lead, seat);
        return true;
    }

    SpecProfile const& specProfile = seat.L->Profile->Specs[seat.Spec];
    float const distance = specProfile.Range == RangeBand::Melee ? MELEE_DISTANCE : RANGED_DISTANCE;

    Creature* dummy = TrainingDummyArena::SpawnDummy(lead, map, distance);
    if (!dummy)
        return false;

    env.Targets = { dummy->GetGUID() };
    StartFight(lead, dummy, seat);
    return true;
}

bool AnimusForge::ClassRoleScenario::BuildSeat(Env& env, uint32 seatIndex, Map*& map, uint8 level,
    Position const& start, uint8& newSession)
{
    EnvData& data = _data[env.Index];
    Seat& seat = data.Seats[seatIndex];
    Layout const& layout = *seat.L;
    Player* old = SeatBot(data, seatIndex);

    seat.Race = layout.Assets->Races[urand(0, uint32(layout.Assets->Races.size()) - 1)];
    seat.Level = level;
    seat.Spec = uint8(urand(0, uint32(layout.Profile->Specs.size()) - 1));
    seat.StartHealth = frand(0.2f, 1.0f);
    seat.EndHealth = frand(0.0f, seat.StartHealth);
    seat.DamageScale = DamageScaleForLevel(level);

    newSession = old ? uint8(1 - seat.ActiveSession) : seat.ActiveSession;

    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Forge{}s{}{}", env.Index, seatIndex, newSession ? "b" : "a");
    spec.Race = seat.Race;
    spec.Class = layout.Profile->Class;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = level;
    spec.AccountId = TrainingDummyArena::BOT_ACCOUNT_BASE + env.Index * SEAT_ACCOUNT_SLOTS + seatIndex * 2 + newSession;
    spec.GuidLow = seat.Guids[newSession];

    Player* bot = BotFactory::Create(spec, seat.Sessions[newSession]);
    if (!bot)
        return false;

    seat.Sessions[newSession] = bot->GetSession();
    seat.Guids[newSession] = bot->GetGUID().GetCounter();

    bool const placed = map ? BotFactory::PlaceInMap(bot, map, start)
        : (map = BotFactory::PlaceInNewInstance(bot, _arenaMapId, start)) != nullptr;
    if (!placed)
    {
        seat.Sessions[newSession] = nullptr;
        BotFactory::DestroyUnplaced(bot);
        return false;
    }

    // Talent points depend on the map for death knights (Ebon Hold, where Create put the bot, only counts
    // quest-rewarded points); recompute them on the arena map.
    bot->InitTalentForLevel();
    Configure(bot, seat);
    return true;
}

void AnimusForge::ClassRoleScenario::Configure(Player* bot, Seat& seat) const
{
    ClassRoleAssets const& assets = *seat.L->Assets;
    SpecProfile const& spec = seat.L->Profile->Specs[seat.Spec];

    GearBuilder::LearnProficiencies(bot);

    seat.Build = assets.Talents->Random(spec.TabPage, bot->GetFreeTalentPoints());
    seat.UnspentTalentPoints = assets.Talents->Apply(bot, seat.Build);

    assets.Kit->Learn(bot);
    assets.Gear->Equip(bot, spec);

    seat.EquippedItems = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++seat.EquippedItems;

    bot->UpdateAllStats();
    bot->SetFullHealth();
    bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
    bot->SetPower(POWER_ENERGY, bot->GetMaxPower(POWER_ENERGY));
    bot->SetPower(POWER_RAGE, 0);
    bot->SetPower(POWER_RUNIC_POWER, 0);
}

void AnimusForge::ClassRoleScenario::StartFight(Player* bot, Unit* dummy, Seat const& seat) const
{
    SpecProfile const& spec = seat.L->Profile->Specs[seat.Spec];

    bot->SetOrientation(bot->GetAngle(dummy));

    // A warrior has no stance until one is cast (a first login casts it), and nothing works without one.
    if (seat.L->Profile->Class == CLASS_WARRIOR)
    {
        uint32 const stance = seat.L->PlayRole() == Role::Tank && bot->HasSpell(SPELL_DEFENSIVE_STANCE)
            ? SPELL_DEFENSIVE_STANCE : SPELL_BATTLE_STANCE;
        bot->CastSpell(bot, stance, true);
    }

    bool const melee = spec.Range == RangeBand::Melee;
    bot->Attack(dummy, melee);

    // Random swing phase so episodes do not all start on the same swing boundary.
    if (melee)
        bot->setAttackTimer(BASE_ATTACK, int32(urand(0, bot->GetAttackTime(BASE_ATTACK))));

    dummy->SetHealth(std::max<uint32>(1, uint32(float(dummy->GetMaxHealth()) * seat.StartHealth)));
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

bool AnimusForge::ClassRoleScenario::IsActionAllowed(Seat const& seat, Player* bot, Unit* target, uint32 action) const
{
    ActionCatalog::Action const& def = seat.L->Catalog().Actions()[action];

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
            return info && !CastInProgress(bot) && !bot->HasSpellCooldown(info->Id) && CanCast(bot, info, target, item);
        }
        case ActionCatalog::Kind::Spell:
            break;
    }

    return IsSpellActionAllowed(bot, target, def);
}

bool AnimusForge::ClassRoleScenario::IsSpellActionAllowed(Player* bot, Unit* target,
    ActionCatalog::Action const& def) const
{
    // Cheap rejections before the full cast check.
    SpellInfo const* info = ActionCatalog::KnownRank(bot, def.FirstRank);
    if (!info || !bot->HasActiveSpell(info->Id) || bot->HasSpellCooldown(info->Id) || CastInProgress(bot))
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
    Seat& seat) const
{
    if (def.NextSwing && bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
        return false;

    SpellInfo const* info = ActionCatalog::KnownRank(bot, def.FirstRank);
    if (!info || !bot->HasActiveSpell(info->Id) || CastInProgress(bot))
        return false;

    // Same path as CMSG_CAST_SPELL. prepare() runs the full cast validation again, so a masked action
    // from a misbehaving client simply fails. The spell owns and frees itself.
    SpellCastTargets targets = TargetsFor(info, bot, target);
    bool const stealthed = bot->HasAuraType(SPELL_AURA_MOD_STEALTH);
    bool const targetCasting = target && target->IsNonMeleeSpellCast(false);
    Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
    if (spell->prepare(&targets) != SPELL_CAST_OK)
        return false;

    ++seat.SpellCasts;

    // A stealth opener (Ambush, Garrote, Cheap Shot, Ravage, Pounce, ...) on the opponent.
    if (HasDuel() && stealthed && info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED)
        && info->NeedsExplicitUnitTarget() && !info->IsPositive())
    {
        seat.StepStealthOpener = true;
        ++seat.StealthOpeners;
    }

    // An interrupt attempt on a casting enemy; the reward checks next decision whether the cast stopped.
    if (HasPack() && targetCasting && target != bot && ActionCatalog::IsInterruptingSpell(info))
        seat.PendingInterrupt = target->GetGUID();

    return true;
}

void AnimusForge::ClassRoleScenario::UpdateDummyHealth(Env const& env, Seat const& seat, Unit* dummy) const
{
    float const progress = env.EpisodeLengthMs
        ? std::min(1.0f, float(env.EpisodeElapsedMs) / float(env.EpisodeLengthMs)) : 0.0f;
    float const fraction = seat.StartHealth + (seat.EndHealth - seat.StartHealth) * progress;

    dummy->SetHealth(std::max<uint32>(1, uint32(float(dummy->GetMaxHealth()) * fraction)));
}

void AnimusForge::ClassRoleScenario::ApplyActions(Env& env, int32 const* actions)
{
    // Env upkeep first (linked pulls, the gauntlet's next pull, the owner, the scripted opponent), so the targets
    // below are current.
    if (HasPvp())
        UpdatePvp(env);
    else if (HasPack())
        UpdatePack(env);

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        ApplySeatAction(env, seat, actions[seat]);
}

void AnimusForge::ClassRoleScenario::ApplySeatAction(Env& env, uint32 seatIndex, int32 action)
{
    Player* bot = env.FindBot(seatIndex);
    if (!bot)
        return;

    Seat& seat = _data[env.Index].Seats[seatIndex];
    Layout const& layout = *seat.L;
    Unit* target = CurrentTarget(env, seatIndex);

    // Only the gauntlet has moments without a target (between pulls).
    if (!target && !HasGauntlet())
        return;

    if (_mode == ArenaMode::Dummy)
        UpdateDummyHealth(env, seat, target);
    else
    {
        // Face the target whenever not running somewhere: casts and swings need it, and turning is not a decision
        // worth learning.
        if (target && bot->IsAlive() && bot->movespline->Finalized() && !bot->HasInArc(float(M_PI) / 2, target))
            bot->SetFacingToObject(target);

        if (HasParty() && action >= int32(layout.PartyActionFirst))
        {
            ApplyPartyAction(env, seatIndex, bot, uint32(action) - layout.PartyActionFirst);
            return;
        }

        if (HasCompanion() && action >= int32(layout.CompanionActionFirst)
            && action < int32(layout.CompanionActionFirst + layout.CompanionActionCount))
        {
            ApplyCompanionAction(env, seatIndex, bot, uint32(action) - layout.CompanionActionFirst);
            return;
        }

        if (HasGauntlet() && action >= int32(layout.GauntletActionFirst)
            && action < int32(layout.GauntletActionFirst + layout.GauntletActionCount))
        {
            ApplyGauntletAction(bot, target, uint32(action) - layout.GauntletActionFirst, seat);
            return;
        }

        if (HasPack() && action >= int32(layout.PackActionFirst)
            && action < int32(layout.PackActionFirst + layout.PackActionCount))
        {
            ApplyPackAction(env, seatIndex, bot, target, uint32(action) - layout.PackActionFirst);
            return;
        }

        if (action >= int32(layout.DuelActionFirst) && action < int32(layout.DuelActionFirst + layout.DuelActionCount))
        {
            // Stopping a cast and leaving a form need no target; ApplyDuelAction checks the rest.
            ApplyDuelAction(bot, target, uint32(action) - layout.DuelActionFirst, seat);
            return;
        }
    }

    std::vector<ActionCatalog::Action> const& catalog = layout.Catalog().Actions();
    if (action <= 0 || action >= int32(catalog.size()))
        return;

    ActionCatalog::Action const& def = catalog[action];
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

            bot->CastItemUseSpell(item, TargetsFor(info, bot, target), 1, 0);
            if (bot->HasSpellCooldown(info->Id))
                ++seat.TrinketUses;
            return;
        }
        case ActionCatalog::Kind::Spell:
            break;
    }

    ApplySpellAction(bot, target, def, seat);
}

void AnimusForge::ClassRoleScenario::Observe(Env& env, float* obs, float* state, uint8* mask)
{
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        ObserveSeat(env, seat, obs + seat * _spec.ObsDim, mask + seat * _spec.NumActions);

    BuildState(env, state);
}

void AnimusForge::ClassRoleScenario::AgentLayouts(Env const& env, uint16* layout) const
{
    EnvData const& data = _data[env.Index];
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        layout[seat] = data.Seats[seat].L ? data.Seats[seat].L->Index : 0;
}

void AnimusForge::ClassRoleScenario::ObserveSeat(Env& env, uint32 seatIndex, float* obs, uint8* mask)
{
    std::fill(obs, obs + _spec.ObsDim, 0.0f);
    std::fill(mask, mask + _spec.NumActions, 0);
    mask[0] = 1;

    Seat const& seat = _data[env.Index].Seats[seatIndex];
    if (!seat.L)
        return;

    Layout const& layout = *seat.L;
    Player* bot = env.FindBot(seatIndex);
    Unit* target = CurrentTarget(env, seatIndex);      // may be null between gauntlet pulls

    obs[OBS_LEVEL] = float(seat.Level) / float(DEFAULT_MAX_LEVEL);
    for (uint32 i = 0; i < PLAYABLE_RACES.size(); ++i)
        obs[OBS_RACE_FIRST + i] = PLAYABLE_RACES[i] == seat.Race ? 1.0f : 0.0f;
    obs[OBS_SPEC_FIRST + std::min<uint32>(seat.Spec, MAX_SPECS - 1)] = 1.0f;

    if (bot && bot->IsAlive() && (target || HasGauntlet()))
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

        if (target)
            obs[OBS_COMBO_POINTS] = float(bot->GetComboPoints(target)) / 5.0f;

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
        if (target)
        {
            obs[OBS_TARGET_HEALTH] = target->GetHealthPct() / 100.0f;
            obs[OBS_TARGET_DISTANCE] = std::min(1.0f, bot->GetDistance(target) / 40.0f);
            obs[OBS_IN_MELEE_FRONT] = bot->IsWithinMeleeRange(target) && bot->HasInArc(2 * float(M_PI) / 3, target)
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
        obs[OBS_LAST_STEP_DAMAGE] = seat.LastStepDamage;
        obs[OBS_LAST_STEP_POWER_DELTA] = seat.LastStepPowerDelta;

        std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
        for (uint32 action = 0; action < actions.size(); ++action)
        {
            SpellInfo const* info = nullptr;
            if (actions[action].Type == ActionCatalog::Kind::Spell)
                info = ActionCatalog::KnownRank(bot, actions[action].FirstRank);
            else if (actions[action].Type == ActionCatalog::Kind::Trinket)
                info = TrinketSpell(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, actions[action].EquipmentSlot));

            if (info)
            {
                float* features = obs + layout.ActionObsFirst + action * ACTION_FEATURES;
                float stacks = 0.0f;
                features[0] = 1.0f;
                features[1] = CooldownFraction(bot, info);
                features[2] = target ? AuraFraction(target, info->Id, botGuid, stacks) : 0.0f;
                features[3] = AuraFraction(bot, info->Id, botGuid, stacks);
                features[4] = stacks;

                if (!obs[OBS_GCD] && info->StartRecoveryTime)
                    obs[OBS_GCD] = std::min(1.0f, float(bot->GetGlobalCooldownMgr().GetGlobalCooldown(info)) / GCD_MS);
            }

            if (action > 0)
                mask[action] = IsActionAllowed(seat, bot, target, action) ? 1 : 0;
        }

        if (HasDuel())
        {
            ObserveDuel(env, seat, bot, target, obs);
            for (uint32 duelAction = 0; duelAction < layout.DuelActionCount; ++duelAction)
                mask[layout.DuelActionFirst + duelAction] = IsDuelActionAllowed(bot, target, duelAction, seat) ? 1 : 0;
        }

        if (HasPack())
        {
            ObservePack(env, seatIndex, bot, obs);
            std::vector<ActionCatalog::Action> const& tactical = layout.Catalog().Tactical();
            for (uint32 packAction = 0; packAction < layout.PackActionCount; ++packAction)
                mask[layout.PackActionFirst + packAction] = packAction < PACK_SLOTS
                    ? IsPackActionAllowed(env, seatIndex, bot, packAction)
                    : target && IsSpellActionAllowed(bot, target, tactical[packAction - PACK_SLOTS]);
        }

        if (HasGauntlet())
        {
            ObserveGauntlet(env, seatIndex, bot, obs);
            for (uint32 gauntletAction = 0; gauntletAction < layout.GauntletActionCount; ++gauntletAction)
                mask[layout.GauntletActionFirst + gauntletAction] =
                    IsGauntletActionAllowed(bot, target, gauntletAction, seat) ? 1 : 0;
        }

        if (HasCompanion())
        {
            ObserveCompanion(env, seatIndex, bot, obs);
            for (uint32 companionAction = 0; companionAction < layout.CompanionActionCount; ++companionAction)
                mask[layout.CompanionActionFirst + companionAction] =
                    IsCompanionActionAllowed(env, seatIndex, bot, companionAction) ? 1 : 0;
        }

        if (HasParty())
        {
            ObserveParty(env, seatIndex, bot, obs);
            for (uint32 partyAction = 0; partyAction < layout.PartyActionCount; ++partyAction)
                mask[layout.PartyActionFirst + partyAction] =
                    IsPartyActionAllowed(env, seatIndex, bot, partyAction) ? 1 : 0;
        }

        if (HasPvp())
            ObservePvp(env, seatIndex, bot, obs);
    }

    std::vector<TalentBuilder::Talent> const& talents = layout.Assets->Talents->Talents();
    for (uint32 i = 0; i < talents.size() && i < seat.Build.Ranks.size(); ++i)
        obs[layout.TalentObsFirst + i] = float(seat.Build.Ranks[i]) / float(std::max<uint8>(1, talents[i].MaxRank));

    for (uint32 tree = 0; tree < TalentBuilder::TREE_COUNT; ++tree)
        obs[layout.TreeObsFirst + tree] = float(seat.Build.TreePoints[tree]) / TALENT_POINTS_AT_MAX_LEVEL;
}

void AnimusForge::ClassRoleScenario::Reward(Env& env, float* reward)
{
    // A pull is cleared (or not) for the whole party at once.
    if (HasPack() && IsPve())
        AssessPull(env);

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        reward[seat] = SeatReward(env, seat);

    if (HasPack() && IsPve())
        FinishPull(env);
}

float AnimusForge::ClassRoleScenario::SeatReward(Env& env, uint32 seatIndex)
{
    Seat& seat = _data[env.Index].Seats[seatIndex];
    Player* bot = env.FindBot(seatIndex);

    float reward = float(env.StepStats[seatIndex].Damage) / seat.DamageScale;
    seat.LastStepDamage = reward;

    switch (_mode)
    {
        case ArenaMode::Duel:      reward = DuelReward(env, seatIndex, bot, env.FindTargetUnit(0)); break;
        case ArenaMode::Pack:
        case ArenaMode::Gauntlet:  reward = PackReward(env, seatIndex, bot); break;
        case ArenaMode::Companion: reward = CompanionReward(env, seatIndex, bot); break;
        case ArenaMode::Party:     reward = PartyReward(env, seatIndex, bot); break;
        case ArenaMode::Pvp:
        case ArenaMode::Arena:     reward = PvpReward(env, seatIndex, bot); break;
        case ArenaMode::Dummy:     break;
    }

    if (bot)
    {
        Powers const power = bot->getPowerType();
        uint32 const current = bot->GetPower(power);
        float const maxPower = float(std::max<uint32>(1, bot->GetMaxPower(power)));
        seat.LastStepPowerDelta = (float(current) - float(seat.LastPower)) / maxPower;
        seat.LastPower = current;
    }

    return reward;
}

void AnimusForge::ClassRoleScenario::EpisodeInfo(Env const& env, float* info) const
{
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        SeatEpisodeInfo(env, seat, info + seat * _spec.EpisodeInfoDim);
}

void AnimusForge::ClassRoleScenario::SeatEpisodeInfo(Env const& env, uint32 seatIndex, float* info) const
{
    AgentStats const& stats = env.EpisodeStats[seatIndex];
    Seat const& seat = _data[env.Index].Seats[seatIndex];
    float const seconds = std::max(0.001f, float(env.EpisodeElapsedMs) / 1000.0f);

    info[INFO_DAMAGE] = float(stats.Damage);
    info[INFO_DPS] = float(stats.Damage) / seconds;
    info[INFO_WHITE_DAMAGE] = float(stats.WhiteDamage);
    info[INFO_SPECIAL_DAMAGE] = float(stats.SpecialDamage);
    info[INFO_LEVEL] = float(seat.Level);
    info[INFO_RACE] = float(seat.Race);
    info[INFO_SPEC] = float(seat.Spec);
    info[INFO_UNSPENT_TALENT_POINTS] = float(seat.UnspentTalentPoints);
    info[INFO_EQUIPPED_ITEMS] = float(seat.EquippedItems);
    info[INFO_SPELL_CASTS] = float(seat.SpellCasts);
    info[INFO_TRINKET_USES] = float(seat.TrinketUses);
    info[INFO_CLASS] = seat.L ? float(seat.L->Profile->Class) : 0.0f;
    info[INFO_ROLE] = seat.L ? float(uint32(seat.L->PlayRole())) : 0.0f;

    if (HasDuel())
        DuelEpisodeInfo(env, seatIndex, info + _duelInfoFirst);
    if (HasPack())
        PackEpisodeInfo(env, seatIndex, info + _packInfoFirst);
    if (HasGauntlet())
        GauntletEpisodeInfo(env, seatIndex, info + _gauntletInfoFirst);
    if (HasCompanion())
        CompanionEpisodeInfo(env, seatIndex, info + _companionInfoFirst);
    if (HasParty())
        PartyEpisodeInfo(env, seatIndex, info + _partyInfoFirst);
    if (HasPvp())
        PvpEpisodeInfo(env, seatIndex, info + _pvpInfoFirst);
}

std::vector<std::string> AnimusForge::ClassRoleScenario::EpisodeInfoNames() const
{
    std::vector<std::string> names = { "damage", "dps", "white_damage", "special_damage", "level", "race", "spec",
        "unspent_talent_points", "equipped_items", "spell_casts", "trinket_uses", "class", "role" };

    if (HasDuel())
        names.insert(names.end(), { "killed", "died", "time_to_kill", "damage_taken", "health_left",
            "stealth_openers", "pet_summoned", "opponent", "casts_completed", "casts_cancelled",
            "cast_seconds_wasted" });

    if (HasPack())
        names.insert(names.end(), { "kills", "interrupts", "pack_size", "linked" });

    if (HasGauntlet())
        names.insert(names.end(), { "pulls_cleared", "food_used", "drink_used", "sustain_casts" });

    if (HasCompanion())
        names.insert(names.end(), { "owner_class", "owner_died", "owner_damage_taken", "owner_healing",
            "threat_on_bot", "threat_on_owner" });

    if (HasParty())
        names.insert(names.end(), { "teammates_died", "teammate_damage_taken", "teammate_healing",
            "threat_on_teammates", "seat" });

    if (HasPvp())
        names.insert(names.end(), { "won", "opponent_class", "opponent_role" });

    return names;
}

Unit* AnimusForge::ClassRoleScenario::CurrentTarget(Env const& env, uint32 seatIndex)
{
    if (HasPvp())
        return FindOpponent(env, seatIndex);

    if (!HasPack())
        return env.FindTargetUnit(0);

    Seat& seat = _data[env.Index].Seats[seatIndex];
    if (Unit* selected = env.FindTargetUnit(seat.TargetSlot); selected && selected->IsAlive())
        return selected;

    // The selection died or despawned: the nearest living enemy, like a player tabbing to the next one.
    Player* bot = env.FindBot(seatIndex);
    Unit* nearest = nullptr;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy || !enemy->IsAlive())
            continue;

        if (!nearest || (bot && bot->GetDistance(enemy) < bot->GetDistance(nearest)))
        {
            nearest = enemy;
            seat.TargetSlot = slot;
        }
    }

    return nearest;
}

bool AnimusForge::ClassRoleScenario::ScriptedAction(std::string const& policy, float const* obs,
    uint8 const* mask, uint16 layoutIndex, int32& action) const
{
    // Smoke-test baselines. "greedy": the first usable spell or trinket in catalog order. "fight" (duel stage on):
    // also start attacking, run to the target, eat or drink between gauntlet pulls, and heal hurt allies.
    if (policy != "greedy" && !(policy == "fight" && HasDuel()))
        return false;

    action = 0;
    if (layoutIndex >= _layouts.size())
        return true;

    Layout const& layout = _layouts[layoutIndex];

    if (policy == "fight")
    {
        float const* duel = obs + layout.DuelObsFirst;
        bool const hasTarget = duel[DUEL_OBS_DISTANCE] > 0.0f;

        if (HasGauntlet() && !hasTarget && obs[OBS_HEALTH] < 0.8f
            && mask[layout.GauntletActionFirst + GAUNTLET_ACTION_EAT])
        {
            action = int32(layout.GauntletActionFirst + GAUNTLET_ACTION_EAT);
            return true;
        }

        if (HasGauntlet() && !hasTarget && obs[OBS_MANA] > 0.0f && obs[OBS_MANA] < 0.8f
            && mask[layout.GauntletActionFirst + GAUNTLET_ACTION_DRINK])
        {
            action = int32(layout.GauntletActionFirst + GAUNTLET_ACTION_DRINK);
            return true;
        }

        uint32 const heals = uint32(layout.AllyHeals.size());
        if (HasCompanion() && heals && obs[layout.CompanionObsFirst + COMPANION_OBS_OWNER_ALIVE] > 0.0f
            && obs[layout.CompanionObsFirst + COMPANION_OBS_OWNER_HEALTH] < 0.7f)
        {
            for (uint32 heal = 0; heal < heals; ++heal)
            {
                uint32 const index = layout.CompanionActionFirst + COMPANION_ACTION_HEAL_FIRST + heal;
                if (mask[index])
                {
                    action = int32(index);
                    return true;
                }
            }

            // Heals cannot be cast in most forms.
            if (mask[layout.DuelActionFirst + DUEL_ACTION_CANCEL_FORM])
            {
                action = int32(layout.DuelActionFirst + DUEL_ACTION_CANCEL_FORM);
                return true;
            }
        }

        // A hurt teammate: the first heal that can reach it.
        if (HasParty() && heals)
        {
            for (uint32 member = 0; member < PARTY_MEMBERS; ++member)
            {
                float const* features = obs + layout.PartyObsFirst + PARTY_OBS_GLOBAL_COUNT + member * MEMBER_FEATURES;
                if (features[MEMBER_ALIVE] == 0.0f || features[MEMBER_HEALTH] >= 0.6f)
                    continue;

                for (uint32 heal = 0; heal < heals; ++heal)
                {
                    uint32 const index = layout.PartyActionFirst + PARTY_ACTION_HEAL_FIRST + member * heals + heal;
                    if (mask[index])
                    {
                        action = int32(index);
                        return true;
                    }
                }
            }
        }

        if (mask[layout.DuelActionFirst + DUEL_ACTION_START_ATTACK])
        {
            action = int32(layout.DuelActionFirst + DUEL_ACTION_START_ATTACK);
            return true;
        }

        if (hasTarget && duel[DUEL_OBS_DISTANCE] * 60.0f > 4.0f && duel[DUEL_OBS_BOT_MOVING] == 0.0f
            && mask[layout.DuelActionFirst + DUEL_ACTION_MOVE_TO_TARGET])
        {
            action = int32(layout.DuelActionFirst + DUEL_ACTION_MOVE_TO_TARGET);
            return true;
        }
    }

    for (uint32 i = 2; i < layout.Catalog().Actions().size(); ++i)
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

    if (IsParty())
        DisbandParty(env);

    if (HasPvp())
        DestroyOpponent(env);

    if (HasCompanion())
        DestroyOwner(env);

    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        if (Player* bot = SeatBot(data, seat))
        {
            BotFactory::Destroy(bot);
            data.Seats[seat].Sessions[data.Seats[seat].ActiveSession] = nullptr;
        }
    }

    env.Bots.clear();
    env.Targets.clear();
}
