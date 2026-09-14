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
#include <vector>

namespace AnimusForge
{
    /// Module settings, read once at startup (mod_animus_forge.conf.dist documents every key).
    struct ForgeConfig
    {
        bool Enable = true;

        /// Scenario currently running: AnimusForge.Scenario, or the current entry of Queue.
        std::string Scenario;

        /// AnimusForge.Queue: scenarios trained one after another, each until its learner finishes.
        /// Empty = run Scenario only.
        std::vector<std::string> Queue;

        /// AnimusForge.Queue.LocalEpisodes: with a local policy, episodes per queued scenario (0 = run the
        /// first one forever).
        uint32 QueueLocalEpisodes = 0;

        uint32 Envs = 64;
        uint32 DecisionTicks = 2;
        uint32 EpisodeSeconds = 60;

        std::string Policy;
        uint32 HsRageThreshold = 15;
        uint32 ReportEpisodes = 256;
        std::string SocketPath;

        /// Remote policy only: start the Python learner as a child process once the socket is up.
        bool LearnerAutoStart = true;
        std::string LearnerPython;      // resolved: never empty after Load
        std::string LearnerWorkDir;     // resolved: never empty after Load
        std::string LearnerConfig;      // AnimusForge.Learner.Config; empty = per scenario (LearnerConfigFor)
        std::string LearnerLogFile;     // resolved: never empty after Load
        std::vector<std::string> LearnerArgs;   // AnimusForge.Learner.Args, split on whitespace
        std::vector<std::string> ClassRoles;    // AnimusForge.ClassRoles; empty = every class/role

        uint32 ArenaMapId = 560;
        Position ArenaPosition;

        [[nodiscard]] bool IsRemote() const { return Policy == "remote"; }

        /// Absolute learner config for a scenario: AnimusForge.Learner.Config if set, else
        /// configs/<scenario>.yaml if it exists, else configs/class_role.yaml.
        [[nodiscard]] std::string LearnerConfigFor(std::string const& scenario) const;

        void Load();
    };
}

#endif
