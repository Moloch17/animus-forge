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
 * Bot lifecycle without a client or a character row.
 *
 * Mirrors the create path (CharacterHandler: new Player -> MotionMaster::Initialize -> Create) and
 * the in-world half of the login path (HandlePlayerLoginFromDB: SetPlayer -> SetMover ->
 * ObjectAccessor::AddObject -> Map::AddPlayerToMap), skipping every step that reads or writes the
 * character database and every step whose only effect is a packet to the client.
 *
 * What LoadFromDB sets up that Create does not, and how it is handled here:
 *   - bound-instance storage: PlayerCreateBoundInstancesMaps, else PlayerBindToInstance crashes
 *   - stat modification:      SetCanModifyStats(true) + UpdateAllStats, else gear adds nothing
 *   - social list:            left null; only packet handlers (invites, channels) read it, and
 *                             SendInitialPacketsBeforeAddToMap -- the one login step that does --
 *                             is skipped because it only builds packets
 */

#include "ForgeBotFactory.h"
#include "GameTime.h"
#include "InstanceSaveMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldSession.h"

namespace
{
    /// CharacterCreateInfo keeps its fields protected (it is filled from CMSG_CHAR_CREATE); a
    /// derived type may set them.
    class BotCreateInfo : public CharacterCreateInfo
    {
    public:
        explicit BotCreateInfo(AnimusForge::BotFactory::BotSpec const& spec)
        {
            Name = spec.Name;
            Race = spec.Race;
            Class = spec.Class;
            Gender = spec.Gender;
        }
    };
}

Player* AnimusForge::BotFactory::Create(BotSpec const& spec)
{
    // accountFlags 0: no collector's edition voucher mail (Player::Create's only DB write).
    WorldSession* session = new WorldSession(spec.AccountId, std::string(spec.Name), 0, nullptr, SEC_PLAYER,
        EXPANSION_WRATH_OF_THE_LICH_KING, 0, LOCALE_enUS, 0, false, false, 0);

    // Default permissions for the security level, in memory. Must precede new Player, whose
    // constructor checks a permission and would otherwise run a sync login DB query.
    session->InitRBACDataForTest();

    Player* bot = new Player(session);
    bot->GetMotionMaster()->Initialize();

    BotCreateInfo info(spec);
    if (!bot->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &info))
    {
        LOG_ERROR("module.animus", "Player::Create failed for bot {} (race {}, class {})", spec.Name, spec.Race,
            spec.Class);
        delete bot;
        delete session;
        return nullptr;
    }

    sInstanceSaveMgr->PlayerCreateBoundInstancesMaps(bot->GetGUID());
    session->SetPlayer(bot);

    // Never save: 0 disables the autosave countdown in Player::Update.
    bot->SetSaveTimer(0);

    // SetLevel, not GiveLevel: GiveLevel sends level-reward mail.
    if (spec.Level && bot->GetLevel() != spec.Level)
    {
        bot->SetLevel(spec.Level, false);
        bot->InitStatsForLevel(true);
        bot->InitTalentForLevel();

        // Raise weapon and defense skill caps to the new level (5 per level), then fill them, as
        // a character who levelled normally would have. Left at level 1 values, every swing would
        // roll against a skill of 5 and mostly miss.
        bot->UpdateSkillsForLevel();
        bot->UpdateSkillsToMaxSkillsForLevel();
    }

    bot->SetCanModifyStats(true);
    bot->UpdateAllStats();
    bot->SetFullHealth();

    return bot;
}

Map* AnimusForge::BotFactory::PlaceInNewInstance(Player* bot, uint32 mapId, Position const& pos)
{
    // A groupless player with no bind for this map always gets a brand new instance.
    Map* map = sMapMgr->CreateMap(mapId, bot);
    if (!map || !map->IsDungeon())
    {
        LOG_ERROR("module.animus", "Map {} did not produce a dungeon instance for bot {}", mapId, bot->GetName());
        return nullptr;
    }

    // Player::Create parked the bot on its race's start continent; move it before entering.
    bot->ResetMap();
    bot->Relocate(pos);
    bot->SetMap(map);
    bot->SetFallInformation(GameTime::GetGameTime().count(), pos.GetPositionZ());
    bot->SetMover(bot);

    ObjectAccessor::AddObject(bot);

    if (!map->AddPlayerToMap(bot))
    {
        LOG_ERROR("module.animus", "Could not add bot {} to map {} instance {}", bot->GetName(), mapId,
            map->GetInstanceId());
        ObjectAccessor::RemoveObject(bot);
        return nullptr;
    }

    return map;
}

void AnimusForge::BotFactory::Destroy(Player* bot)
{
    WorldSession* session = bot->GetSession();
    ObjectGuid const guid = bot->GetGUID();
    uint32 const mapId = bot->GetMapId();
    Difficulty const difficulty = bot->GetMap()->GetDifficulty();

    // A dead bot would be repopped at a graveyard (a far teleport) by LogoutPlayer.
    if (!bot->IsAlive())
        bot->ResurrectPlayer(1.0f);

    // Removes the player from its map and deletes it; false = no SaveToDB.
    session->LogoutPlayer(false);

    sInstanceSaveMgr->PlayerUnbindInstance(guid, mapId, difficulty, true);

    delete session;
}
