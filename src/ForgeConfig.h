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

#ifndef MOD_ANIMUS_FORGE_CONFIG_H
#define MOD_ANIMUS_FORGE_CONFIG_H

#include "Define.h"
#include "Position.h"
#include <string>

namespace AnimusForge
{
    /// Module settings, read once at startup (mod_animus_forge.conf.dist documents every key).
    struct ForgeConfig
    {
        bool Enable = true;

        /// Scenario started automatically when the server starts.
        std::string Scenario;

        uint32 Envs = 64;
        uint32 DecisionTicks = 1;
        uint32 EpisodeSeconds = 60;

        std::string Policy;
        uint32 HsRageThreshold = 15;
        uint32 ReportEpisodes = 256;
        std::string SocketPath;

        /// Remote policy only: start the Python learner as a child process once the socket is up.
        bool LearnerAutoStart = true;
        std::string LearnerPython;      // resolved: never empty after Load
        std::string LearnerWorkDir;     // resolved: never empty after Load
        std::string LearnerConfig;      // resolved: never empty after Load
        std::string LearnerLogFile;     // resolved: never empty after Load

        uint32 ArenaMapId = 560;
        Position ArenaPosition;

        [[nodiscard]] bool IsRemote() const { return Policy == "remote"; }

        void Load();
    };
}

#endif
