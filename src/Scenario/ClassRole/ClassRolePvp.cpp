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
 * The PvP stages of ClassRoleScenario: ArenaMode::Pvp (seat 0 against a scripted enemy player) and ArenaMode::Arena
 * (self-play: seat 0 against seat 1, both learned). Opponent lifecycle, hostility, the PvP observations and rewards.
 */

#include "ClassRoleScenario.h"
#include "DuelArena.h"
#include "Env.h"
#include "ForgeBotFactory.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "TrainingDummyArena.h"
#include "WorldSession.h"
#include <algorithm>

namespace
{
    constexpr std::array<uint8, 10> PVP_CLASSES =
    {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_DEATH_KNIGHT, CLASS_SHAMAN,
        CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };

    constexpr uint32 OPPONENT_ACCOUNT_OFFSET = 300000;
    constexpr int32 OPPONENT_LEVEL_SPREAD = 1;
    constexpr uint32 OPPONENT_ENGAGE_MAX_MS = 3000;
    constexpr int32 OPPONENT_HEALER_CHANCE = 20;
    constexpr int32 OPPONENT_TANK_CHANCE = 20;

    // Player faction templates: a human's (Alliance) and an orc's (Horde). Enemies get the other side's.
    constexpr uint32 FACTION_ALLIANCE_PLAYER = 1;
    constexpr uint32 FACTION_HORDE_PLAYER = 2;

    constexpr uint32 STUN_STATES = UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING;
    constexpr uint32 CROWD_CONTROL_STATES = STUN_STATES | UNIT_STATE_ROOT;

    /// Make `enemy` hostile to `player` and flag both for PvP, which players need to attack each other.
    void MakeEnemies(Player* player, Player* enemy)
    {
        enemy->SetFaction(player->GetTeamId() == TEAM_ALLIANCE ? FACTION_HORDE_PLAYER : FACTION_ALLIANCE_PLAYER);
        for (Player* fighter : { player, enemy })
            if (!fighter->IsPvP())
                fighter->UpdatePvP(true, true);
    }

    bool IsControlled(Unit const* unit)
    {
        return unit->HasUnitState(CROWD_CONTROL_STATES) || unit->HasAuraType(SPELL_AURA_MOD_SILENCE)
            || unit->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE) || unit->HasAuraType(SPELL_AURA_TRANSFORM);
    }
}

Player* AnimusForge::ClassRoleScenario::FindOpponent(Env const& env, uint32 seat) const
{
    if (IsArena())
        return env.FindBot(1 - seat);

    EnvData const& data = _data[env.Index];
    WorldSession* session = data.OpponentSessions[data.OpponentActiveSession];
    Player* opponent = session ? session->GetPlayer() : nullptr;
    return opponent && opponent->IsInWorld() ? opponent : nullptr;
}

bool AnimusForge::ClassRoleScenario::StartPvp(Env& env, Map* map)
{
    EnvData& data = _data[env.Index];

    // No XP, the hunter's stable, a warrior's stance: the same start as the duel.
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        StartDuel(env.FindBot(seat), data.Seats[seat]);

    if (IsArena())
    {
        MakeEnemies(env.FindBot(0), env.FindBot(1));
        return true;
    }

    Player* bot = env.FindBot(0);
    if (!RebuildOpponent(env, bot, map))
        return false;

    env.Targets = { FindOpponent(env, 0)->GetGUID() };
    return true;
}

bool AnimusForge::ClassRoleScenario::RebuildOpponent(Env& env, Player* bot, Map* map)
{
    EnvData& data = _data[env.Index];
    WorldSession* activeSession = data.OpponentSessions[data.OpponentActiveSession];
    Player* old = activeSession ? activeSession->GetPlayer() : nullptr;

    uint8 const level = uint8(std::clamp<int32>(int32(data.Seats[0].Level) + irand(-OPPONENT_LEVEL_SPREAD,
        OPPONENT_LEVEL_SPREAD), 1, DEFAULT_MAX_LEVEL));

    int32 const roll = irand(0, 99);
    Role role = roll < OPPONENT_HEALER_CHANCE ? Role::Heal
        : roll < OPPONENT_HEALER_CHANCE + OPPONENT_TANK_CHANCE ? Role::Tank : Role::Dps;
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

    uint8 const session = old ? uint8(1 - data.OpponentActiveSession) : data.OpponentActiveSession;

    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Foe{}{}", env.Index, session ? "b" : "a");
    spec.Race = assets.Races[urand(0, uint32(assets.Races.size()) - 1)];
    spec.Class = playerClass;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = level;
    spec.AccountId = TrainingDummyArena::BOT_ACCOUNT_BASE + OPPONENT_ACCOUNT_OFFSET + env.Index * 2 + session;
    spec.GuidLow = data.OpponentGuids[session];

    Player* opponent = BotFactory::Create(spec, data.OpponentSessions[session]);
    if (!opponent)
        return false;

    data.OpponentSessions[session] = opponent->GetSession();
    data.OpponentGuids[session] = opponent->GetGUID().GetCounter();

    // Out of range at a random bearing, facing a random way, like the duel's creature.
    Position start = DuelArena::FindSpawnPoint(bot, map);
    start.SetOrientation(frand(0.0f, 2.0f * float(M_PI)));
    if (!BotFactory::PlaceInMap(opponent, map, start))
    {
        data.OpponentSessions[session] = nullptr;
        BotFactory::DestroyUnplaced(opponent);
        return false;
    }

    opponent->InitTalentForLevel();
    CompanionOwner::Configure(opponent, assets, data.Opponent);
    data.Opponent.EngageMs = env.EpisodeElapsedMs + urand(0, OPPONENT_ENGAGE_MAX_MS);
    MakeEnemies(bot, opponent);

    if (old)
        data.OpponentSessions[data.OpponentActiveSession] = BotFactory::Destroy(old, true);

    data.OpponentActiveSession = session;
    data.OpponentClass = playerClass;
    data.OpponentRole = role;
    return true;
}

void AnimusForge::ClassRoleScenario::DestroyOpponent(Env& env)
{
    EnvData& data = _data[env.Index];
    if (!IsArena())
    {
        WorldSession* session = data.OpponentSessions[data.OpponentActiveSession];
        if (Player* opponent = session ? session->GetPlayer() : nullptr)
        {
            BotFactory::Destroy(opponent);
            data.OpponentSessions[data.OpponentActiveSession] = nullptr;
        }
    }

    env.Targets.clear();
}

void AnimusForge::ClassRoleScenario::UpdatePvp(Env& env)
{
    EnvData& data = _data[env.Index];
    Player* bot = env.FindBot(0);
    Player* opponent = FindOpponent(env, 0);
    if (!bot || !opponent)
        return;

    // Zone updates can drop the PvP flag; the fight needs it.
    if (!bot->IsPvP() || !opponent->IsPvP())
        MakeEnemies(bot, opponent);

    if (!IsArena())
        CompanionOwner::UpdateOpponent(opponent, bot, env.EpisodeElapsedMs, data.Opponent);
}

void AnimusForge::ClassRoleScenario::ObservePvp(Env const& env, uint32 seatIndex, Player* bot, float* obs) const
{
    EnvData const& data = _data[env.Index];
    float* pvp = obs + data.Seats[seatIndex].L->PvpObsFirst;

    pvp[PVP_OBS_BOT_STUNNED] = bot->HasUnitState(STUN_STATES) ? 1.0f : 0.0f;
    pvp[PVP_OBS_BOT_ROOTED] = bot->HasUnitState(UNIT_STATE_ROOT) ? 1.0f : 0.0f;
    pvp[PVP_OBS_BOT_SILENCED] = bot->HasAuraType(SPELL_AURA_MOD_SILENCE)
        || bot->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE) ? 1.0f : 0.0f;
    pvp[PVP_OBS_MIRROR] = IsArena() ? 1.0f : 0.0f;

    Player* opponent = FindOpponent(env, seatIndex);
    if (!opponent)
        return;

    Seat const* other = IsArena() ? &data.Seats[1 - seatIndex] : nullptr;
    uint8 const opponentClass = other && other->L ? other->L->Profile->Class : data.OpponentClass;
    Role const opponentRole = other && other->L ? other->L->PlayRole() : data.OpponentRole;
    for (uint32 i = 0; i < PVP_CLASSES.size(); ++i)
        pvp[PVP_OBS_OPPONENT_CLASS_FIRST + i] = PVP_CLASSES[i] == opponentClass ? 1.0f : 0.0f;
    pvp[PVP_OBS_OPPONENT_ROLE_FIRST + uint32(opponentRole)] = 1.0f;

    pvp[PVP_OBS_OPPONENT_LEVEL_DIFF] = (float(opponent->GetLevel()) - float(bot->GetLevel())) / 5.0f;
    if (uint32 const maxMana = opponent->GetMaxPower(POWER_MANA))
        pvp[PVP_OBS_OPPONENT_MANA] = float(opponent->GetPower(POWER_MANA)) / float(maxMana);

    Powers const power = opponent->getPowerType();
    if (power != POWER_MANA)
        if (uint32 const maxPower = opponent->GetMaxPower(power))
            pvp[PVP_OBS_OPPONENT_RAGE_ENERGY] = float(opponent->GetPower(power)) / float(maxPower);

    pvp[PVP_OBS_OPPONENT_CONTROLLED] = IsControlled(opponent) ? 1.0f : 0.0f;
    pvp[PVP_OBS_OPPONENT_STEALTHED] = opponent->HasAuraType(SPELL_AURA_MOD_STEALTH) ? 1.0f : 0.0f;
    pvp[PVP_OBS_OPPONENT_PET_OUT] = opponent->GetPetGUID() || !opponent->m_Controlled.empty() ? 1.0f : 0.0f;

    if (Spell const* cast = opponent->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        if (cast->m_spellInfo->HasEffect(SPELL_EFFECT_HEAL) || cast->m_spellInfo->HasAura(SPELL_AURA_PERIODIC_HEAL))
            pvp[PVP_OBS_OPPONENT_HEALING] = 1.0f;
}

float AnimusForge::ClassRoleScenario::PvpReward(Env& env, uint32 seatIndex, Player* bot)
{
    Player* opponent = FindOpponent(env, seatIndex);
    if (!bot || !opponent)
        return 0.0f;

    return DuelReward(env, seatIndex, bot, opponent);
}

void AnimusForge::ClassRoleScenario::PvpEpisodeInfo(Env const& env, uint32 seatIndex, float* info) const
{
    EnvData const& data = _data[env.Index];
    Seat const& seat = data.Seats[seatIndex];
    Seat const* other = IsArena() ? &data.Seats[1 - seatIndex] : nullptr;

    info[PVP_INFO_WON] = seat.Killed && !seat.Died ? 1.0f : 0.0f;
    info[PVP_INFO_OPPONENT_CLASS] = float(other && other->L ? other->L->Profile->Class : data.OpponentClass);
    info[PVP_INFO_OPPONENT_ROLE] = float(uint32(other && other->L ? other->L->PlayRole() : data.OpponentRole));
}
