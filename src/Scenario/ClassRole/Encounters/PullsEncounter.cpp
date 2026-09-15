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

#include "Encounters.h"
#include "CombatReward.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Env.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Opponents.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "Supplies.h"
#include <algorithm>

namespace
{
    constexpr uint32 HIGHEST_OPPONENT_LEVEL = 83;
    constexpr float PULL_TIME_SCALE_MS = 60000.0f;      // observation and fast-pull scale
    constexpr float QUIET_TIME_SCALE_MS = 20000.0f;
    constexpr float NEXT_PULL_SCALE_MS = 20000.0f;

    void Despawn(AnimusForge::Env& env)
    {
        for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
            if (Creature* enemy = env.FindTarget(slot))
                enemy->DespawnOrUnsummon();

        env.Targets.clear();
    }
}

AnimusForge::ClassRole::PullsEncounter::PullsEncounter(ClassRoleScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
    // Load it at startup rather than on the first episode.
    Opponents::OpponentPool::Instance();
    if (Gauntlet())
        ConsumablePool::Instance();
}

std::vector<AnimusForge::ClassRole::RewardTerm> AnimusForge::ClassRole::PullsEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::DamageDealt, RewardTerm::DamageTaken, RewardTerm::Casting,
        RewardTerm::Approach, RewardTerm::StealthOpener, RewardTerm::Interrupt, RewardTerm::Kill, RewardTerm::Clear,
        RewardTerm::HealthKept, RewardTerm::Death };
}

void AnimusForge::ClassRole::PullsEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("kills", [this](Env const& env, uint32) { return float(_envs[env.Index].Kills); });
    table.Add("interrupts", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].Interrupts);
    });
    table.Add("pack_size", [this](Env const& env, uint32) { return float(_envs[env.Index].PackSize); });
    table.Add("linked", [this](Env const& env, uint32) { return _envs[env.Index].Linked ? 1.0f : 0.0f; });

    if (Gauntlet())
    {
        table.Add("pulls_cleared", [this](Env const& env, uint32) { return float(_envs[env.Index].PullsCleared); });
        table.Add("food_used", [this](Env const& env, uint32 seat)
        {
            return float(_envs[env.Index].Seats[seat].FoodUsed);
        });
        table.Add("drink_used", [this](Env const& env, uint32 seat)
        {
            return float(_envs[env.Index].Seats[seat].DrinkUsed);
        });
        table.Add("sustain_casts", [this](Env const& env, uint32 seat)
        {
            return float(_envs[env.Index].Seats[seat].SustainCasts);
        });
        table.Add("deaths", [this](Env const& env, uint32 seat)
        {
            return float(_scenario.Data(env).Seats[seat].Combat.Deaths);
        });
    }

    if (_scenario.Stage().Owner)
        table.Add("wipes", [this](Env const& env, uint32) { return float(_envs[env.Index].Wipes); });
}

void AnimusForge::ClassRole::PullsEncounter::ResetEpisode(Env& env)
{
    EnvPulls& pulls = _envs[env.Index];
    std::array<SeatPull, MAX_SEATS> seats = pulls.Seats;
    pulls = EnvPulls();

    // The seats' supplies belong to their characters, which the rebuild replaces anyway.
    for (uint32 seat = 0; seat < MAX_SEATS; ++seat)
    {
        pulls.Seats[seat].FoodItem = seats[seat].FoodItem;
        pulls.Seats[seat].DrinkItem = seats[seat].DrinkItem;
    }
}

bool AnimusForge::ClassRole::PullsEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);
    EnvPulls& pulls = _envs[env.Index];

    for (uint32 seatIndex = 0; seatIndex < data.ActiveSeats; ++seatIndex)
    {
        SeatState& seat = data.Seats[seatIndex];
        Player* bot = _scenario.SeatBot(env, seatIndex);
        _scenario.PrepareFighter(bot, seat);

        SeatPull& supplies = pulls.Seats[seatIndex];
        supplies = SeatPull();
        if (Gauntlet())
        {
            ConsumablePool const& consumables = ConsumablePool::Instance();
            supplies.FoodItem = consumables.Food(seat.Level);
            supplies.DrinkItem = bot->GetMaxPower(POWER_MANA) ? consumables.Drink(seat.Level) : 0;
            StockConsumables(bot, supplies.FoodItem, supplies.DrinkItem);
        }
    }

    return SpawnPull(env, map);
}

bool AnimusForge::ClassRole::PullsEncounter::SpawnPull(Env& env, Map* map)
{
    EnvState& data = _scenario.Data(env);
    EnvPulls& pulls = _envs[env.Index];
    ClassRoleTuning::PullTuning const& tuning = _scenario.Tuning().Pulls;
    StageDefinition const& stage = _scenario.Stage();
    Opponents::OpponentPool const& pool = Opponents::OpponentPool::Instance();

    Player* lead = _scenario.SeatBot(env, 0);
    if (!lead)
        return false;

    uint8 const botLevel = data.Seats[0].Level;
    uint8 level = botLevel;
    std::vector<uint32> entries;
    pulls.EliteOrHigher = false;

    // A party faces dungeon-like packs: 2-4 creatures, each sometimes elite, up to 2 levels higher.
    if (stage.PartyGroup)
    {
        level = uint8(std::min<uint32>(HIGHEST_OPPONENT_LEVEL, botLevel + urand(0, 2)));
        uint8 const poolLevel = uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL));
        for (uint32 i = urand(2, PACK_SLOTS); i > 0; --i)
        {
            uint32 const elite = roll_chance_i(tuning.PartyEliteChance) ? pool.RandomElite(poolLevel) : 0;
            if (elite)
                pulls.EliteOrHigher = true;
            if (uint32 const entry = elite ? elite : pool.RandomPackMember(poolLevel))
                entries.push_back(entry);
        }
    }

    if (entries.empty() && Gauntlet() && roll_chance_i(tuning.EliteChance))
    {
        if (uint32 const elite = pool.RandomElite(botLevel))
        {
            entries.push_back(elite);
            pulls.EliteOrHigher = true;
        }
    }

    if (entries.empty())
    {
        if (Gauntlet() && roll_chance_i(tuning.HigherLevelChance))
        {
            level = uint8(std::min<uint32>(HIGHEST_OPPONENT_LEVEL, botLevel + urand(1, 3)));
            pulls.EliteOrHigher = true;
        }

        uint32 const count = Gauntlet() ? urand(1, PACK_SLOTS) : urand(2, PACK_SLOTS);
        for (uint32 i = 0; i < count; ++i)
            if (uint32 const entry = pool.RandomPackMember(uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL))))
                entries.push_back(entry);
    }

    // With an owner, pulls spawn around the owner, who walks over to them after a moment (in a party, after the tank
    // has had time to pull). A tank owner always starts the pull, and any other owner sometimes does, as a player who
    // pulls without waiting for the companions.
    OwnerEncounter* ownerPart = _scenario.OwnerPart();
    Player* anchor = ownerPart ? ownerPart->Find(env) : nullptr;
    if (anchor)
    {
        bool const ownerPulls = ownerPart->RoleOf(env) == Role::Tank || roll_chance_i(tuning.OwnerPullsChance);
        ownerPart->StateOf(env).EngageMs = env.EpisodeElapsedMs
            + (ownerPulls ? urand(tuning.OwnerPullsMinMs, tuning.OwnerPullsMaxMs)
            : stage.PartyGroup ? urand(tuning.PartyOwnerEngageMinMs, tuning.PartyOwnerEngageMaxMs)
            : urand(tuning.OwnerEngageMinMs, tuning.OwnerEngageMaxMs));
    }

    std::vector<Creature*> pack = Opponents::SpawnPack(anchor ? anchor : lead, map, entries, level);
    if (pack.empty())
        return false;

    env.Targets.clear();
    for (Creature* member : pack)
        env.Targets.push_back(member->GetGUID());

    if (!pulls.PullsCleared && !pulls.PackSize)
    {
        pulls.PackSize = uint32(pack.size());
        data.OpponentEntry = entries.front();
    }

    pulls.Linked = roll_chance_i(tuning.LinkedChance);
    pulls.PullKills = 0;
    pulls.PullStartMs = env.EpisodeElapsedMs;
    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
    {
        data.Seats[seat].TargetSlot = 0;
        data.Seats[seat].Combat.LastDistance = -1.0f;
        pulls.Seats[seat].PullDamageTaken = 0;
    }

    return true;
}

void AnimusForge::ClassRole::PullsEncounter::UpdateEnemies(Env& env)
{
    // Linked pulls: once one member is in combat, the rest of the pack joins in, on whoever the engaged member is
    // fighting.
    if (!_envs[env.Index].Linked)
        return;

    std::vector<Creature*> members;
    Unit* engagedVictim = nullptr;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        if (Creature* member = env.FindTarget(slot); member && member->IsAlive())
        {
            members.push_back(member);
            if (member->IsInCombat() && !engagedVictim)
                engagedVictim = member->GetVictim() ? member->GetVictim() : env.FindBot(0);
        }
    }

    if (engagedVictim && engagedVictim->IsAlive())
        for (Creature* member : members)
            if (!member->IsInCombat() && member->IsAIEnabled && member->CanCreatureAttack(engagedVictim))
                member->AI()->AttackStart(engagedVictim);
}

void AnimusForge::ClassRole::PullsEncounter::Update(Env& env)
{
    OwnerEncounter* ownerPart = _scenario.OwnerPart();
    if (ownerPart)
        Recover(env);

    if (!Gauntlet() || !env.Targets.empty() || env.EpisodeElapsedMs < _envs[env.Index].NextPullMs)
        return;

    // The next pull once the break is over, if anyone is left to fight it.
    bool anyoneAlive = false;
    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
        if (Player* bot = env.FindBot(seat); bot && bot->IsAlive())
            anyoneAlive = true;

    Player* owner = ownerPart ? ownerPart->Find(env) : nullptr;
    if (anyoneAlive && (!ownerPart || (owner && owner->IsAlive())))
        if (Map* map = env.FindMap())
            SpawnPull(env, map);
}

void AnimusForge::ClassRole::PullsEncounter::EndPull(Env& env, EnvPulls& pulls)
{
    pulls.PullKills = 0;
    pulls.PullCleared = false;
    pulls.QuietSinceMs = env.EpisodeElapsedMs;
    pulls.NextPullMs = env.EpisodeElapsedMs
        + urand(_scenario.Tuning().Pulls.NextPullMinMs, _scenario.Tuning().Pulls.NextPullMaxMs);

    EnvState& data = _scenario.Data(env);
    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
    {
        data.Seats[seat].TargetSlot = 0;
        data.Seats[seat].Combat.LastDistance = -1.0f;
    }
}

void AnimusForge::ClassRole::PullsEncounter::Recover(Env& env)
{
    EnvState const& data = _scenario.Data(env);
    EnvPulls& pulls = _envs[env.Index];
    Player* owner = _scenario.Owner(env);

    bool anyoneAlive = owner && owner->IsAlive();
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        if (Player* bot = _scenario.SeatBot(env, seat); bot && bot->IsAlive())
            anyoneAlive = true;

    // A wipe: nobody is left to finish the pull, so it is cleared away and the next one comes after the usual break.
    if (!anyoneAlive && !env.Targets.empty())
    {
        Despawn(env);
        ++pulls.Wipes;
        EndPull(env, pulls);
    }

    if (!env.Targets.empty())
        return;

    // Between pulls: the dead stand up with part of their health and mana, and their deaths can be paid for again.
    float const fraction = _scenario.Tuning().Pulls.RecoverFraction;
    auto const recover = [fraction](Player* player)
    {
        player->ResurrectPlayer(fraction);
        player->SetPower(POWER_MANA, uint32(float(player->GetMaxPower(POWER_MANA)) * fraction));
    };

    if (owner && !owner->IsAlive())
    {
        recover(owner);
        _scenario.NotifyRecovered(env, RECOVERED_OWNER);
    }

    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
    {
        Player* bot = _scenario.SeatBot(env, seat);
        if (!bot || bot->IsAlive())
            continue;

        recover(bot);
        _scenario.NotifyRecovered(env, int32(seat));
    }
}

bool AnimusForge::ClassRole::PullsEncounter::SelectTarget(Env const& env, uint32 seatIndex, Unit*& target)
{
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    if (Unit* selected = env.FindTargetUnit(seat.TargetSlot); selected && selected->IsAlive())
    {
        target = selected;
        return true;
    }

    // The selection died or despawned: the nearest living enemy, like a player tabbing to the next one.
    Player* bot = env.FindBot(seatIndex);
    target = nullptr;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy || !enemy->IsAlive())
            continue;

        if (!target || (bot && bot->GetDistance(enemy) < bot->GetDistance(target)))
        {
            target = enemy;
            seat.TargetSlot = slot;
        }
    }

    return true;
}

void AnimusForge::ClassRole::PullsEncounter::OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result)
{
    SeatPull& pull = _envs[env.Index].Seats[seat];
    pull.SustainCasts += result.SustainCasts;
    pull.FoodUsed += result.FoodUsed;
    pull.DrinkUsed += result.DrinkUsed;

    if (!result.PendingInterrupt.IsEmpty())
        pull.PendingInterrupt = result.PendingInterrupt;
}

void AnimusForge::ClassRole::PullsEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    EnvPulls const& pulls = _envs[env.Index];

    view.PullsCleared = pulls.PullsCleared;
    view.QuietTime = std::min(1.0f, float(env.EpisodeElapsedMs - pulls.QuietSinceMs) / QUIET_TIME_SCALE_MS);
    view.PullTime = std::min(1.0f, float(env.EpisodeElapsedMs - pulls.PullStartMs) / PULL_TIME_SCALE_MS);
    view.ElitePull = pulls.EliteOrHigher;
    view.FoodItem = pulls.Seats[seat].FoodItem;
    view.DrinkItem = pulls.Seats[seat].DrinkItem;
}

void AnimusForge::ClassRole::PullsEncounter::BeforeRewards(Env& env)
{
    EnvPulls& pulls = _envs[env.Index];

    uint32 alive = 0;
    uint32 dead = 0;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Unit* enemy = env.FindTargetUnit(slot))
            ++(enemy->IsAlive() ? alive : dead);

    pulls.NewKills = dead > pulls.PullKills ? dead - pulls.PullKills : 0;
    pulls.Kills += pulls.NewKills;
    pulls.PullKills = std::max(pulls.PullKills, dead);
    pulls.PullCleared = !env.Targets.empty() && !alive && dead;
}

void AnimusForge::ClassRole::PullsEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    ClassRoleTuning::PullTuning const& tuning = _scenario.Tuning().Pulls;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());
    if (!bot)
        return;

    EnvPulls& pulls = _envs[env.Index];
    SeatPull& pull = pulls.Seats[seatIndex];
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    CombatTally& tally = seat.Combat;
    AgentStats const& step = env.StepStats[seatIndex];
    float const botHealth = float(std::max<uint32>(1, bot->GetMaxHealth()));

    // Damage is a fraction of the pull's total health, taken damage a fraction of the bot's.
    float pullHealth = 0.0f;
    Unit* nearest = nullptr;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy)
            continue;

        pullHealth += float(enemy->GetMaxHealth());
        if (enemy->IsAlive() && (!nearest || bot->GetDistance(enemy) < bot->GetDistance(nearest)))
            nearest = enemy;
    }

    if (pullHealth > 0.0f)
        ledger.Add(RewardTerm::DamageDealt, tuning.DamageDealt * float(step.Damage) / pullHealth);

    tally.DamageTaken += step.DamageTaken;
    pull.PullDamageTaken += step.DamageTaken;
    seat.LastStepDamageTaken = float(step.DamageTaken) / botHealth;
    ledger.Add(RewardTerm::DamageTaken,
        -(Gauntlet() ? tuning.GauntletDamageTaken : tuning.DamageTaken) * seat.LastStepDamageTaken);

    CombatReward::Casting(bot, step, tally, _scenario.Tuning().Casting, ledger);
    CombatReward::Approach(bot, nearest && bot->IsAlive() ? nearest : nullptr,
        CombatReward::DesiredRange(seat, _scenario.Tuning().Duel), tuning.Approach, tally, ledger);

    if (tally.StepStealthOpener)
    {
        ledger.Add(RewardTerm::StealthOpener, tuning.StealthOpener);
        tally.StepStealthOpener = false;
    }

    // An interrupt counts when the enemy it was cast at is no longer casting.
    if (!pull.PendingInterrupt.IsEmpty())
    {
        Unit* interrupted = ObjectAccessor::GetUnit(*bot, pull.PendingInterrupt);
        if (!interrupted || !interrupted->IsNonMeleeSpellCast(false))
        {
            ledger.Add(RewardTerm::Interrupt, tuning.Interrupt);
            ++pull.Interrupts;
        }

        pull.PendingInterrupt.Clear();
    }

    if (bot->GetPetGUID() || !bot->m_Controlled.empty())
        tally.PetSummoned = true;

    // Kills and clears are the party's: every seat shares them. With an owner they count more: the pilot's party
    // learned to fight less to avoid the penalties.
    float const clearScale = _scenario.Stage().Owner ? tuning.OwnerClearScale : 1.0f;
    if (pulls.NewKills)
        ledger.Add(RewardTerm::Kill, tuning.Kill * clearScale * float(pulls.NewKills));

    if (pulls.PullCleared)
    {
        float const healthKept = 1.0f - std::min(1.0f, float(pull.PullDamageTaken) / botHealth);

        if (Gauntlet())
        {
            float const pullTime = float(env.EpisodeElapsedMs - pulls.PullStartMs);
            ledger.Add(RewardTerm::Clear,
                (tuning.Clear + tuning.FastPull * (1.0f - std::min(1.0f, pullTime / PULL_TIME_SCALE_MS))) * clearScale);
            pull.PullDamageTaken = 0;
        }
        else
        {
            if (!tally.Killed)
            {
                tally.Killed = true;
                tally.KillTimeMs = env.EpisodeElapsedMs;
            }

            ledger.Add(RewardTerm::Clear, tuning.Clear + tuning.FastClear * CombatReward::TimeLeft(env));
        }

        ledger.Add(RewardTerm::HealthKept, tuning.HealthKept * healthKept);
    }

    if (!tally.DeathCounted && !bot->IsAlive())
    {
        tally.DeathCounted = true;
        tally.Died = true;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -(Gauntlet() ? tuning.GauntletDeath : tuning.PackDeath));
    }
}

void AnimusForge::ClassRole::PullsEncounter::AfterRewards(Env& env)
{
    EnvPulls& pulls = _envs[env.Index];
    if (!pulls.PullCleared || !Gauntlet())
        return;

    // Clear the field and schedule the next pull.
    Despawn(env);
    ++pulls.PullsCleared;
    EndPull(env, pulls);
}

void AnimusForge::ClassRole::PullsEncounter::WriteState(Env const& env, float* state) const
{
    EnvPulls const& pulls = _envs[env.Index];
    bool const pullActive = !env.Targets.empty();

    state[ClassRoleScenario::STATE_PULL_ACTIVE] = pullActive ? 1.0f : 0.0f;
    state[ClassRoleScenario::STATE_PULLS_CLEARED] = std::min(1.0f, float(pulls.PullsCleared) / 10.0f);
    state[ClassRoleScenario::STATE_NEXT_PULL] = pullActive ? 0.0f
        : std::clamp((float(pulls.NextPullMs) - float(env.EpisodeElapsedMs)) / NEXT_PULL_SCALE_MS, 0.0f, 1.0f);
    state[ClassRoleScenario::STATE_ELITE_PULL] = pullActive && pulls.EliteOrHigher ? 1.0f : 0.0f;
    state[ClassRoleScenario::STATE_LINKED_PULL] = pullActive && pulls.Linked ? 1.0f : 0.0f;
}

bool AnimusForge::ClassRole::PullsEncounter::IsTerminal(Env const& env) const
{
    CombatTally const& tally = _scenario.Data(env).Seats[0].Combat;

    // With an owner nobody's death ends the episode (they stand up after the pull), so letting the owner die is
    // never a way out of the penalties.
    if (_scenario.Stage().Owner)
        return false;

    return Gauntlet() ? tally.Died : tally.Killed || tally.Died;
}
