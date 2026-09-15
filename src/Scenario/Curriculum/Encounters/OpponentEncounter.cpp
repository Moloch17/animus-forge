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
#include "BotAccounts.h"
#include "CombatReward.h"
#include "Env.h"
#include "Map.h"
#include "Opponents.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "StringFormat.h"
#include <algorithm>

namespace
{
    // Player faction templates: a human's (Alliance) and an orc's (Horde). Enemies get the other side's.
    constexpr uint32 FACTION_ALLIANCE_PLAYER = 1;
    constexpr uint32 FACTION_HORDE_PLAYER = 2;

    /// Make `enemy` hostile to `player` and flag both for PvP, which players need to attack each other.
    void MakeEnemies(Player* player, Player* enemy)
    {
        enemy->SetFaction(player->GetTeamId() == TEAM_ALLIANCE ? FACTION_HORDE_PLAYER : FACTION_ALLIANCE_PLAYER);
        for (Player* fighter : { player, enemy })
            if (!fighter->IsPvP())
                fighter->UpdatePvP(true, true);
    }
}

AnimusForge::Curriculum::OpponentEncounter::OpponentEncounter(StageScenario& scenario, uint32 envs, bool mirror)
    : Encounter(scenario), _mirror(mirror), _envs(envs)
{
}

std::vector<AnimusForge::Curriculum::RewardTerm> AnimusForge::Curriculum::OpponentEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::DamageDealt, RewardTerm::DamageTaken, RewardTerm::Casting,
        RewardTerm::Approach, RewardTerm::StealthOpener, RewardTerm::Kill, RewardTerm::HealthKept, RewardTerm::Death };
}

void AnimusForge::Curriculum::OpponentEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("won", [this](Env const& env, uint32 seat)
    {
        CombatTally const& tally = _scenario.Data(env).Seats[seat].Combat;
        return tally.Killed && !tally.Died ? 1.0f : 0.0f;
    });

    table.Add("opponent_class", [this](Env const& env, uint32 seat)
    {
        if (!_mirror)
            return float(_envs[env.Index].Class);
        SeatState const& other = _scenario.Data(env).Seats[1 - seat];
        return other.L ? float(other.L->Profile->Class) : 0.0f;
    });

    table.Add("opponent_role", [this](Env const& env, uint32 seat)
    {
        if (!_mirror)
            return float(uint32(_envs[env.Index].PlayRole));
        SeatState const& other = _scenario.Data(env).Seats[1 - seat];
        return other.L ? float(uint32(other.L->PlayRole())) : 0.0f;
    });
}

Player* AnimusForge::Curriculum::OpponentEncounter::Find(Env const& env, uint32 seat) const
{
    if (_mirror)
        return env.FindBot(1 - seat);

    Player* opponent = _envs[env.Index].Bot.Active();
    return opponent && opponent->IsInWorld() ? opponent : nullptr;
}

bool AnimusForge::Curriculum::OpponentEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);

    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
        _scenario.PrepareFighter(_scenario.SeatBot(env, seat), data.Seats[seat]);

    if (_mirror)
    {
        MakeEnemies(_scenario.SeatBot(env, 0), _scenario.SeatBot(env, 1));
        return true;
    }

    if (!RebuildScripted(env, _scenario.SeatBot(env, 0), map))
        return false;

    env.Targets = { Find(env, 0)->GetGUID() };
    return true;
}

bool AnimusForge::Curriculum::OpponentEncounter::RebuildScripted(Env& env, Player* bot, Map* map)
{
    EnvOpponent& opponent = _envs[env.Index];
    CurriculumTuning::OpponentTuning const& tuning = _scenario.Tuning().Opponent;

    uint8 const level = uint8(std::clamp<int32>(int32(_scenario.Data(env).Seats[0].Level)
        + irand(-tuning.LevelSpread, tuning.LevelSpread), 1, DEFAULT_MAX_LEVEL));

    Role role = RollRole(tuning.TankChance, tuning.HealerChance);
    std::vector<uint8> classes = ClassRoleAssets::ClassesForRole(level, role);
    if (classes.empty())
    {
        role = Role::Dps;
        classes = ClassRoleAssets::ClassesForRole(level, role);
    }
    if (classes.empty())
        return false;

    uint8 const playerClass = classes[urand(0, uint32(classes.size()) - 1)];
    ClassRoleAssets const& assets = ClassRoleAssets::For(*ClassRoleAssets::FindProfile(playerClass, role));

    opponent.Bot.Begin();
    uint8 const session = opponent.Bot.NextSession();

    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Foe{}{}", env.Index, session ? "b" : "a");
    spec.Race = assets.Races[urand(0, uint32(assets.Races.size()) - 1)];
    spec.Class = playerClass;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = level;
    spec.AccountId = BotAccounts::Opponent(env.Index, session);

    // Out of range at a random bearing, facing a random way, like the duel's creature.
    Position start = Opponents::FindSpawnPoint(bot, map);
    start.SetOrientation(frand(0.0f, 2.0f * float(M_PI)));
    Player* enemy = opponent.Bot.CreateNext(spec, map, _scenario.SpawnMapId(), start);
    if (!enemy)
        return false;

    enemy->InitTalentForLevel();
    ScriptedPlayer::Configure(enemy, assets, opponent.Script, true);
    opponent.Script.EngageMs = env.EpisodeElapsedMs + urand(0, tuning.EngageMaxMs);
    MakeEnemies(bot, enemy);

    opponent.Bot.Promote();
    opponent.Class = playerClass;
    opponent.PlayRole = role;
    return true;
}

void AnimusForge::Curriculum::OpponentEncounter::Update(Env& env)
{
    Player* bot = env.FindBot(0);
    Player* opponent = Find(env, 0);
    if (!bot || !opponent)
        return;

    // Zone updates can drop the PvP flag; the fight needs it.
    if (!bot->IsPvP() || !opponent->IsPvP())
        MakeEnemies(bot, opponent);

    if (!_mirror)
        ScriptedPlayer::UpdateOpponent(opponent, bot, env.EpisodeElapsedMs, _envs[env.Index].Script,
            _scenario.Tuning().ScriptedPlayers);
}

bool AnimusForge::Curriculum::OpponentEncounter::SelectTarget(Env const& env, uint32 seat, Unit*& target)
{
    target = Find(env, seat);
    return true;
}

void AnimusForge::Curriculum::OpponentEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    view.Opponent = Find(env, seat);
    view.Mirror = _mirror;

    if (_mirror)
    {
        SeatState const& other = _scenario.Data(env).Seats[1 - seat];
        view.OpponentClass = other.L ? other.L->Profile->Class : 0;
        view.OpponentRole = other.L ? other.L->PlayRole() : Role::Dps;
        return;
    }

    view.OpponentClass = _envs[env.Index].Class;
    view.OpponentRole = _envs[env.Index].PlayRole;
}

void AnimusForge::Curriculum::OpponentEncounter::Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger)
{
    // No opponent in the world (a far teleport, a failed rebuild): nothing to score, not even the step cost.
    if (Player* opponent = Find(env, seat); bot && opponent)
        CombatReward::OneOnOne(_scenario, env, seat, bot, opponent, ledger);
}

bool AnimusForge::Curriculum::OpponentEncounter::IsTerminal(Env const& env) const
{
    EnvState const& data = _scenario.Data(env);
    if (_mirror)
        return data.Seats[0].Combat.Died || data.Seats[1].Combat.Died;

    return data.Seats[0].Combat.Died || data.Seats[0].Combat.Killed;
}

void AnimusForge::Curriculum::OpponentEncounter::Teardown(Env& env)
{
    if (!_mirror)
        _envs[env.Index].Bot.Destroy();

    env.Targets.clear();
}
