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

#include "AnimusConfig.h"
#include "Config.h"
#include <algorithm>

void Animus::ForgeConfig::Load()
{
    Enable = sConfigMgr->GetOption<bool>("AnimusForge.Enable", true);

    Scenario = sConfigMgr->GetOption<std::string>("AnimusForge.Scenario", "warrior_dummy");

    Envs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Envs", 64));
    DecisionTicks = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.DecisionTicks", 1));
    EpisodeSeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.EpisodeSeconds", 60));

    Policy = sConfigMgr->GetOption<std::string>("AnimusForge.Policy", "remote");
    HsRageThreshold = sConfigMgr->GetOption<uint32>("AnimusForge.HsRageThreshold", 15);
    ReportEpisodes = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.ReportEpisodes", 256));
    SocketPath = sConfigMgr->GetOption<std::string>("AnimusForge.Socket", "/tmp/animus-forge.sock");

    ArenaMapId = sConfigMgr->GetOption<uint32>("AnimusForge.Arena.MapId", 560);
    ArenaPosition.Relocate(
        sConfigMgr->GetOption<float>("AnimusForge.Arena.X", 2741.9f),
        sConfigMgr->GetOption<float>("AnimusForge.Arena.Y", 1315.2f),
        sConfigMgr->GetOption<float>("AnimusForge.Arena.Z", 14.0f),
        sConfigMgr->GetOption<float>("AnimusForge.Arena.O", 2.96f));
}
