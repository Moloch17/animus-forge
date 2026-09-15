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
#include <filesystem>
#include <string>
#include <vector>

namespace AnimusForge
{
    /// Game milliseconds per world tick: the forge core's fixed sim tick (ForgeUpdateLoop in ForgeMain.cpp).
    constexpr uint32 SIM_TICK_MS = 50;

    /// Module settings, read once at startup (mod_animus_forge.conf.dist documents every key). Only settings: what is
    /// running lives in Forge. The class/role curriculum's tuning is ClassRoleTuning.
    struct ForgeConfig
    {
        bool Enable = true;

        /// AnimusForge.Queue: the scenarios `forge start` trains, one after another, when given none. Empty =
        /// every class/role stage, first to last.
        std::vector<std::string> Queue;

        /// AnimusForge.Queue.SkipFinished: `forge start` without scenarios skips those whose run already finished
        /// and moved on (<RunsDir>/<name>/finished.json with "advanced": true).
        bool QueueSkipFinished = true;

        /// AnimusForge.Queue.LocalEpisodes: with a local policy, episodes per scenario of `forge start` (0 = run
        /// the first one until cancelled).
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

        /// AnimusForge.ModelDir, resolved: where `forge export` writes models. Never empty after Load.
        std::string ModelDir;
        /// AnimusForge.Progress.Interval, seconds; 0 = no periodic report.
        uint32 ProgressInterval = 60;

        /// AnimusForge.Fast.*: the low-resolution profile `forge fast` trains with (see FastProfile).
        uint32 FastEnvs = 16;
        uint32 FastDecisionTicks = 4;
        uint32 FastEpisodeSeconds = 30;
        std::vector<std::string> FastClassRoles;    // empty = AnimusForge.ClassRoles
        std::string FastOutputDir;                  // resolved: never empty after Load
        std::string FastLearnerOverlay;             // resolved: never empty after Load
        std::vector<std::string> FastLearnerArgs;

        [[nodiscard]] bool IsRemote() const { return Policy == "remote"; }

        /// These settings with the fast profile applied: fewer envs, coarser decisions, shorter episodes, fewer
        /// class/roles, and the learner's small budgets (FastLearnerOverlay). Everything goes to FastOutputDir (runs,
        /// layouts and models), so a test run never archives, seeds from or overwrites a real run.
        [[nodiscard]] ForgeConfig FastProfile() const;

        /// Where learners train: <OutputDir>/runs, runs/<scenario>/ per scenario.
        [[nodiscard]] std::filesystem::path RunsDir() const;

        /// Where scenarios write their layout manifests and stage descriptions: <OutputDir>/layouts.
        [[nodiscard]] std::filesystem::path LayoutsDir() const;

        /// Absolute learner config for a scenario: AnimusForge.Learner.Config if set, else configs/<scenario>.yaml.
        [[nodiscard]] std::string LearnerConfigFor(std::string const& scenario) const;

        void Load();
    };
}

#endif
