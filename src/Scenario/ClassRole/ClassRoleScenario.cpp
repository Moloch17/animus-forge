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
#include "Containers.h"
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

    constexpr float PARTY_SPACING = 3.0f;
    constexpr uint32 WORLD_TICK_MS = 50;            // the sim's fixed world tick
    constexpr uint32 MAX_COMBAT_TIME_MS = 60000;

    // Party makeup: how many of the four seats have a character (a player may bring 1-4 companions), and how often
    // the roles follow the classic tank, healer and damage dealers rather than being drawn one by one.
    constexpr std::array<int32, 4> PARTY_SIZE_WEIGHTS = { 20, 20, 20, 40 };     // 1, 2, 3, 4 seats
    constexpr int32 CLASSIC_PARTY_CHANCE = 50;

    // Levels: half the characters are 61-80, where most players are and where the pilot played worst; the rest are
    // spread over every level the class/roles can be.
    constexpr uint8 HIGH_LEVEL_FIRST = 61;
    constexpr int32 HIGH_LEVEL_CHANCE = 50;

    uint8 RandomLevel(uint8 minLevel)
    {
        if (minLevel <= HIGH_LEVEL_FIRST && roll_chance_i(HIGH_LEVEL_CHANCE))
            return uint8(urand(HIGH_LEVEL_FIRST, DEFAULT_MAX_LEVEL));
        return uint8(urand(minLevel, DEFAULT_MAX_LEVEL));
    }

    AnimusForge::Role RandomRole()
    {
        // Damage dealers are half of all characters, tanks and healers a quarter each.
        uint32 const roll = urand(0, 99);
        return roll < 50 ? AnimusForge::Role::Dps : roll < 75 ? AnimusForge::Role::Tank : AnimusForge::Role::Heal;
    }
    constexpr uint32 SEAT_ACCOUNT_SLOTS = AnimusForge::ClassRoleScenario::MAX_SEATS * 2;
}

void AnimusForge::ClassRoleScenario::Seat::ResetEpisode()
{
    LastStepDamage = 0.0f;
    LastStepPowerDelta = 0.0f;
    SpellCasts = 0;
    TrinketUses = 0;
    InCombat = false;
    CombatStartMs = 0;

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
    CastsStopped = 0;
    CastsMoved = 0;
    CastsTargetLost = 0;
    CastsOther = 0;
    Killed = false;
    Died = false;
    Deaths = 0;
    DeathCounted = false;

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
    return AnimusForge::ClassRole::StageScenarioName(mode);
}

AnimusForge::ClassRoleScenario::ClassRoleScenario(ForgeConfig const& config, ArenaMode mode)
    : _mode(mode), _name(ScenarioName(mode)), _arenaMapId(config.ArenaMapId), _arenaPosition(config.ArenaPosition)
{
    _seatCount = IsParty() ? MAX_SEATS : IsArena() ? 2 : 1;
    _decisionScale = float(config.DecisionTicks * WORLD_TICK_MS) / 50.0f;

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
        ConsumablePool::Instance();
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

    switch (_mode)
    {
        case ArenaMode::Duel:
        case ArenaMode::Pack:
            return data.Seats[0].Killed || data.Seats[0].Died;     // pack: Killed = cleared
        case ArenaMode::Gauntlet:
            return data.Seats[0].Died;
        case ArenaMode::Companion:
        case ArenaMode::Party:
            return false;       // deaths are recovered from after the pull (Recover); episodes end at their length
        case ArenaMode::Pvp:
            return data.Seats[0].Died || data.Seats[0].Killed;
        case ArenaMode::Arena:
            return data.Seats[0].Died || data.Seats[1].Died;
        case ArenaMode::Base:
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
    data.QuietSinceMs = 0;
    data.NextPullMs = 0;
    data.EliteOrHigherPull = false;
    data.OwnerDied = false;
    data.OwnerDeaths = 0;
    data.OwnerDeathCounted = false;
    data.Wipes = 0;
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

    // How many seats play this episode: every seat, except in a party, which has 1-4 like a player's companions.
    // The rest stay empty: no character, no layout, only the no-op allowed.
    data.ActiveSeats = _seatCount;
    std::array<Role, MAX_SEATS> roles{};
    if (IsParty())
    {
        int32 roll = irand(0, 99);
        data.ActiveSeats = 1;
        while (data.ActiveSeats < MAX_SEATS && roll >= PARTY_SIZE_WEIGHTS[data.ActiveSeats - 1])
            roll -= PARTY_SIZE_WEIGHTS[data.ActiveSeats++ - 1];

        // Half the parties are the classic makeup (as many of a tank, a healer and two damage dealers as there are
        // seats, in a random order); the rest draw every seat's role on its own.
        roles = { Role::Tank, Role::Heal, Role::Dps, Role::Dps };
        if (roll_chance_i(CLASSIC_PARTY_CHANCE))
            Acore::Containers::RandomShuffle(roles);
        else
            for (Role& role : roles)
                role = RandomRole();
    }
    else
        for (Role& role : roles)
            role = _layouts[urand(0, uint32(_layouts.size()) - 1)].PlayRole();

    // The class/roles first, then one level they can all be.
    uint8 minLevel = 1;
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        data.Seats[seat].L = seat < data.ActiveSeats ? &PickLayout(roles[seat], DEFAULT_MAX_LEVEL) : nullptr;
        if (data.Seats[seat].L)
            minLevel = std::max(minLevel, data.Seats[seat].L->Assets->Kit->MinLevel());
    }

    uint8 const level = RandomLevel(minLevel);
    Map* map = oldBots[0] ? env.FindMap() : nullptr;

    // The new bots go on idle sessions and into the map before the old ones leave, so the instance always has a
    // bound player.
    std::array<uint8, MAX_SEATS> newSessions{};
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        newSessions[seat] = data.Seats[seat].ActiveSession;

    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
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
    // One agent slot per seat; an empty seat's slot holds no bot.
    env.Bots.clear();
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        env.Bots.push_back(seat < data.ActiveSeats ? SeatBot(data, seat)->GetGUID() : ObjectGuid::Empty);
    env.Targets.clear();

    // Teammates are friends whatever their races; arena opponents are made enemies by StartPvp.
    if (IsParty())
        for (uint32 seat = 1; seat < data.ActiveSeats; ++seat)
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
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            StartSeatPack(SeatBot(data, seat), data.Seats[seat]);
        return SpawnPull(env, map);
    }

    // The duel.
    Seat& seat = data.Seats[0];
    data.OpponentEntry = DuelArena::OpponentPool::Instance().Random(seat.Level);
    Creature* opponent = data.OpponentEntry ? DuelArena::SpawnOpponent(lead, map, data.OpponentEntry) : nullptr;
    if (!opponent)
        return false;

    env.Targets = { opponent->GetGUID() };
    StartDuel(lead, seat);
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
    seat.DamageScale = AnimusForge::ClassRole::DamageScale(level);

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
    assets.Gear->Equip(bot, spec, HasPvp());

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

AnimusForge::SeatView AnimusForge::ClassRoleScenario::ViewSeat(Env const& env, uint32 seatIndex, Player* bot,
    Unit* target) const
{
    EnvData const& data = _data[env.Index];
    Seat const& seat = data.Seats[seatIndex];

    SeatView view;
    view.L = seat.L;
    view.Bot = bot;
    view.Target = target;
    view.Level = seat.Level;
    view.Race = seat.Race;
    view.Spec = seat.Spec;
    view.Build = &seat.Build;
    view.LastStepDamage = seat.LastStepDamage;
    view.LastStepPowerDelta = seat.LastStepPowerDelta;
    view.LastStepDamageTaken = seat.LastStepDamageTaken;
    view.CombatTime = seat.InCombat
        ? std::min(1.0f, float(env.EpisodeElapsedMs - seat.CombatStartMs) / float(MAX_COMBAT_TIME_MS)) : 0.0f;

    view.StableCount = uint32(std::min<std::size_t>(seat.Stable.size(), STABLE_SLOTS));
    std::copy_n(seat.Stable.begin(), view.StableCount, view.Stable.begin());

    view.EnemyCount = uint32(std::min<std::size_t>(env.Targets.size(), PACK_SLOTS));
    for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
        view.Enemies[slot] = env.FindTargetUnit(slot);
    view.TargetSlot = seat.TargetSlot;

    if (HasGauntlet())
        ViewPull(env, view);
    view.FoodItem = seat.FoodItem;
    view.DrinkItem = seat.DrinkItem;

    if (HasCompanion())
        view.Owner = FindOwner(data);

    // Only the party stage has teammates: the PvP stages keep the party block but fight alone (an arena's other seat
    // is the enemy).
    if (IsParty())
    {
        for (uint32 slot = 0; slot < PARTY_MEMBERS; ++slot)
        {
            uint32 const teammateSeat = TeammateSeat(seatIndex, slot);
            if (teammateSeat >= _seatCount || !data.Seats[teammateSeat].L)
                continue;

            Layout const& other = *data.Seats[teammateSeat].L;
            view.Teammates[slot] = { env.FindBot(teammateSeat), other.PlayRole(), other.Profile->Class };
        }

        view.Tank = PartyTank(data);
    }

    if (HasPvp())
    {
        Seat const* other = IsArena() ? &data.Seats[1 - seatIndex] : nullptr;
        view.Opponent = FindOpponent(env, seatIndex);
        view.OpponentClass = other && other->L ? other->L->Profile->Class : data.OpponentClass;
        view.OpponentRole = other && other->L ? other->L->PlayRole() : data.OpponentRole;
        view.Mirror = IsArena();
    }

    return view;
}

void AnimusForge::ClassRoleScenario::ApplySeatAction(Env& env, uint32 seatIndex, int32 action)
{
    Player* bot = env.FindBot(seatIndex);
    if (!bot)
        return;

    Seat& seat = _data[env.Index].Seats[seatIndex];
    Unit* target = CurrentTarget(env, seatIndex);

    // Only the gauntlet has moments without a target (between pulls).
    if (!target && !HasGauntlet())
        return;

    SeatView view = ViewSeat(env, seatIndex, bot, target);
    SeatActionResult result;
    SeatEncoder::Apply(view, action, result);

    seat.TargetSlot = view.TargetSlot;
    seat.SpellCasts += result.SpellCasts;
    seat.TrinketUses += result.TrinketUses;
    seat.SustainCasts += result.SustainCasts;
    seat.FoodUsed += result.FoodUsed;
    seat.DrinkUsed += result.DrinkUsed;

    if (result.StealthOpener)
    {
        seat.StepStealthOpener = true;
        ++seat.StealthOpeners;
    }

    if (!result.PendingInterrupt.IsEmpty())
        seat.PendingInterrupt = result.PendingInterrupt;

    if (result.CallBeast && CallHunterBeast(bot, result.CallBeast))
        SeatEncoder::StartCallBeastCooldown(bot);
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

    if (!_data[env.Index].Seats[seatIndex].L)
        return;

    Player* bot = env.FindBot(seatIndex);
    Unit* target = CurrentTarget(env, seatIndex);      // may be null between gauntlet pulls
    TrackCombat(env, _data[env.Index].Seats[seatIndex], bot);
    SeatEncoder::Observe(ViewSeat(env, seatIndex, bot, target), obs, mask);
}

void AnimusForge::ClassRoleScenario::TrackCombat(Env const& env, Seat& seat, Player* bot) const
{
    bool const inCombat = bot && bot->IsAlive() && bot->IsInCombat();
    if (inCombat && !seat.InCombat)
        seat.CombatStartMs = env.EpisodeElapsedMs;
    seat.InCombat = inCombat;
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
    if (!seat.L)
        return 0.0f;        // an empty party seat

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
        case ArenaMode::Base:      break;
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
    info[INFO_PRESENT] = seat.L ? 1.0f : 0.0f;

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
        "unspent_talent_points", "equipped_items", "spell_casts", "trinket_uses", "class", "role", "present" };

    if (HasDuel())
        names.insert(names.end(), { "killed", "died", "time_to_kill", "damage_taken", "health_left",
            "stealth_openers", "pet_summoned", "opponent", "casts_completed", "casts_cancelled",
            "cast_seconds_wasted", "cancelled_stopped", "cancelled_moved", "cancelled_target", "cancelled_other" });

    if (HasPack())
        names.insert(names.end(), { "kills", "interrupts", "pack_size", "linked" });

    if (HasGauntlet())
        names.insert(names.end(), { "pulls_cleared", "food_used", "drink_used", "sustain_casts", "deaths" });

    if (HasCompanion())
        names.insert(names.end(), { "owner_class", "owner_died", "owner_damage_taken", "owner_healing",
            "threat_on_bot", "threat_on_owner", "owner_role", "owner_deaths", "wipes" });

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
    // Smoke-test baselines. "greedy": the first usable spell or trinket in catalog order. "fight": also start
    // attacking, run to the target, eat or drink between gauntlet pulls, and heal hurt allies.
    if (policy != "greedy" && policy != "fight")
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
