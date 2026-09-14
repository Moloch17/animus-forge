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
 * The PvP stages of ClassRoleScenario: ArenaMode::Pvp (a scripted enemy player) and ArenaMode::Arena (self-play:
 * paired envs whose bots fight each other). Opponent lifecycle, hostility, the PvP observations and rewards.
 *
 * Self-play pairs: env 2k and env 2k+1 share env 2k's instance, and each one's target is the other's bot. Their
 * episodes end together: whatever ends one ends the other on the same decision (a death is seen by both, and the
 * time limit is the same). EnvPool resets the lower env first, which rebuilds its bot before the higher env has
 * scored the decision, so the lower env leaves a snapshot (PartnerEnded, PartnerDied) that the higher env scores
 * and ends from.
 */

#include "ClassRoleScenario.h"
#include "DuelArena.h"
#include "Env.h"
#include "ForgeBotFactory.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Pet.h"
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

Player* AnimusForge::ClassRoleScenario::FindOpponent(Env const& env) const
{
    if (IsArena())
    {
        uint32 const partner = PartnerIndex(env.Index);
        return partner < _envs.size() && _envs[partner] ? _envs[partner]->FindBot(0) : nullptr;
    }

    EnvData const& data = _data[env.Index];
    WorldSession* session = data.OpponentSessions[data.OpponentActiveSession];
    return session ? session->GetPlayer() : nullptr;
}

bool AnimusForge::ClassRoleScenario::StartPvp(Env& env, Player* bot, Map* map, EnvData& data) const
{
    // No XP, the hunter's stable, a warrior's stance: the same start as the duel.
    StartDuel(bot, nullptr, data);

    if (!IsArena())
    {
        if (!RebuildOpponent(env, bot, map, data))
            return false;

        env.Targets = { FindOpponent(env)->GetGUID() };
        return true;
    }

    // Self-play: the lower env of a pair builds first and waits; the higher one links both targets.
    if (env.Index % 2 == 0)
    {
        env.Targets.clear();
        return true;
    }

    Env* partner = _envs[PartnerIndex(env.Index)];
    Player* opponent = partner ? partner->FindBot(0) : nullptr;
    if (!opponent)
        return false;

    MakeEnemies(opponent, bot);
    env.Targets = { opponent->GetGUID() };
    partner->Targets = { bot->GetGUID() };
    return true;
}

bool AnimusForge::ClassRoleScenario::RebuildOpponent(Env& env, Player* bot, Map* map, EnvData& data) const
{
    CompanionOwner::Templates const& templates = CompanionOwner::Templates::Instance();
    WorldSession* activeSession = data.OpponentSessions[data.OpponentActiveSession];
    Player* old = activeSession ? activeSession->GetPlayer() : nullptr;

    uint8 const level = uint8(std::clamp<int32>(int32(data.Level) + irand(-OPPONENT_LEVEL_SPREAD,
        OPPONENT_LEVEL_SPREAD), 1, DEFAULT_MAX_LEVEL));

    int32 const roll = irand(0, 99);
    Role role = roll < OPPONENT_HEALER_CHANCE ? Role::Heal
        : roll < OPPONENT_HEALER_CHANCE + OPPONENT_TANK_CHANCE ? Role::Tank : Role::Dps;
    std::vector<uint8> classes = templates.ClassesForRole(level, role);
    if (classes.empty())
    {
        role = Role::Dps;
        classes = templates.ClassesForRole(level, role);
    }
    if (classes.empty())
        return false;

    uint8 const playerClass = classes[urand(0, uint32(classes.size()) - 1)];
    CompanionOwner::Template const* opponentTemplate = templates.ForClassRole(playerClass, role);

    uint8 const session = old ? uint8(1 - data.OpponentActiveSession) : data.OpponentActiveSession;

    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Foe{}{}", env.Index, session ? "b" : "a");
    spec.Race = opponentTemplate->Races[urand(0, uint32(opponentTemplate->Races.size()) - 1)];
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
    CompanionOwner::Configure(opponent, *opponentTemplate, data.Opponent);
    data.Opponent.EngageMs = env.EpisodeElapsedMs + urand(0, OPPONENT_ENGAGE_MAX_MS);
    MakeEnemies(bot, opponent);

    if (old)
        data.OpponentSessions[data.OpponentActiveSession] = BotFactory::Destroy(old, true);

    data.OpponentActiveSession = session;
    data.OpponentClass = playerClass;
    data.OpponentRole = role;
    return true;
}

void AnimusForge::ClassRoleScenario::DestroyOpponent(Env& env, EnvData& data) const
{
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

void AnimusForge::ClassRoleScenario::UpdatePvp(Env& env, Player* bot, EnvData& data) const
{
    Player* opponent = FindOpponent(env);
    if (!opponent)
        return;

    // Zone updates can drop the PvP flag; the fight needs it.
    if (!bot->IsPvP() || !opponent->IsPvP())
        MakeEnemies(bot, opponent);

    if (!IsArena())
        CompanionOwner::UpdateOpponent(opponent, bot, env.EpisodeElapsedMs, data.Opponent);
}

void AnimusForge::ClassRoleScenario::ObservePvp(Env const& env, Player* bot, float* obs) const
{
    EnvData const& data = _data[env.Index];
    float* pvp = obs + _pvpObsFirst;

    pvp[PVP_OBS_BOT_STUNNED] = bot->HasUnitState(STUN_STATES) ? 1.0f : 0.0f;
    pvp[PVP_OBS_BOT_ROOTED] = bot->HasUnitState(UNIT_STATE_ROOT) ? 1.0f : 0.0f;
    pvp[PVP_OBS_BOT_SILENCED] = bot->HasAuraType(SPELL_AURA_MOD_SILENCE)
        || bot->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE) ? 1.0f : 0.0f;
    pvp[PVP_OBS_MIRROR] = IsArena() ? 1.0f : 0.0f;

    Player* opponent = FindOpponent(env);
    if (!opponent)
        return;

    uint8 const opponentClass = IsArena() ? _profile.Class : data.OpponentClass;
    Role const opponentRole = IsArena() ? _profile.PlayRole : data.OpponentRole;
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

float AnimusForge::ClassRoleScenario::PvpReward(Env& env, Player* bot, EnvData& data) const
{
    Player* opponent = FindOpponent(env);
    if (!bot || !opponent)
        return 0.0f;

    // Stage 8, higher env of a pair whose lower env already ended the episode: score from the snapshot (the
    // partner's bot has been rebuilt), with no distance shaping against the new bot.
    int8 opponentDead = -1;
    if (IsArena() && data.PartnerEnded)
    {
        opponentDead = data.PartnerDied ? 1 : 0;
        data.LastDistance = -1.0f;
    }

    return DuelReward(env, bot, opponent, data, opponentDead);
}

void AnimusForge::ClassRoleScenario::PvpEpisodeInfo(Env const& env, float* info) const
{
    EnvData const& data = _data[env.Index];

    info[PVP_INFO_WON] = data.Killed && !data.Died ? 1.0f : 0.0f;
    info[PVP_INFO_OPPONENT_CLASS] = float(IsArena() ? _profile.Class : data.OpponentClass);
    info[PVP_INFO_OPPONENT_ROLE] = float(uint32(IsArena() ? _profile.PlayRole : data.OpponentRole));
}
