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
#include "Baselines.h"
#include "BotAccounts.h"
#include "Containers.h"
#include "Creature.h"
#include "EncoderSupport.h"
#include "Encounters.h"
#include "Env.h"
#include "ForgeConfig.h"
#include "JsonWriter.h"
#include "Log.h"
#include "Map.h"
#include "Opponents.h"
#include "Player.h"
#include "Random.h"
#include "SeatEncoder.h"
#include "StageDefinition.h"
#include "StringFormat.h"
#include "Supplies.h"
#include "TrainingDummy.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace
{
    using namespace AnimusForge::ClassRole;

    enum ClassRoleSpells : uint32
    {
        SPELL_BATTLE_STANCE     = 2457,
        SPELL_DEFENSIVE_STANCE  = 71,
    };

    constexpr float PARTY_SPACING = 3.0f;
    constexpr uint32 WORLD_TICK_MS = 50;            // the sim's fixed world tick
    constexpr float MAX_COMBAT_TIME_MS = 60000.0f;
    constexpr float POSITION_SCALE = 40.0f;

    /// Version of stage.json.
    constexpr uint32 STAGE_FILE_FORMAT = 1;

    uint8 RandomLevel(uint8 minLevel, ClassRoleTuning::CharacterTuning const& tuning)
    {
        uint32 const highFirst = std::clamp<uint32>(tuning.HighLevelFirst, 1, DEFAULT_MAX_LEVEL);
        if (minLevel <= highFirst && roll_chance_i(tuning.HighLevelChance))
            return uint8(urand(highFirst, DEFAULT_MAX_LEVEL));
        return uint8(urand(minLevel, DEFAULT_MAX_LEVEL));
    }

    Role RandomRole(ClassRoleTuning::PartyTuning const& tuning)
    {
        int32 const roll = irand(0, 99);
        int32 const dps = 100 - tuning.RoleTankChance - tuning.RoleHealerChance;
        return roll < dps ? Role::Dps : roll < dps + tuning.RoleTankChance ? Role::Tank : Role::Heal;
    }

    /// How many party seats get a character, drawn from the size weights.
    uint32 RandomPartySize(ClassRoleTuning::PartyTuning const& tuning)
    {
        std::array<int32, MAX_SEATS> const weights =
            { tuning.SizeWeight1, tuning.SizeWeight2, tuning.SizeWeight3, tuning.SizeWeight4 };

        int32 total = 0;
        for (int32 weight : weights)
            total += weight;
        if (total <= 0)
            return MAX_SEATS;

        int32 roll = irand(0, total - 1);
        for (uint32 size = 1; size <= MAX_SEATS; ++size)
        {
            if (roll < weights[size - 1])
                return size;
            roll -= weights[size - 1];
        }

        return MAX_SEATS;
    }

    float Relative(float coordinate, float origin)
    {
        return std::clamp((coordinate - origin) / POSITION_SCALE, -2.0f, 2.0f);
    }

    float OtherPower(Unit const* unit)
    {
        Powers const power = unit->getPowerType();
        if (power == POWER_MANA)
            return 0.0f;

        uint32 const maxPower = unit->GetMaxPower(power);
        return maxPower ? float(unit->GetPower(power)) / float(maxPower) : 0.0f;
    }

    /// Write `content` to `path` unless the file already holds exactly that: manifests are rebuilt at every start but
    /// rarely change.
    bool WriteIfChanged(std::filesystem::path const& path, std::string const& content)
    {
        std::error_code error;
        if (std::filesystem::file_size(path, error) == content.size() && !error)
        {
            std::ifstream existing(path, std::ios::binary);
            std::string const current((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (current == content)
                return true;
        }

        std::filesystem::path const partial = path.string() + ".partial";
        {
            std::ofstream file(partial, std::ios::binary | std::ios::trunc);
            file << content;
            if (!file)
                return false;
        }

        std::filesystem::rename(partial, path, error);
        return !error;
    }
}

AnimusForge::ClassRole::ClassRoleScenario::ClassRoleScenario(ForgeConfig const& config, StageDefinition const& stage)
    : _stage(stage), _tuning(ClassRoleTuning::Load()), _spawnMapId(config.SpawnMapId),
    _spawnPoint(config.SpawnPosition), _seatCount(stage.SeatCount()),
    _decisionScale(float(config.DecisionTicks * WORLD_TICK_MS) / 50.0f)
{
    // The class/roles this run plays: AnimusForge.ClassRoles, or all of them.
    for (ClassRoleProfile const& profile : ClassRoleProfiles())
    {
        if (!config.ClassRoles.empty()
            && std::find(config.ClassRoles.begin(), config.ClassRoles.end(), profile.Name) == config.ClassRoles.end())
            continue;

        if (ClassRoleAssets::For(profile).Races.empty())
            continue;

        Layout layout = Layout::Build(profile, _stage);
        layout.Index = uint16(_layouts.size());
        _layouts.push_back(std::move(layout));
    }

    _spec.AgentsPerEnv = _seatCount;
    for (Layout const& layout : _layouts)
    {
        _spec.ObsDim = std::max(_spec.ObsDim, layout.ObsDim);
        _spec.NumActions = std::max(_spec.NumActions, layout.NumActions);
        _spec.Layouts.push_back(LayoutSpec{ layout.Profile->Name, layout.ObsDim, layout.NumActions });
    }

    _spec.StateDim = STATE_GLOBAL_COUNT + MAX_SEATS * STATE_SEAT_FEATURES + PACK_SLOTS * STATE_ENEMY_FEATURES;
    _data.resize(config.Envs);

    // The stage's encounters, in build order.
    uint32 const envs = config.Envs;
    OpponentEncounter* opponent = nullptr;
    PullsEncounter* pulls = nullptr;
    CreatureEncounter* creature = nullptr;

    auto const add = [this](auto encounter)
    {
        auto* raw = encounter.get();
        _encounters.push_back(std::move(encounter));
        return raw;
    };

    if (_stage.Against == Opposition::ScriptedPlayer || _stage.Against == Opposition::MirrorSeat)
        opponent = add(std::make_unique<OpponentEncounter>(*this, envs, _stage.Against == Opposition::MirrorSeat));
    if (_stage.Owner)
        _owner = add(std::make_unique<OwnerEncounter>(*this, envs));
    if (_stage.PartyGroup)
        _party = add(std::make_unique<PartyEncounter>(*this, envs));
    if (_stage.Against == Opposition::Pulls)
        pulls = add(std::make_unique<PullsEncounter>(*this, envs));
    if (_stage.Against == Opposition::Creature)
        creature = add(std::make_unique<CreatureEncounter>(*this));

    // Pulls record the step's damage taken before the owner's tank refund reads it.
    for (Encounter* encounter : std::initializer_list<Encounter*>{ creature, pulls, _owner, _party, opponent })
        if (encounter)
            _rewardOrder.push_back(encounter);

    if (_stage.Against == Opposition::Creature || _stage.Against == Opposition::Pulls)
        Opponents::OpponentPool::Instance();    // load it at startup rather than on the first episode
    ConsumablePool::Instance();

    AddCoreEpisodeInfo();
    for (Encounter* encounter : _rewardOrder)
        encounter->AddEpisodeInfo(_info);

    // What the seats are paid for, term by term.
    for (Encounter* encounter : _rewardOrder)
    {
        for (RewardTerm term : encounter->RewardTerms())
        {
            std::string name = "reward_" + std::string(RewardTermName(term));
            if (_info.Contains(name))
                continue;

            _info.Add(std::move(name), [this, term](Env const& env, uint32 seat)
            {
                return Data(env).Seats[seat].Rewards.Episode(term);
            });
        }
    }

    _spec.EpisodeInfoDim = _info.Size();

    WriteStageFiles(config);

    LOG_INFO("module.animus", "{}: {} seats per env, {} class/role layouts (obs up to {}, actions up to {}), state {}",
        Name(), _seatCount, _layouts.size(), _spec.ObsDim, _spec.NumActions, _spec.StateDim);
}

AnimusForge::ClassRole::ClassRoleScenario::~ClassRoleScenario() = default;

char const* AnimusForge::ClassRole::ClassRoleScenario::Name() const
{
    return _stage.Name.c_str();
}

void AnimusForge::ClassRole::ClassRoleScenario::AddCoreEpisodeInfo()
{
    auto const seat = [this](Env const& env, uint32 index) -> SeatState const& { return Data(env).Seats[index]; };

    _info.Add("damage", [](Env const& env, uint32 index) { return float(env.EpisodeStats[index].Damage); });
    _info.Add("dps", [](Env const& env, uint32 index)
    {
        float const seconds = std::max(0.001f, float(env.EpisodeElapsedMs) / 1000.0f);
        return float(env.EpisodeStats[index].Damage) / seconds;
    });
    _info.Add("white_damage", [](Env const& env, uint32 index) { return float(env.EpisodeStats[index].WhiteDamage); });
    _info.Add("special_damage", [](Env const& env, uint32 index)
    {
        return float(env.EpisodeStats[index].SpecialDamage);
    });
    _info.Add("level", [seat](Env const& env, uint32 index) { return float(seat(env, index).Level); });
    _info.Add("race", [seat](Env const& env, uint32 index) { return float(seat(env, index).Race); });
    _info.Add("spec", [seat](Env const& env, uint32 index) { return float(seat(env, index).Spec); });
    _info.Add("unspent_talent_points", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).UnspentTalentPoints);
    });
    _info.Add("equipped_items", [seat](Env const& env, uint32 index) { return float(seat(env, index).EquippedItems); });
    _info.Add("spell_casts", [seat](Env const& env, uint32 index) { return float(seat(env, index).SpellCasts); });
    _info.Add("trinket_uses", [seat](Env const& env, uint32 index) { return float(seat(env, index).TrinketUses); });
    _info.Add("class", [seat](Env const& env, uint32 index)
    {
        Layout const* layout = seat(env, index).L;
        return layout ? float(layout->Profile->Class) : 0.0f;
    });
    _info.Add("role", [seat](Env const& env, uint32 index)
    {
        Layout const* layout = seat(env, index).L;
        return layout ? float(uint32(layout->PlayRole())) : 0.0f;
    });
    // A party seat left empty this episode reports 0: ignore its row.
    _info.Add("present", [seat](Env const& env, uint32 index) { return seat(env, index).L ? 1.0f : 0.0f; });

    // Fights against something that fights back.
    auto const tally = [this](Env const& env, uint32 index) -> CombatTally const&
    {
        return Data(env).Seats[index].Combat;
    };

    _info.Add("killed", [tally](Env const& env, uint32 index) { return tally(env, index).Killed ? 1.0f : 0.0f; });
    _info.Add("died", [tally](Env const& env, uint32 index) { return tally(env, index).Died ? 1.0f : 0.0f; });
    _info.Add("time_to_kill", [tally](Env const& env, uint32 index)
    {
        CombatTally const& combat = tally(env, index);
        return float(combat.Killed ? combat.KillTimeMs : env.EpisodeElapsedMs) / 1000.0f;
    });
    _info.Add("damage_taken", [tally](Env const& env, uint32 index) { return float(tally(env, index).DamageTaken); });
    _info.Add("health_left", [](Env const& env, uint32 index)
    {
        Player* bot = env.FindBot(index);
        return bot ? bot->GetHealthPct() / 100.0f : 0.0f;
    });
    _info.Add("stealth_openers", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).StealthOpeners);
    });
    _info.Add("pet_summoned", [tally](Env const& env, uint32 index)
    {
        return tally(env, index).PetSummoned ? 1.0f : 0.0f;
    });
    _info.Add("opponent", [this](Env const& env, uint32) { return float(Data(env).OpponentEntry); });
    _info.Add("casts_completed", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).CastsCompleted);
    });
    _info.Add("casts_cancelled", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).CastsCancelled);
    });
    _info.Add("cast_seconds_wasted", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).CastMsWasted) / 1000.0f;
    });
    _info.Add("cancelled_stopped", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).CastsStopped);
    });
    _info.Add("cancelled_moved", [tally](Env const& env, uint32 index) { return float(tally(env, index).CastsMoved); });
    _info.Add("cancelled_target", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).CastsTargetLost);
    });
    _info.Add("cancelled_other", [tally](Env const& env, uint32 index) { return float(tally(env, index).CastsOther); });
    _info.Add("consumables_used", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).ConsumablesUsed);
    });
    _info.Add("self_resurrections", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).SelfResurrections);
    });
}

void AnimusForge::ClassRole::ClassRoleScenario::WriteStageFiles(ForgeConfig const& config) const
{
    // Each layout's manifest, for export to publish beside its model, and the stage's description: its blocks, what
    // it extends (the learner's seed chain), its models and the effective tuning (copied into every run).
    std::filesystem::path const directory = std::filesystem::path(config.LayoutsDir()) / _stage.Name;
    std::error_code error;
    std::filesystem::create_directories(directory, error);

    for (Layout const& layout : _layouts)
        if (!WriteIfChanged(directory / (layout.ModelName() + ".json"), layout.Manifest()))
            LOG_WARN("module.animus", "{}: could not write the {} layout manifest to {}", Name(), layout.ModelName(),
                directory.string());

    JsonWriter json;
    json.BeginObject()
        .Key("format").Value(STAGE_FILE_FORMAT)
        .Key("stage").Value(_stage.Name)
        .Key("suffix").Value(_stage.Suffix)
        .Key("extends").Value(_stage.Extends)
        .Key("summary").Value(_stage.Summary)
        .Key("seats").Value(_seatCount)
        .Key("blocks").Array(_stage.Blocks, [](JsonWriter& out, BlockId id) { out.Value(BlockName(id)); });

    // The stages a run seeds from, closest first: the learner takes the first one that has been trained.
    json.Key("seed_chain").BeginArray();
    for (StageDefinition const* base = FindStage(_stage.Extends); base; base = FindStage(base->Extends))
        json.Value(base->Name);
    json.EndArray();

    json.Key("models").BeginObject();
    for (Layout const& layout : _layouts)
        json.Key(layout.Profile->Name).Value(layout.ModelName());
    json.EndObject();

    // Where each block sits in each layout: a later stage seeds its networks block by block from these.
    json.Key("layouts").BeginObject();
    for (Layout const& layout : _layouts)
    {
        json.Key(layout.Profile->Name).BeginObject()
            .Key("obs_dim").Value(layout.ObsDim)
            .Key("num_actions").Value(layout.NumActions)
            .Key("blocks").BeginArray();
        for (BlockId id : layout.Blocks)
        {
            BlockSlice const& slice = layout.Slice(id);
            json.BeginObject()
                .Key("name").Value(BlockName(id))
                .Key("obs").Span(slice.ObsFirst, slice.ObsCount)
                .Key("actions").Span(slice.ActionFirst, slice.ActionCount)
                .EndObject();
        }
        json.EndArray().EndObject();
    }
    json.EndObject();

    json.Key("episode_info").Array(_info.Names(), [](JsonWriter& out, std::string const& name) { out.Value(name); });
    json.Key("tuning").Raw(_tuning.Json());
    json.EndObject();

    if (!WriteIfChanged(directory / "stage.json", json.Str()))
        LOG_WARN("module.animus", "{}: could not write stage.json to {}", Name(), directory.string());
}

AnimusForge::ClassRole::EnvState& AnimusForge::ClassRole::ClassRoleScenario::Data(Env const& env)
{
    return _data[env.Index];
}

AnimusForge::ClassRole::EnvState const& AnimusForge::ClassRole::ClassRoleScenario::Data(Env const& env) const
{
    return _data[env.Index];
}

Player* AnimusForge::ClassRole::ClassRoleScenario::SeatBot(Env const& env, uint32 seat) const
{
    return _data[env.Index].Seats[seat].Bot.Active();
}

Player* AnimusForge::ClassRole::ClassRoleScenario::Owner(Env const& env) const
{
    return _owner ? _owner->Find(env) : nullptr;
}

Player* AnimusForge::ClassRole::ClassRoleScenario::PartyTank(Env const& env) const
{
    return _party ? _party->Tank(env) : nullptr;
}

AnimusForge::ClassRole::Layout const& AnimusForge::ClassRole::ClassRoleScenario::PickLayout(Role role,
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

bool AnimusForge::ClassRole::ClassRoleScenario::IsTerminal(Env const& env) const
{
    return std::any_of(_encounters.begin(), _encounters.end(),
        [&env](std::unique_ptr<Encounter> const& encounter) { return encounter->IsTerminal(env); });
}

bool AnimusForge::ClassRole::ClassRoleScenario::Setup(Env& env)
{
    if (_layouts.empty())
    {
        LOG_ERROR("module.animus", "{}: no class/role to play (check AnimusForge.ClassRoles)", Name());
        return false;
    }

    if (!Rebuild(env))
        return false;

    Data(env).Fresh = true;
    return true;
}

void AnimusForge::ClassRole::ClassRoleScenario::Reset(Env& env)
{
    // Setup already built the first episode's characters.
    EnvState& data = Data(env);
    if (data.Fresh)
    {
        data.Fresh = false;
        return;
    }

    if (!Rebuild(env))
        LOG_ERROR("module.animus", "{}: env {} could not build new characters; it keeps the old ones", Name(),
            env.Index);
}

bool AnimusForge::ClassRole::ClassRoleScenario::Rebuild(Env& env)
{
    EnvState& data = Data(env);

    // A new episode starts from clean totals.
    for (SeatState& seat : data.Seats)
        seat.ResetEpisode();
    for (auto const& encounter : _encounters)
        encounter->ResetEpisode(env);

    std::vector<Creature*> oldTargets;
    for (uint32 target = 0; target < env.Targets.size(); ++target)
        if (Creature* creature = env.FindTarget(target))
            oldTargets.push_back(creature);

    for (auto const& encounter : _encounters)
        encounter->BeforeRebuild(env);

    bool const firstBuild = !SeatBot(env, 0);
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        data.Seats[seat].Bot.Begin();

    // How many seats play this episode: every seat, except in a party, which has 1-4 like a player's companions. The
    // rest stay empty: no character, no layout, only the no-op allowed.
    data.ActiveSeats = _seatCount;
    std::array<Role, MAX_SEATS> roles{};
    if (_stage.Seats == SeatPlan::Party)
    {
        data.ActiveSeats = RandomPartySize(_tuning.Party);

        // Some parties are the classic makeup (as many of a tank, a healer and two damage dealers as there are seats,
        // in a random order); the rest draw every seat's role on its own.
        roles = { Role::Tank, Role::Heal, Role::Dps, Role::Dps };
        if (roll_chance_i(_tuning.Party.ClassicChance))
            Acore::Containers::RandomShuffle(roles);
        else
            for (Role& role : roles)
                role = RandomRole(_tuning.Party);
    }
    else
        for (uint32 seat = 0; seat < _seatCount; ++seat)
            roles[seat] = _layouts[urand(0, uint32(_layouts.size()) - 1)].PlayRole();

    // The class/roles first, then one level they can all be.
    uint8 minLevel = 1;
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        data.Seats[seat].L = seat < data.ActiveSeats ? &PickLayout(roles[seat], DEFAULT_MAX_LEVEL) : nullptr;
        if (data.Seats[seat].L)
            minLevel = std::max(minLevel, data.Seats[seat].L->Assets->Kit->MinLevel());
    }

    uint8 const level = RandomLevel(minLevel, _tuning.Characters);
    Map* map = firstBuild ? nullptr : env.FindMap();

    // The new bots go on idle sessions and into the map before the old ones leave, so the instance always has a
    // bound player.
    Player* firstNew = nullptr;
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
    {
        Position start = _spawnPoint;
        if (_stage.Seats == SeatPlan::Party)
        {
            start.m_positionX += (seat % 2 ? -PARTY_SPACING : PARTY_SPACING) * float(1 + seat / 2);
            start.m_positionY += (seat % 2 ? PARTY_SPACING : -PARTY_SPACING);
        }
        else if (_stage.Seats == SeatPlan::Mirror && seat == 1 && firstNew)
        {
            // Out of range of the first seat's new bot, at a random bearing, facing a random way.
            start = Opponents::FindSpawnPoint(firstNew, map);
            start.SetOrientation(frand(0.0f, 2.0f * float(M_PI)));
        }

        Player* bot = BuildSeat(env, seat, map, level, start);
        if (!bot)
            return false;

        if (seat == 0)
            firstNew = bot;
    }

    for (Creature* creature : oldTargets)
        creature->DespawnOrUnsummon();

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        data.Seats[seat].Bot.Promote();

    Player* lead = SeatBot(env, 0);
    if (firstBuild)
        TrainingDummy::ClearSpawnArea(lead);

    env.MapId = map->GetId();
    env.InstanceId = map->GetInstanceId();
    // One agent slot per seat; an empty seat's slot holds no bot.
    env.Bots.clear();
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        env.Bots.push_back(seat < data.ActiveSeats ? SeatBot(env, seat)->GetGUID() : ObjectGuid::Empty);
    env.Targets.clear();

    for (auto const& encounter : _encounters)
        if (!encounter->Build(env, map, level))
            return false;

    StockSeats(env);
    return true;
}

Player* AnimusForge::ClassRole::ClassRoleScenario::BuildSeat(Env& env, uint32 seatIndex, Map*& map, uint8 level,
    Position const& start)
{
    SeatState& seat = Data(env).Seats[seatIndex];
    Layout const& layout = *seat.L;

    seat.Race = layout.Assets->Races[urand(0, uint32(layout.Assets->Races.size()) - 1)];
    seat.Level = level;
    seat.Spec = uint8(urand(0, uint32(layout.Profile->Specs.size()) - 1));
    seat.DamageScale = DamageScale(level);

    uint8 const session = seat.Bot.NextSession();

    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Forge{}s{}{}", env.Index, seatIndex, session ? "b" : "a");
    spec.Race = seat.Race;
    spec.Class = layout.Profile->Class;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = level;
    spec.AccountId = BotAccounts::Seat(env.Index, seatIndex, session);

    Player* bot = seat.Bot.CreateNext(spec, map, _spawnMapId, start);
    if (!bot)
        return nullptr;

    // Talent points depend on the map for death knights (Ebon Hold, where Create put the bot, only counts
    // quest-rewarded points); recompute them on the spawn map.
    bot->InitTalentForLevel();
    Configure(bot, seat);
    return bot;
}

void AnimusForge::ClassRole::ClassRoleScenario::Configure(Player* bot, SeatState& seat) const
{
    ClassRoleAssets const& assets = *seat.L->Assets;
    SpecProfile const& spec = seat.L->Profile->Specs[seat.Spec];

    GearBuilder::LearnProficiencies(bot);

    seat.Build = assets.Talents->Standard(spec.Name, spec.TabPage, bot->GetFreeTalentPoints());
    seat.UnspentTalentPoints = assets.Talents->Apply(bot, seat.Build);

    assets.Kit->Learn(bot);
    assets.Talents->ApplyGlyphs(bot, spec.Name);
    assets.Gear->Equip(bot, spec, _stage.Has(BlockId::Pvp));

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

void AnimusForge::ClassRole::ClassRoleScenario::PrepareFighter(Player* bot, SeatState& seat) const
{
    // Levelling up mid-episode would change the character under the policy.
    bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);

    seat.Stable = seat.L->Profile->Class == CLASS_HUNTER
        ? StablePool::Instance().Random(STABLE_SLOTS) : std::vector<uint32>();

    // A warrior has no stance until one is cast (a first login casts it), and nothing works without one.
    if (seat.L->Profile->Class == CLASS_WARRIOR)
        bot->CastSpell(bot, seat.L->PlayRole() == Role::Tank && bot->HasSpell(SPELL_DEFENSIVE_STANCE)
            ? SPELL_DEFENSIVE_STANCE : SPELL_BATTLE_STANCE, true);
}

void AnimusForge::ClassRole::ClassRoleScenario::StockSeats(Env& env)
{
    EnvState& data = Data(env);

    // Warlocks hand out healthstones to the party they are in.
    Player* owner = Owner(env);
    bool warlockInParty = owner && owner->getClass() == CLASS_WARLOCK;
    if (_stage.PartyGroup)
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            if (data.Seats[seat].L && data.Seats[seat].L->Profile->Class == CLASS_WARLOCK)
                warlockInParty = true;

    ConsumablePool const& pool = ConsumablePool::Instance();
    for (uint32 seatIndex = 0; seatIndex < data.ActiveSeats; ++seatIndex)
    {
        SeatState& seat = data.Seats[seatIndex];
        Player* bot = SeatBot(env, seatIndex);
        if (!bot || !seat.L)
            continue;

        seat.Supplies = pool.Supplies(seat.Level, bot->GetMaxPower(POWER_MANA) > 0,
            seat.L->Profile->Class == CLASS_WARLOCK, warlockInParty);
        StockBattleSupplies(bot, seat.Supplies, seat.L->Profile->Specs[seat.Spec].Stats);
    }
}

void AnimusForge::ClassRole::ClassRoleScenario::AcceptResurrections(Env& env)
{
    EnvState& data = Data(env);

    std::vector<Player*> players;
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        players.push_back(SeatBot(env, seat));
    players.push_back(Owner(env));

    for (Player* player : players)
    {
        if (!player || player->IsAlive() || !player->isResurrectRequested())
            continue;

        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        {
            if (Player* reviver = SeatBot(env, seat); reviver && player->isResurrectRequestedBy(reviver->GetGUID()))
            {
                data.Seats[seat].StepRevivedAlly = true;
                ++data.Seats[seat].Revives;
            }
        }

        player->ResurectUsingRequestData();
    }
}

bool AnimusForge::ClassRole::ClassRoleScenario::DeadForGood(Env const& env, uint32 seatIndex) const
{
    CombatTally const& tally = Data(env).Seats[seatIndex].Combat;
    Player* bot = env.FindBot(seatIndex);
    if (!tally.Died || (bot && bot->IsAlive()))
        return false;

    bool const canResurrect = bot && !_stage.Has(BlockId::Pvp) && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL);
    return !canResurrect || env.EpisodeElapsedMs >= tally.DeathMs + _tuning.Resurrection.GraceMs;
}

bool AnimusForge::ClassRole::ClassRoleScenario::SeatCanResurrect(Env const& env, uint32 seatIndex) const
{
    SeatState const& seat = Data(env).Seats[seatIndex];
    Player* bot = SeatBot(env, seatIndex);
    if (!bot || !bot->IsAlive() || !seat.L)
        return false;

    std::vector<ActionCatalog::Action> const& revives = seat.L->AllyRevives;
    return std::any_of(revives.begin(), revives.end(), [bot](ActionCatalog::Action const& revive)
    {
        return revive.Type == ActionCatalog::Kind::Spell && ActionCatalog::KnownRank(bot, revive.FirstRank);
    });
}

void AnimusForge::ClassRole::ClassRoleScenario::NotifyRecovered(Env& env, int32 who)
{
    if (who >= 0)
        Data(env).Seats[who].Combat.DeathCounted = false;

    for (auto const& encounter : _encounters)
        encounter->OnRecovered(env, who);
}

void AnimusForge::ClassRole::ClassRoleScenario::ApplyActions(Env& env, int32 const* actions)
{
    // Env upkeep first (linked pulls, the owner, the next pull, the scripted opponent), so the targets below are
    // current.
    for (auto const& encounter : _encounters)
        encounter->UpdateEnemies(env);
    for (auto const& encounter : _encounters)
        encounter->Update(env);

    AcceptResurrections(env);

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        ApplySeatAction(env, seat, actions[seat]);
}

Unit* AnimusForge::ClassRole::ClassRoleScenario::CurrentTarget(Env const& env, uint32 seat)
{
    Unit* target = nullptr;
    for (auto const& encounter : _encounters)
        if (encounter->SelectTarget(env, seat, target))
            return target;

    return env.FindTargetUnit(0);
}

AnimusForge::ClassRole::SeatView AnimusForge::ClassRole::ClassRoleScenario::ViewSeat(Env const& env,
    uint32 seatIndex, Player* bot, Unit* target) const
{
    SeatState const& seat = Data(env).Seats[seatIndex];

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
        ? std::min(1.0f, float(env.EpisodeElapsedMs - seat.CombatStartMs) / MAX_COMBAT_TIME_MS) : 0.0f;
    view.Supplies = seat.Supplies;
    view.SelfResurrectAllowed = !_stage.Has(BlockId::Pvp);

    view.StableCount = uint32(std::min<std::size_t>(seat.Stable.size(), STABLE_SLOTS));
    std::copy_n(seat.Stable.begin(), view.StableCount, view.Stable.begin());

    view.EnemyCount = uint32(std::min<std::size_t>(env.Targets.size(), PACK_SLOTS));
    for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
        view.Enemies[slot] = env.FindTargetUnit(slot);
    view.TargetSlot = seat.TargetSlot;

    for (auto const& encounter : _encounters)
        encounter->View(env, seatIndex, view);

    return view;
}

void AnimusForge::ClassRole::ClassRoleScenario::ApplySeatAction(Env& env, uint32 seatIndex, int32 action)
{
    Player* bot = env.FindBot(seatIndex);
    SeatState& seat = Data(env).Seats[seatIndex];
    if (!bot || !seat.L)
        return;

    Unit* target = CurrentTarget(env, seatIndex);
    if (!target && !SeatEncoder::ActsWithoutTarget(*seat.L))
        return;

    for (auto const& encounter : _encounters)
        encounter->BeforeSeatAction(env, seatIndex, target);

    SeatView view = ViewSeat(env, seatIndex, bot, target);
    SeatActionResult result;
    SeatEncoder::Apply(view, action, result);

    seat.TargetSlot = view.TargetSlot;
    seat.SpellCasts += result.SpellCasts;
    seat.TrinketUses += result.TrinketUses;
    seat.ConsumablesUsed += result.ConsumablesUsed;
    seat.SelfResurrections += result.SelfResurrected ? 1 : 0;

    if (result.StealthOpener)
    {
        seat.Combat.StepStealthOpener = true;
        ++seat.Combat.StealthOpeners;
    }

    for (auto const& encounter : _encounters)
        encounter->OnSeatAction(env, seatIndex, result);

    if (result.CallBeast && CallHunterBeast(bot, result.CallBeast))
        Encoding::StartCallBeastCooldown(bot);
}

void AnimusForge::ClassRole::ClassRoleScenario::Observe(Env& env, float* obs, float* state, uint8* mask)
{
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        ObserveSeat(env, seat, obs + seat * _spec.ObsDim, mask + seat * _spec.NumActions);

    WriteState(env, state);
}

void AnimusForge::ClassRole::ClassRoleScenario::AgentLayouts(Env const& env, uint16* layout) const
{
    EnvState const& data = Data(env);
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        layout[seat] = data.Seats[seat].L ? data.Seats[seat].L->Index : 0;
}

void AnimusForge::ClassRole::ClassRoleScenario::ObserveSeat(Env& env, uint32 seatIndex, float* obs, uint8* mask)
{
    std::fill(obs, obs + _spec.ObsDim, 0.0f);
    std::fill(mask, mask + _spec.NumActions, 0);
    mask[0] = 1;

    SeatState& seat = Data(env).Seats[seatIndex];
    if (!seat.L)
        return;

    Player* bot = env.FindBot(seatIndex);
    Unit* target = CurrentTarget(env, seatIndex);      // may be null between gauntlet pulls

    // Note when the bot entered or left combat (SeatView::CombatTime).
    bool const inCombat = bot && bot->IsAlive() && bot->IsInCombat();
    if (inCombat && !seat.InCombat)
        seat.CombatStartMs = env.EpisodeElapsedMs;
    seat.InCombat = inCombat;

    SeatEncoder::Observe(ViewSeat(env, seatIndex, bot, target), obs, mask);
}

void AnimusForge::ClassRole::ClassRoleScenario::Reward(Env& env, float* reward)
{
    for (Encounter* encounter : _rewardOrder)
        encounter->BeforeRewards(env);

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        reward[seat] = SeatReward(env, seat);

    for (Encounter* encounter : _rewardOrder)
        encounter->AfterRewards(env);
}

float AnimusForge::ClassRole::ClassRoleScenario::SeatReward(Env& env, uint32 seatIndex)
{
    SeatState& seat = Data(env).Seats[seatIndex];
    if (!seat.L)
        return 0.0f;        // an empty party seat

    Player* bot = env.FindBot(seatIndex);
    seat.LastStepDamage = float(env.StepStats[seatIndex].Damage) / seat.DamageScale;

    // Standing again (resurrected, or recovered after a pull): the next death is paid for again.
    if (bot && bot->IsAlive())
        seat.Combat.DeathCounted = false;

    for (Encounter* encounter : _rewardOrder)
        encounter->Reward(env, seatIndex, bot, seat.Rewards);

    if (bot)
    {
        Powers const power = bot->getPowerType();
        uint32 const current = bot->GetPower(power);
        float const maxPower = float(std::max<uint32>(1, bot->GetMaxPower(power)));
        seat.LastStepPowerDelta = (float(current) - float(seat.LastPower)) / maxPower;
        seat.LastPower = current;
    }

    return seat.Rewards.TakeStep();
}

void AnimusForge::ClassRole::ClassRoleScenario::WriteState(Env const& env, float* state) const
{
    std::fill(state, state + _spec.StateDim, 0.0f);

    EnvState const& data = Data(env);
    float const originX = _spawnPoint.GetPositionX();
    float const originY = _spawnPoint.GetPositionY();

    state[STATE_EPISODE_TIME] = env.EpisodeLengthMs
        ? std::min(1.0f, float(env.EpisodeElapsedMs) / float(env.EpisodeLengthMs)) : 0.0f;

    for (auto const& encounter : _encounters)
        encounter->WriteState(env, state);

    std::array<Player*, MAX_SEATS> bots{};
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        Player* bot = env.FindBot(seat);
        SeatState const& slot = data.Seats[seat];
        bots[seat] = bot;
        if (!bot || !slot.L)
            continue;

        float* features = state + STATE_GLOBAL_COUNT + seat * STATE_SEAT_FEATURES;
        features[STATE_SEAT_PRESENT] = 1.0f;
        features[STATE_SEAT_ALIVE] = bot->IsAlive() ? 1.0f : 0.0f;
        features[STATE_SEAT_HEALTH] = bot->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = bot->GetMaxPower(POWER_MANA))
            features[STATE_SEAT_MANA] = float(bot->GetPower(POWER_MANA)) / float(maxMana);
        features[STATE_SEAT_OTHER_POWER] = OtherPower(bot);
        features[STATE_SEAT_LEVEL] = float(slot.Level) / float(DEFAULT_MAX_LEVEL);
        features[STATE_SEAT_ROLE_FIRST + uint32(slot.L->PlayRole())] = 1.0f;
        WriteOneHot(PLAYABLE_CLASSES, slot.L->Profile->Class, features + STATE_SEAT_CLASS_FIRST);
        features[STATE_SEAT_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
        features[STATE_SEAT_CASTING] = bot->IsNonMeleeSpellCast(false, false, true) ? 1.0f : 0.0f;
        features[STATE_SEAT_X] = Relative(bot->GetPositionX(), originX);
        features[STATE_SEAT_Y] = Relative(bot->GetPositionY(), originY);
    }

    // The enemies: the env's targets (creatures, or the scripted enemy player); in self-play each seat's opponent is
    // the other seat, already in the seat block.
    Player* owner = Owner(env);
    float const leadLevel = float(data.Seats[0].Level);
    for (uint32 slot = 0; slot < env.Targets.size() && slot < PACK_SLOTS; ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy)
            continue;

        float* features = state + STATE_GLOBAL_COUNT + MAX_SEATS * STATE_SEAT_FEATURES + slot * STATE_ENEMY_FEATURES;
        Unit const* victim = enemy->GetVictim();

        features[STATE_ENEMY_PRESENT] = 1.0f;
        features[STATE_ENEMY_ALIVE] = enemy->IsAlive() ? 1.0f : 0.0f;
        features[STATE_ENEMY_HEALTH] = enemy->GetHealthPct() / 100.0f;
        features[STATE_ENEMY_X] = Relative(enemy->GetPositionX(), originX);
        features[STATE_ENEMY_Y] = Relative(enemy->GetPositionY(), originY);
        features[STATE_ENEMY_CASTING] = enemy->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
        features[STATE_ENEMY_ELITE] = enemy->ToCreature() && enemy->ToCreature()->isElite() ? 1.0f : 0.0f;
        features[STATE_ENEMY_LEVEL_DIFF] = (float(enemy->GetLevel()) - leadLevel) / 5.0f;
        features[STATE_ENEMY_IN_COMBAT] = enemy->IsInCombat() ? 1.0f : 0.0f;
        features[STATE_ENEMY_ON_OWNER] = victim && victim == owner ? 1.0f : 0.0f;
        for (uint32 seat = 0; seat < _seatCount; ++seat)
            if (victim && victim == bots[seat])
                features[STATE_ENEMY_ON_SEAT_FIRST + seat] = 1.0f;
    }
}

void AnimusForge::ClassRole::ClassRoleScenario::EpisodeInfo(Env const& env, float* info) const
{
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        _info.Write(env, seat, info + seat * _spec.EpisodeInfoDim);
}

bool AnimusForge::ClassRole::ClassRoleScenario::ScriptedAction(std::string const& policy, float const* obs,
    uint8 const* mask, uint16 layoutIndex, int32& action) const
{
    if (_layouts.empty() || !Baselines::Supports(policy, _layouts.front()))
        return false;

    action = layoutIndex < _layouts.size() ? Baselines::Choose(policy, _layouts[layoutIndex], obs, mask) : 0;
    return true;
}

void AnimusForge::ClassRole::ClassRoleScenario::Teardown(Env& env)
{
    for (uint32 target = 0; target < env.Targets.size(); ++target)
        if (Creature* creature = env.FindTarget(target))
            creature->DespawnOrUnsummon();

    // In reverse reward order: the party disbands before its owner leaves.
    for (auto encounter = _rewardOrder.rbegin(); encounter != _rewardOrder.rend(); ++encounter)
        (*encounter)->Teardown(env);

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        Data(env).Seats[seat].Bot.Destroy();

    env.Bots.clear();
    env.Targets.clear();
}
