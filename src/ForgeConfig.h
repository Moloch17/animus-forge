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
#include "StageSettings.h"
#include <filesystem>
#include <string>
#include <vector>

namespace AnimusForge
{
    /// Module settings, read once at startup (mod_animus_forge.conf.dist documents every key). Only settings: what is
    /// running lives in Forge. The curriculum's tuning is CurriculumTuning.
    struct ForgeConfig
    {
        bool Enable = true;

        /// AnimusForge.Queue: the scenarios `forge start` trains, one after another, when given none. Empty =
        /// every curriculum stage, first to last.
        std::vector<std::string> Queue;

        /// AnimusForge.Queue.SkipFinished: `forge start` without scenarios skips those whose run already finished
        /// and moved on (<RunsDir>/<name>/finished.json with "advanced": true).
        bool QueueSkipFinished = true;

        /// AnimusForge.Queue.LocalEpisodes: with a local policy, episodes per scenario of `forge start` (0 = run
        /// the first one until cancelled).
        uint32 QueueLocalEpisodes = 0;

        uint32 Envs = 64;
        /// AnimusForge.DecisionMs: game time per decision, which is also the forge core's world tick (ForgeUpdateLoop in
        /// ForgeMain.cpp reads the same key): every world update is one decision.
        uint32 DecisionMs = 100;
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

        /// AnimusForge.ModelDir, resolved: where `forge export` writes models. Never empty after Load.
        std::string ModelDir;
        /// AnimusForge.Progress.Interval, seconds; 0 = no periodic report (`forge status` and each stage's end only).
        uint32 ProgressInterval = 0;

        /// The level every character is, or 0 for the curriculum's random levels (AnimusForge.Curriculum.Characters.*).
        /// Only the fast profile sets it (AnimusForge.Fast.Level).
        uint32 Level = 0;

        /// AnimusForge.Fast.*: the low-resolution profile `forge fast` trains with (see FastProfile).
        uint32 FastEnvs = 16;
        uint32 FastLevel = 20;
        /// AnimusForge.Fast.Queue: what `forge fast` trains when given no scenarios; empty = AnimusForge.Queue.
        std::vector<std::string> FastQueue;
        std::vector<std::string> FastClassRoles;    // empty = AnimusForge.ClassRoles
        std::string FastOutputDir;                  // resolved: never empty after Load
        std::string FastLearnerOverlay;             // resolved: never empty after Load
        std::vector<std::string> FastLearnerArgs;

        [[nodiscard]] bool IsRemote() const { return Policy == "remote"; }

        /// What the scenario and its env pool take from these settings (animus-lib's StageSettings).
        [[nodiscard]] Animus::StageSettings Stage() const;

        /// These settings with the fast profile applied: fewer envs, a few class/roles at one level, and the learner's
        /// quick convergence settings (FastLearnerOverlay). Everything goes to FastOutputDir (runs,
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
