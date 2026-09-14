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
 * The pack and gauntlet stages of ClassRoleScenario (ArenaMode::Pack, ArenaMode::Gauntlet), also the pulls of the
 * companion and party stages: pulls of several creatures, enemy slots and target selection, tactical and sustain
 * spells, food and drink, and the clear-fast, take-little-damage rewards. A pull is shared by every seat of the env;
 * whether it was cleared is decided once per decision (AssessPull) and the field is cleared after every seat has
 * been rewarded (FinishPull).
 */

#include "ClassRoleScenario.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DuelArena.h"
#include "Env.h"
#include "Item.h"
#include "Map.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr int32 LINKED_CHANCE = 70;         // percent of pulls whose members aggro together
    constexpr int32 ELITE_CHANCE = 15;          // gauntlet: a single elite instead of a pack
    constexpr int32 HIGHER_LEVEL_CHANCE = 25;   // gauntlet: a pack 1-3 levels above the bot
    constexpr int32 PARTY_ELITE_CHANCE = 50;    // party: per pack member
    constexpr uint32 HIGHEST_OPPONENT_LEVEL = 83;
    constexpr uint32 NEXT_PULL_MIN_MS = 8000;
    constexpr uint32 NEXT_PULL_MAX_MS = 20000;
    constexpr float PULL_TIME_SCALE_MS = 60000.0f;
    constexpr uint32 OWNER_ENGAGE_MIN_MS = 1500;
    constexpr uint32 OWNER_ENGAGE_MAX_MS = 5000;
    constexpr uint32 PARTY_OWNER_ENGAGE_MIN_MS = 4000;  // after the party's tank has had time to pull
    constexpr uint32 PARTY_OWNER_ENGAGE_MAX_MS = 7000;

    // Reward terms. Damage is a fraction of the pull's total health, taken damage a fraction of the bot's.
    constexpr float DAMAGE_DEALT = 2.0f;
    constexpr float DAMAGE_TAKEN = 1.0f;
    constexpr float GAUNTLET_DAMAGE_TAKEN = 1.5f;   // surviving many pulls matters more than any one
    constexpr float APPROACH = 0.5f;
    constexpr float STEALTH_OPENER = 0.5f;
    constexpr float INTERRUPT = 0.3f;
    constexpr float KILL = 0.5f;
    constexpr float STEP_COST = 0.0002f;
    constexpr float CLEAR = 2.0f;
    constexpr float FAST_CLEAR = 3.0f;          // pack: times the fraction of the episode still left
    constexpr float FAST_PULL = 2.0f;           // gauntlet: times 1 - pull time / 60 s
    constexpr float HEALTH_KEPT = 2.0f;         // times the fraction of the bot's health not lost this pull
    constexpr float PACK_DEATH = 3.0f;
    constexpr float GAUNTLET_DEATH = 5.0f;

    constexpr uint32 CROWD_CONTROL_STATES = UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING
        | UNIT_STATE_ROOT;

    float CooldownFraction(Player const* bot, SpellInfo const* info)
    {
        uint32 const full = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
        return full ? std::min(1.0f, float(bot->GetSpellCooldownDelay(info->Id)) / float(full)) : 0.0f;
    }

    bool IsCrowdControlled(Unit const* unit)
    {
        return unit->HasUnitState(CROWD_CONTROL_STATES) || unit->HasAuraType(SPELL_AURA_MOD_SILENCE)
            || unit->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE) || unit->HasAuraType(SPELL_AURA_TRANSFORM);
    }

    SpellInfo const* UseSpell(uint32 itemEntry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
        if (!proto || proto->Spells[0].SpellId <= 0 || proto->Spells[0].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            return nullptr;

        return sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId);
    }
}

void AnimusForge::ClassRoleScenario::StartSeatPack(Player* bot, Seat& seat) const
{
    // No XP, the hunter's stable, a warrior's stance: the same start as the duel.
    StartDuel(bot, seat);

    if (HasGauntlet())
    {
        DuelArena::ConsumablePool const& consumables = DuelArena::ConsumablePool::Instance();
        seat.FoodItem = consumables.Food(seat.Level);
        seat.DrinkItem = bot->GetMaxPower(POWER_MANA) ? consumables.Drink(seat.Level) : 0;

        for (uint32 item : { seat.FoodItem, seat.DrinkItem })
            for (uint32 i = 0; item && i < CONSUMABLE_COUNT; ++i)
                if (!bot->StoreNewItemInBestSlots(item, 1))
                    break;
    }
}

bool AnimusForge::ClassRoleScenario::SpawnPull(Env& env, Map* map)
{
    EnvData& data = _data[env.Index];
    DuelArena::OpponentPool const& pool = DuelArena::OpponentPool::Instance();
    Player* lead = env.FindBot(0);
    if (!lead)
        return false;

    uint8 const botLevel = data.Seats[0].Level;
    uint8 level = botLevel;
    std::vector<uint32> entries;
    data.EliteOrHigherPull = false;

    // A party faces dungeon-like packs: 2-4 creatures, each elite half the time, up to 2 levels higher.
    if (HasParty())
    {
        level = uint8(std::min<uint32>(HIGHEST_OPPONENT_LEVEL, botLevel + urand(0, 2)));
        uint8 const poolLevel = uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL));
        for (uint32 i = urand(2, PACK_SLOTS); i > 0; --i)
        {
            uint32 const elite = roll_chance_i(PARTY_ELITE_CHANCE) ? pool.RandomElite(poolLevel) : 0;
            if (elite)
                data.EliteOrHigherPull = true;
            if (uint32 const entry = elite ? elite : pool.RandomPackMember(poolLevel))
                entries.push_back(entry);
        }
    }

    if (entries.empty() && HasGauntlet() && roll_chance_i(ELITE_CHANCE))
    {
        if (uint32 const elite = pool.RandomElite(botLevel))
        {
            entries.push_back(elite);
            data.EliteOrHigherPull = true;
        }
    }

    if (entries.empty())
    {
        if (HasGauntlet() && roll_chance_i(HIGHER_LEVEL_CHANCE))
        {
            level = uint8(std::min<uint32>(HIGHEST_OPPONENT_LEVEL, botLevel + urand(1, 3)));
            data.EliteOrHigherPull = true;
        }

        uint32 const count = HasGauntlet() ? urand(1, PACK_SLOTS) : urand(2, PACK_SLOTS);
        for (uint32 i = 0; i < count; ++i)
            if (uint32 const entry = pool.RandomPackMember(uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL))))
                entries.push_back(entry);
    }

    // With an owner, pulls spawn around the owner, who walks over to them after a moment (in a party, after the
    // tank has had time to pull).
    Player* anchor = HasCompanion() ? FindOwner(data) : nullptr;
    if (anchor)
        data.Owner.EngageMs = env.EpisodeElapsedMs + (IsParty()
            ? urand(PARTY_OWNER_ENGAGE_MIN_MS, PARTY_OWNER_ENGAGE_MAX_MS)
            : urand(OWNER_ENGAGE_MIN_MS, OWNER_ENGAGE_MAX_MS));

    std::vector<Creature*> pack = DuelArena::SpawnPack(anchor ? anchor : lead, map, entries, level);
    if (pack.empty())
        return false;

    env.Targets.clear();
    for (Creature* member : pack)
        env.Targets.push_back(member->GetGUID());

    if (!data.PullsCleared && !data.PackSize)
    {
        data.PackSize = uint32(pack.size());
        data.OpponentEntry = entries.front();
    }

    data.PackLinked = roll_chance_i(LINKED_CHANCE);
    data.PullKills = 0;
    data.PullStartMs = env.EpisodeElapsedMs;
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        data.Seats[seat].TargetSlot = 0;
        data.Seats[seat].PullDamageTaken = 0;
        data.Seats[seat].LastDistance = -1.0f;
    }
    return true;
}

void AnimusForge::ClassRoleScenario::UpdatePack(Env& env)
{
    EnvData& data = _data[env.Index];

    // Linked pulls: once one member is in combat, the rest of the pack joins in, on whoever the engaged member is
    // fighting.
    if (data.PackLinked)
    {
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

    if (HasCompanion())
        UpdateOwner(env);

    // Gauntlet: the next pull once the break is over, if anyone is left to fight it.
    bool anyoneAlive = false;
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        if (Player* bot = env.FindBot(seat); bot && bot->IsAlive())
            anyoneAlive = true;

    Player* owner = HasCompanion() ? FindOwner(data) : nullptr;
    if (HasGauntlet() && env.Targets.empty() && anyoneAlive && (!HasCompanion() || (owner && owner->IsAlive()))
        && env.EpisodeElapsedMs >= data.NextPullMs)
        if (Map* map = env.FindMap())
            SpawnPull(env, map);
}

void AnimusForge::ClassRoleScenario::AssessPull(Env& env)
{
    EnvData& data = _data[env.Index];

    uint32 alive = 0;
    uint32 dead = 0;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Unit* enemy = env.FindTargetUnit(slot))
            ++(enemy->IsAlive() ? alive : dead);

    data.NewKills = dead > data.PullKills ? dead - data.PullKills : 0;
    data.Kills += data.NewKills;
    data.PullKills = std::max(data.PullKills, dead);
    data.PullCleared = !env.Targets.empty() && !alive && dead;
}

void AnimusForge::ClassRoleScenario::FinishPull(Env& env)
{
    EnvData& data = _data[env.Index];
    if (!data.PullCleared || !HasGauntlet())
        return;

    // Clear the field and schedule the next pull.
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Creature* enemy = env.FindTarget(slot))
            enemy->DespawnOrUnsummon();

    env.Targets.clear();
    ++data.PullsCleared;
    data.PullKills = 0;
    data.PullCleared = false;
    data.NextPullMs = env.EpisodeElapsedMs + urand(NEXT_PULL_MIN_MS, NEXT_PULL_MAX_MS);
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        data.Seats[seat].TargetSlot = 0;
        data.Seats[seat].LastDistance = -1.0f;
    }
}

void AnimusForge::ClassRoleScenario::ObservePack(Env const& env, uint32 seatIndex, Player* bot, float* obs) const
{
    Seat const& seat = _data[env.Index].Seats[seatIndex];
    float* pack = obs + seat.L->PackObsFirst;

    uint32 alive = 0;
    uint32 inCombat = 0;
    for (uint32 slot = 0; slot < env.Targets.size() && slot < PACK_SLOTS; ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy)
            continue;

        float* features = pack + PACK_OBS_GLOBAL_COUNT + slot * SLOT_FEATURES;
        float const bearing = bot->GetRelativeAngle(enemy);
        Unit const* victim = enemy->GetVictim();

        features[SLOT_PRESENT] = 1.0f;
        features[SLOT_ALIVE] = enemy->IsAlive() ? 1.0f : 0.0f;
        features[SLOT_HEALTH] = enemy->GetHealthPct() / 100.0f;
        features[SLOT_DISTANCE] = std::min(1.0f, bot->GetDistance(enemy) / 60.0f);
        features[SLOT_BEARING_SIN] = std::sin(bearing);
        features[SLOT_BEARING_COS] = std::cos(bearing);
        features[SLOT_BEHIND] = enemy->isInBack(bot) ? 1.0f : 0.0f;
        features[SLOT_ATTACKS_BOT] = victim == bot ? 1.0f : 0.0f;
        features[SLOT_ATTACKS_PET] = victim && victim != bot && victim->GetOwnerGUID() == bot->GetGUID() ? 1.0f : 0.0f;
        features[SLOT_CASTING] = enemy->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
        features[SLOT_IN_COMBAT] = enemy->IsInCombat() ? 1.0f : 0.0f;
        features[SLOT_CROWD_CONTROLLED] = IsCrowdControlled(enemy) ? 1.0f : 0.0f;
        features[SLOT_CURRENT_TARGET] = slot == seat.TargetSlot ? 1.0f : 0.0f;
        features[SLOT_ELITE] = enemy->ToCreature() && enemy->ToCreature()->isElite() ? 1.0f : 0.0f;
        features[SLOT_LEVEL_DIFFERENCE] = (float(enemy->GetLevel()) - float(bot->GetLevel())) / 5.0f;

        alive += enemy->IsAlive() ? 1 : 0;
        inCombat += enemy->IsAlive() && enemy->IsInCombat() ? 1 : 0;
    }

    pack[PACK_OBS_ALIVE] = float(alive) / float(PACK_SLOTS);
    pack[PACK_OBS_IN_COMBAT] = float(inCombat) / float(PACK_SLOTS);

    float* tactical = pack + PACK_OBS_GLOBAL_COUNT + PACK_SLOTS * SLOT_FEATURES;
    std::vector<ActionCatalog::Action> const& actions = seat.L->Catalog().Tactical();
    for (uint32 i = 0; i < actions.size(); ++i)
    {
        if (SpellInfo const* info = ActionCatalog::KnownRank(bot, actions[i].FirstRank))
        {
            tactical[i * 2] = 1.0f;
            tactical[i * 2 + 1] = CooldownFraction(bot, info);
        }
    }
}

void AnimusForge::ClassRoleScenario::ObserveGauntlet(Env const& env, uint32 seatIndex, Player* bot, float* obs) const
{
    EnvData const& data = _data[env.Index];
    Seat const& seat = data.Seats[seatIndex];
    float* gauntlet = obs + seat.L->GauntletObsFirst;
    bool const pullActive = !env.Targets.empty();

    gauntlet[GAUNTLET_OBS_PULLS_CLEARED] = std::min(1.0f, float(data.PullsCleared) / 10.0f);
    gauntlet[GAUNTLET_OBS_PULL_ACTIVE] = pullActive ? 1.0f : 0.0f;
    gauntlet[GAUNTLET_OBS_NEXT_PULL] = pullActive ? 0.0f
        : std::clamp((float(data.NextPullMs) - float(env.EpisodeElapsedMs)) / float(NEXT_PULL_MAX_MS), 0.0f, 1.0f);
    gauntlet[GAUNTLET_OBS_PULL_TIME] = pullActive
        ? std::min(1.0f, float(env.EpisodeElapsedMs - data.PullStartMs) / PULL_TIME_SCALE_MS) : 0.0f;
    gauntlet[GAUNTLET_OBS_ELITE_PULL] = pullActive && data.EliteOrHigherPull ? 1.0f : 0.0f;
    gauntlet[GAUNTLET_OBS_EATING] = bot->HasAuraType(SPELL_AURA_MOD_REGEN) ? 1.0f : 0.0f;
    gauntlet[GAUNTLET_OBS_DRINKING] = bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN) ? 1.0f : 0.0f;
    gauntlet[GAUNTLET_OBS_FOOD_LEFT] = seat.FoodItem
        ? float(bot->GetItemCount(seat.FoodItem)) / float(CONSUMABLE_COUNT) : 0.0f;
    gauntlet[GAUNTLET_OBS_DRINK_LEFT] = seat.DrinkItem
        ? float(bot->GetItemCount(seat.DrinkItem)) / float(CONSUMABLE_COUNT) : 0.0f;

    float* sustain = gauntlet + GAUNTLET_OBS_GLOBAL_COUNT;
    std::vector<ActionCatalog::Action> const& actions = seat.L->Catalog().Sustain();
    for (uint32 i = 0; i < actions.size(); ++i)
    {
        if (SpellInfo const* info = ActionCatalog::KnownRank(bot, actions[i].FirstRank))
        {
            sustain[i * 2] = 1.0f;
            sustain[i * 2 + 1] = CooldownFraction(bot, info);
        }
    }
}

bool AnimusForge::ClassRoleScenario::IsPackActionAllowed(Env const& env, uint32 seatIndex, Player* bot,
    uint32 packAction) const
{
    // Only the target slots; tactical spells are masked by IsSpellActionAllowed.
    if (packAction >= PACK_SLOTS || packAction >= env.Targets.size()
        || packAction == _data[env.Index].Seats[seatIndex].TargetSlot || !bot->IsAlive())
        return false;

    Unit const* enemy = env.FindTargetUnit(packAction);
    return enemy && enemy->IsAlive();
}

void AnimusForge::ClassRoleScenario::ApplyPackAction(Env& env, uint32 seatIndex, Player* bot, Unit* target,
    uint32 packAction)
{
    Seat& seat = _data[env.Index].Seats[seatIndex];
    if (packAction >= PACK_SLOTS)
    {
        if (target)
            ApplySpellAction(bot, target, seat.L->Catalog().Tactical()[packAction - PACK_SLOTS], seat);
        return;
    }

    if (!IsPackActionAllowed(env, seatIndex, bot, packAction))
        return;

    Unit* enemy = env.FindTargetUnit(packAction);
    seat.TargetSlot = packAction;
    bot->SetSelection(enemy->GetGUID());

    // Keep swinging, at the new target.
    if (bot->GetVictim())
        bot->Attack(enemy, bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING));
}

bool AnimusForge::ClassRoleScenario::IsGauntletActionAllowed(Player* bot, Unit* target, uint32 gauntletAction,
    Seat const& seat) const
{
    if (gauntletAction >= GAUNTLET_ACTION_SUSTAIN_FIRST)
        return IsSpellActionAllowed(bot, target,
            seat.L->Catalog().Sustain()[gauntletAction - GAUNTLET_ACTION_SUSTAIN_FIRST]);

    bool const eat = gauntletAction == GAUNTLET_ACTION_EAT;
    uint32 const item = eat ? seat.FoodItem : seat.DrinkItem;
    SpellInfo const* info = item ? UseSpell(item) : nullptr;

    return info && bot->IsAlive() && !bot->IsInCombat() && bot->movespline->Finalized()
        && !bot->IsNonMeleeSpellCast(false) && bot->GetItemCount(item) && !bot->HasSpellCooldown(info->Id)
        && !bot->HasAuraType(eat ? SPELL_AURA_MOD_REGEN : SPELL_AURA_MOD_POWER_REGEN);
}

void AnimusForge::ClassRoleScenario::ApplyGauntletAction(Player* bot, Unit* target, uint32 gauntletAction,
    Seat& seat) const
{
    if (!IsGauntletActionAllowed(bot, target, gauntletAction, seat))
        return;

    if (gauntletAction >= GAUNTLET_ACTION_SUSTAIN_FIRST)
    {
        if (ApplySpellAction(bot, target, seat.L->Catalog().Sustain()[gauntletAction - GAUNTLET_ACTION_SUSTAIN_FIRST],
            seat))
            ++seat.SustainCasts;
        return;
    }

    bool const eat = gauntletAction == GAUNTLET_ACTION_EAT;
    Item* item = bot->GetItemByEntry(eat ? seat.FoodItem : seat.DrinkItem);
    if (!item)
        return;

    SpellCastTargets targets;
    targets.SetUnitTarget(bot);
    bot->CastItemUseSpell(item, targets, 1, 0);

    if (bot->HasAuraType(eat ? SPELL_AURA_MOD_REGEN : SPELL_AURA_MOD_POWER_REGEN))
        ++(eat ? seat.FoodUsed : seat.DrinkUsed);
}

float AnimusForge::ClassRoleScenario::PackReward(Env& env, uint32 seatIndex, Player* bot)
{
    float reward = -STEP_COST;
    if (!bot)
        return reward;

    EnvData& data = _data[env.Index];
    Seat& seat = data.Seats[seatIndex];
    AgentStats const& step = env.StepStats[seatIndex];
    float const botHealth = float(std::max<uint32>(1, bot->GetMaxHealth()));

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
        reward += DAMAGE_DEALT * float(step.Damage) / pullHealth;

    seat.DamageTaken += step.DamageTaken;
    seat.PullDamageTaken += step.DamageTaken;
    seat.LastStepDamageTaken = float(step.DamageTaken) / botHealth;
    reward -= (HasGauntlet() ? GAUNTLET_DAMAGE_TAKEN : DAMAGE_TAKEN) * seat.LastStepDamageTaken;
    reward += CastReward(bot, step, seat);

    // Potential-based shaping toward the nearest living enemy, as in the duel.
    if (nearest && bot->IsAlive())
    {
        float const excess = std::max(0.0f, bot->GetDistance(nearest) - DesiredRange(seat));
        if (seat.LastDistance >= 0.0f)
            reward += APPROACH * (seat.LastDistance - excess) / 40.0f;
        seat.LastDistance = excess;
    }
    else
        seat.LastDistance = -1.0f;

    if (seat.StepStealthOpener)
    {
        reward += STEALTH_OPENER;
        seat.StepStealthOpener = false;
    }

    // An interrupt counts when the enemy it was cast at is no longer casting.
    if (!seat.PendingInterrupt.IsEmpty())
    {
        Unit* interrupted = ObjectAccessor::GetUnit(*bot, seat.PendingInterrupt);
        if (!interrupted || !interrupted->IsNonMeleeSpellCast(false))
        {
            reward += INTERRUPT;
            ++seat.Interrupts;
        }

        seat.PendingInterrupt.Clear();
    }

    if (bot->GetPetGUID() || !bot->m_Controlled.empty())
        seat.PetSummoned = true;

    // Kills and clears are the party's: every seat shares them.
    reward += KILL * float(data.NewKills);

    if (data.PullCleared)
    {
        float const healthKept = 1.0f - std::min(1.0f, float(seat.PullDamageTaken) / botHealth);

        if (!seat.Killed && !HasGauntlet())
        {
            seat.Killed = true;
            seat.KillTimeMs = env.EpisodeElapsedMs;
        }

        if (HasGauntlet())
        {
            float const pullTime = float(env.EpisodeElapsedMs - data.PullStartMs);
            reward += CLEAR + FAST_PULL * (1.0f - std::min(1.0f, pullTime / PULL_TIME_SCALE_MS))
                + HEALTH_KEPT * healthKept;
            seat.PullDamageTaken = 0;
        }
        else
        {
            float const timeLeft = env.EpisodeLengthMs
                ? 1.0f - std::min(1.0f, float(env.EpisodeElapsedMs) / float(env.EpisodeLengthMs)) : 0.0f;
            reward += CLEAR + FAST_CLEAR * timeLeft + HEALTH_KEPT * healthKept;
        }
    }

    if (!seat.Died && !bot->IsAlive())
    {
        seat.Died = true;
        reward -= HasGauntlet() ? GAUNTLET_DEATH : PACK_DEATH;
    }

    return reward;
}

void AnimusForge::ClassRoleScenario::PackEpisodeInfo(Env const& env, uint32 seatIndex, float* info) const
{
    EnvData const& data = _data[env.Index];

    info[PACK_INFO_KILLS] = float(data.Kills);
    info[PACK_INFO_INTERRUPTS] = float(data.Seats[seatIndex].Interrupts);
    info[PACK_INFO_PACK_SIZE] = float(data.PackSize);
    info[PACK_INFO_LINKED] = data.PackLinked ? 1.0f : 0.0f;
}

void AnimusForge::ClassRoleScenario::GauntletEpisodeInfo(Env const& env, uint32 seatIndex, float* info) const
{
    EnvData const& data = _data[env.Index];
    Seat const& seat = data.Seats[seatIndex];

    info[GAUNTLET_INFO_PULLS_CLEARED] = float(data.PullsCleared);
    info[GAUNTLET_INFO_FOOD_USED] = float(seat.FoodUsed);
    info[GAUNTLET_INFO_DRINK_USED] = float(seat.DrinkUsed);
    info[GAUNTLET_INFO_SUSTAIN_CASTS] = float(seat.SustainCasts);
}
