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
    /// Module settings, read once at startup (mod_animus_forge.conf.dist documents every key). Only settings: what is
    /// running lives in Forge. The class/role curriculum's tuning is ClassRoleTuning.
    struct ForgeConfig
    {
        bool Enable = true;

        /// AnimusForge.Queue: scenarios trained one after another, each until its learner finishes. Empty = every
        /// class/role stage, first to last.
        std::vector<std::string> Queue;

        /// AnimusForge.Queue.SkipFinished: skip queued scenarios whose run already finished
        /// (<RunsDir>/<name>/finished.json).
        bool QueueSkipFinished = true;

        /// AnimusForge.Queue.LocalEpisodes: with a local policy, episodes per queued scenario (0 = run the
        /// first one forever).
        uint32 QueueLocalEpisodes = 0;

        uint32 Envs = 64;
        uint32 DecisionTicks = 2;
        uint32 EpisodeSeconds = 60;

        std::string Policy;
        uint32 ReportEpisodes = 256;
        std::string SocketPath;

        /// AnimusForge.OutputDir, resolved: where runs/ and layouts/ go. Never empty after Load.
        std::string OutputDir;

        /// Remote policy only: start the Python learner as a child process once the socket is up.
        bool LearnerAutoStart = true;
        std::string LearnerPython;      // resolved: never empty after Load
        std::string LearnerWorkDir;     // resolved: never empty after Load
        std::string LearnerConfig;      // AnimusForge.Learner.Config; empty = configs/<scenario>.yaml
        std::string LearnerLogFile;     // resolved: never empty after Load
        std::vector<std::string> LearnerArgs;   // AnimusForge.Learner.Args, split on whitespace
        std::vector<std::string> ClassRoles;    // AnimusForge.ClassRoles; empty = every class/role

        /// AnimusForge.SpawnPoint.*: the instanceable map and position every env's bots start at.
        uint32 SpawnMapId = 560;
        Position SpawnPosition;

        /// AnimusForge.WarriorDummy20.HsRageThreshold: rage at which warrior_dummy_20's scripted policies queue
        /// Heroic Strike.
        uint32 WarriorDummy20HsRageThreshold = 15;

        [[nodiscard]] bool IsRemote() const { return Policy == "remote"; }

        /// Where learners train: <OutputDir>/runs.
        [[nodiscard]] std::string RunsDir() const;

        /// Where scenarios write their layout manifests and stage descriptions: <OutputDir>/layouts.
        [[nodiscard]] std::string LayoutsDir() const;

        /// Absolute learner config for a scenario: AnimusForge.Learner.Config if set, else configs/<scenario>.yaml.
        [[nodiscard]] std::string LearnerConfigFor(std::string const& scenario) const;

        void Load();
    };
}

#endif
