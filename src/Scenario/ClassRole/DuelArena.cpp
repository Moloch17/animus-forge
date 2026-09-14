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

#include "DuelArena.h"
#include "CharmInfo.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SummonLevel.h"
#include "TemporarySummon.h"
#include <cmath>
#include <map>
#include <unordered_set>

namespace
{
    enum DuelArenaSpells : uint32
    {
        SPELL_TAME_BEAST            = 1515,
    };

    /// Hunters get Tame Beast and Call Pet at level 10.
    constexpr uint8 HUNTER_PET_LEVEL = 10;

    constexpr uint32 SPAWN_ATTEMPTS = 12;
    constexpr float PACK_SPREAD = 5.0f;
    constexpr float MAX_HEIGHT_DIFFERENCE = 6.0f;

    constexpr uint32 UNUSABLE_UNIT_FLAGS = UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_NOT_SELECTABLE
        | UNIT_FLAG_PACIFIED;
    constexpr uint32 UNUSABLE_EXTRA_FLAGS = CREATURE_FLAG_EXTRA_CIVILIAN | CREATURE_FLAG_EXTRA_TRIGGER
        | CREATURE_FLAG_EXTRA_GUARD;

    bool IsFairOpponentType(uint32 type)
    {
        switch (type)
        {
            case CREATURE_TYPE_BEAST:
            case CREATURE_TYPE_DRAGONKIN:
            case CREATURE_TYPE_DEMON:
            case CREATURE_TYPE_ELEMENTAL:
            case CREATURE_TYPE_GIANT:
            case CREATURE_TYPE_UNDEAD:
            case CREATURE_TYPE_HUMANOID:
                return true;
            default:
                return false;
        }
    }
}

AnimusForge::DuelArena::OpponentPool const& AnimusForge::DuelArena::OpponentPool::Instance()
{
    static OpponentPool const pool;
    return pool;
}

AnimusForge::DuelArena::OpponentPool::OpponentPool()
{
    std::unordered_set<uint32> spawned;
    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT id FROM creature"))
    {
        do
        {
            spawned.insert(result->Fetch()[0].Get<uint32>());
        } while (result->NextRow());
    }

    // SmartAI creatures whose scripts only cast spells or talk, on combat events: casters and ability users
    // without scripts that flee, summon, despawn or change phases. Pack and gauntlet stages only.
    std::unordered_set<uint32> castOnlySmart;
    if (QueryResult result = WorldDatabase.Query("SELECT ss.entryorguid FROM smart_scripts ss "
        "JOIN creature_template ct ON ct.entry = ss.entryorguid AND ct.AIName = 'SmartAI' AND ct.ScriptName = '' "
        "WHERE ss.source_type = 0 AND ss.entryorguid > 0 GROUP BY ss.entryorguid "
        "HAVING SUM(ss.action_type NOT IN (1, 11)) = 0 "
        "AND SUM(ss.event_type NOT IN (0, 2, 3, 4, 5, 6, 8, 9, 12, 13, 14)) = 0 AND SUM(ss.action_type = 11) > 0"))
    {
        do
        {
            castOnlySmart.insert(uint32(result->Fetch()[0].Get<int32>()));
        } while (result->NextRow());
    }

    uint32 opponents = 0;
    uint32 packMembers = 0;
    uint32 elites = 0;
    std::map<uint32, std::vector<uint32>> beastsByFamily;
    for (auto const& [entry, info] : *sObjectMgr->GetCreatureTemplates())
    {
        if (!spawned.contains(entry))
            continue;

        if (info.IsTameable(false))
            beastsByFamily[info.family].push_back(entry);

        // Plain combat creatures with sane stat multipliers.
        if (!IsFairOpponentType(info.type) || info.npcflag || info.VehicleId || (info.unit_flags & UNUSABLE_UNIT_FLAGS)
            || (info.flags_extra & UNUSABLE_EXTRA_FLAGS) || info.ModHealth < 0.5f || info.ModHealth > 2.0f
            || info.DamageModifier < 0.5f || info.DamageModifier > 2.0f || !info.minlevel)
            continue;

        // Default AI: no SmartAI or C++ script that could summon, flee or despawn.
        bool const defaultAI = !info.ScriptID && info.AIName.empty();
        bool const castingAI = castOnlySmart.contains(entry);
        if (!defaultAI && !castingAI)
            continue;

        uint32 const maxLevel = std::min<uint32>(info.maxlevel, DEFAULT_MAX_LEVEL);
        if (info.rank == CREATURE_ELITE_NORMAL)
        {
            for (uint32 level = info.minlevel; level <= maxLevel; ++level)
            {
                if (defaultAI)
                    _byLevel[level].push_back(entry);
                _packByLevel[level].push_back(entry);
            }

            opponents += defaultAI ? 1 : 0;
            ++packMembers;
        }
        else if (info.rank == CREATURE_ELITE_ELITE)
        {
            for (uint32 level = info.minlevel; level <= maxLevel; ++level)
                _elitesByLevel[level].push_back(entry);

            ++elites;
        }
    }

    for (auto& [family, entries] : beastsByFamily)
        _beastsByFamily.push_back(std::move(entries));

    LOG_INFO("module.animus", "Duel arena: {} opponent creatures, {} pack creatures ({} casting), {} elites, {} "
        "tameable beast families", opponents, packMembers, castOnlySmart.size(), elites, _beastsByFamily.size());
}

uint32 AnimusForge::DuelArena::OpponentPool::PickNear(std::array<std::vector<uint32>, 81> const& byLevel, uint8 level)
{
    // The level itself, then the nearest levels either side.
    for (int32 offset = 0; offset <= DEFAULT_MAX_LEVEL; ++offset)
    {
        for (int32 candidate : { int32(level) - offset, int32(level) + offset })
        {
            if (candidate < 1 || candidate > DEFAULT_MAX_LEVEL || byLevel[candidate].empty())
                continue;

            std::vector<uint32> const& entries = byLevel[candidate];
            return entries[urand(0, uint32(entries.size()) - 1)];
        }
    }

    return 0;
}

uint32 AnimusForge::DuelArena::OpponentPool::Random(uint8 level) const
{
    return PickNear(_byLevel, level);
}

uint32 AnimusForge::DuelArena::OpponentPool::RandomPackMember(uint8 level) const
{
    return PickNear(_packByLevel, level);
}

uint32 AnimusForge::DuelArena::OpponentPool::RandomElite(uint8 level) const
{
    return PickNear(_elitesByLevel, level);
}

std::vector<uint32> AnimusForge::DuelArena::OpponentPool::RandomStable(uint32 count) const
{
    std::vector<uint32> families(_beastsByFamily.size());
    for (uint32 i = 0; i < families.size(); ++i)
        families[i] = i;

    std::vector<uint32> stable;
    while (stable.size() < count && !families.empty())
    {
        uint32 const pick = urand(0, uint32(families.size()) - 1);
        std::vector<uint32> const& entries = _beastsByFamily[families[pick]];
        stable.push_back(entries[urand(0, uint32(entries.size()) - 1)]);
        families.erase(families.begin() + pick);
    }

    return stable;
}

Position AnimusForge::DuelArena::FindSpawnPoint(Player* bot, Map* map)
{
    // A random bearing and distance; retry a few bearings for a spot in line of sight on roughly level
    // ground, so the opponent is reachable. The last try is used regardless.
    Position pos;
    for (uint32 attempt = 0; attempt < SPAWN_ATTEMPTS; ++attempt)
    {
        float const bearing = frand(0.0f, 2.0f * float(M_PI));
        float const distance = frand(SPAWN_DISTANCE_MIN, SPAWN_DISTANCE_MAX);

        pos.m_positionX = bot->GetPositionX() + distance * std::cos(bearing);
        pos.m_positionY = bot->GetPositionY() + distance * std::sin(bearing);
        pos.m_positionZ = bot->GetPositionZ();

        float const ground = map->GetHeight(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 5.0f);
        if (ground <= INVALID_HEIGHT)
            continue;

        pos.m_positionZ = ground;
        if (std::fabs(ground - bot->GetPositionZ()) < MAX_HEIGHT_DIFFERENCE
            && bot->IsWithinLOS(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 2.0f))
            break;
    }

    // A random facing, so the bot has to learn to get behind it.
    pos.SetOrientation(frand(0.0f, 2.0f * float(M_PI)));
    return pos;
}

Creature* AnimusForge::DuelArena::SummonOpponent(Player* bot, Map* map, uint32 entry, Position const& pos, uint8 level)
{
    PendingSummonLevel = level;
    TempSummon* opponent = map->SummonCreature(entry, pos);
    PendingSummonLevel = 0;

    if (!opponent)
    {
        LOG_ERROR("module.animus", "Could not summon opponent {} for bot {}", entry, bot->GetName());
        return nullptr;
    }

    opponent->SetFaction(FACTION_MONSTER);
    opponent->SetReactState(REACT_AGGRESSIVE);
    opponent->SetHomePosition(pos);
    opponent->SetFullHealth();

    return opponent;
}

Creature* AnimusForge::DuelArena::SpawnOpponent(Player* bot, Map* map, uint32 entry)
{
    return SummonOpponent(bot, map, entry, FindSpawnPoint(bot, map), bot->GetLevel());
}

std::vector<Creature*> AnimusForge::DuelArena::SpawnPack(Player* bot, Map* map, std::vector<uint32> const& entries,
    uint8 level)
{
    Position const center = FindSpawnPoint(bot, map);

    std::vector<Creature*> pack;
    for (uint32 i = 0; i < entries.size(); ++i)
    {
        // Loosely clustered around the center, each facing its own way.
        Position pos = center;
        if (i)
        {
            float const angle = frand(0.0f, 2.0f * float(M_PI));
            float const offset = frand(2.0f, PACK_SPREAD);
            pos.m_positionX += offset * std::cos(angle);
            pos.m_positionY += offset * std::sin(angle);

            float const ground = map->GetHeight(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 5.0f);
            if (ground > INVALID_HEIGHT)
                pos.m_positionZ = ground;

            pos.SetOrientation(frand(0.0f, 2.0f * float(M_PI)));
        }

        if (Creature* member = SummonOpponent(bot, map, entries[i], pos, level))
            pack.push_back(member);
    }

    return pack;
}

AnimusForge::DuelArena::ConsumablePool const& AnimusForge::DuelArena::ConsumablePool::Instance()
{
    static ConsumablePool const pool;
    return pool;
}

AnimusForge::DuelArena::ConsumablePool::ConsumablePool()
{
    std::unordered_set<uint32> sold;
    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT CAST(item AS SIGNED) FROM npc_vendor"))
    {
        do
        {
            if (int64 const item = result->Fetch()[0].Get<int64>(); item > 0)
                sold.insert(uint32(item));
        } while (result->NextRow());
    }

    for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (proto.Class != ITEM_CLASS_CONSUMABLE || proto.SubClass != ITEM_SUBCLASS_FOOD || !sold.contains(itemId)
            || proto.RequiredSkill || proto.RequiredReputationFaction)
            continue;

        _Spell const& use = proto.Spells[0];
        SpellInfo const* info = use.SpellId > 0 && use.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE
            ? sSpellMgr->GetSpellInfo(use.SpellId) : nullptr;
        if (!info)
            continue;

        uint8 const level = uint8(std::min<uint32>(proto.RequiredLevel, DEFAULT_MAX_LEVEL));
        if (info->HasAura(SPELL_AURA_MOD_REGEN))
            _food.emplace_back(level, itemId);
        else if (info->HasAura(SPELL_AURA_MOD_POWER_REGEN))
            _drink.emplace_back(level, itemId);
    }

    std::sort(_food.begin(), _food.end());
    std::sort(_drink.begin(), _drink.end());

    LOG_INFO("module.animus", "Consumables: {} foods, {} drinks sold by vendors", _food.size(), _drink.size());
}

uint32 AnimusForge::DuelArena::ConsumablePool::Best(std::vector<std::pair<uint8, uint32>> const& items, uint8 level)
{
    uint32 best = 0;
    for (auto const& [reqLevel, itemId] : items)
        if (reqLevel <= level)
            best = itemId;

    return best;
}

bool AnimusForge::DuelArena::CallHunterBeast(Player* bot, uint32 entry)
{
    if (bot->getClass() != CLASS_HUNTER || bot->GetPetGUID() || bot->GetLevel() < HUNTER_PET_LEVEL || !entry)
        return false;

    Pet* pet = bot->CreateTamedPetFrom(entry, SPELL_TAME_BEAST);
    if (!pet)
        return false;

    // Spell::EffectTameCreature without the database save: the bot and its pet are never saved.
    pet->SetUInt32Value(UNIT_FIELD_LEVEL, bot->GetLevel());
    pet->GetMap()->AddToMap(pet->ToCreature(), true);
    bot->SetMinion(pet, true);
    pet->InitTalentForLevel();
    bot->PetSpellInitialize();
    return true;
}

bool AnimusForge::DuelArena::PetAttack(Player* bot, Unit* target)
{
    bool ordered = false;
    for (Unit* controlled : bot->m_Controlled)
    {
        Creature* pet = controlled->ToCreature();
        if (!pet || !pet->IsAlive() || !pet->IsAIEnabled || pet->GetVictim() == target
            || !pet->CanCreatureAttack(target))
            continue;

        // HandlePetActionHelper, COMMAND_ATTACK.
        pet->ClearUnitState(UNIT_STATE_FOLLOW);
        pet->AttackStop();
        if (CharmInfo* charmInfo = pet->GetCharmInfo())
        {
            charmInfo->SetIsCommandAttack(true);
            charmInfo->SetIsAtStay(false);
            charmInfo->SetIsFollowing(false);
            charmInfo->SetIsCommandFollow(false);
            charmInfo->SetIsReturning(false);
        }

        pet->AI()->AttackStart(target);
        ordered = true;
    }

    return ordered;
}
