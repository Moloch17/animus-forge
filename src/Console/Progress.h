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

#ifndef MOD_ANIMUS_FORGE_PROGRESS_H
#define MOD_ANIMUS_FORGE_PROGRESS_H

#include "Define.h"
#include "TextTable.h"
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AnimusForge
{
    struct ForgeConfig;

    /// runs/<run>/progress.json as the learner last wrote it (python/animus/progress.py): one flat JSON object of
    /// numbers, strings and nulls.
    class ProgressFile
    {
    public:
        /// False when the file is missing or is not a flat JSON object.
        bool Load(std::filesystem::path const& path);

        /// Parse a flat JSON object. Nested objects and arrays are rejected; null values are left out.
        bool Parse(std::string const& text);

        [[nodiscard]] std::optional<double> Number(std::string const& key) const;
        [[nodiscard]] std::string Text(std::string const& key) const;

    private:
        std::unordered_map<std::string, double> _numbers;
        std::unordered_map<std::string, std::string> _strings;
    };

    /// What the sim knows about the running scenario, gathered by Forge for a report.
    struct SimSnapshot
    {
        std::string Scenario;
        std::string State;                  // "training", "paused", "running greedy", ...
        uint32 PlanPosition = 0;            // 1-based
        uint32 PlanSize = 0;
        bool Remote = true;
        uint32 Envs = 0;
        uint32 AgentsPerEnv = 0;
        uint64 Decisions = 0;
        uint64 Episodes = 0;
        uint64 EpisodeLimit = 0;            // local runs: stop after this many episodes (0 = none)
        double ScenarioSeconds = 0.0;       // wall time since the scenario started
        double TicksPerSecond = 0.0;        // sim ticks per wall second over the last interval
        double EpisodesPerSecond = 0.0;     // over the last interval
        bool LearnerRunning = false;
        int32 LearnerPid = -1;
        bool LearnerConnected = false;
        bool LearnerFailed = false;
        double SecondsSinceAct = -1.0;      // < 0: no ACT yet from this learner
        uint64 EpisodeMeansCount = 0;
        std::vector<std::pair<std::string, double>> EpisodeMeans;
    };

    /// One scenario of the current plan, for the plan table.
    struct PlanRow
    {
        std::string Scenario;
        std::string Status;                 // "done", "training", "pending", "skipped", "failed", ...
        bool Current = false;
        bool Resume = false;
    };

    /// Builds the progress report: `forge status` and the periodic report while a scenario runs.
    ///
    /// Remembers the previous periodic report of the running scenario for trends, the step rate for ETAs and the
    /// first entropy for collapse warnings. Only a periodic report advances that state, so `forge status` in
    /// between does not shorten the trend window.
    class ProgressMonitor
    {
    public:
        /// A new scenario started (or resumed): forget the previous one's trends.
        void Begin(std::string const& scenario);

        /// Write the report. Warnings go to `warn` (one line each), everything else to `info`.
        void Report(ForgeConfig const& config, SimSnapshot const& sim, std::vector<PlanRow> const& plan,
            LineSink const& info, LineSink const& warn, bool periodic);

    private:
        struct Sample
        {
            double UnixTime = 0.0;
            double EnvSteps = 0.0;
        };

        struct Previous
        {
            std::optional<double> Reward;
            std::optional<double> Entropy;
            std::optional<double> Rate;
            std::optional<double> Score;
            std::optional<double> TicksPerSecond;
        };

        [[nodiscard]] std::optional<double> StepRate(ProgressFile const& progress) const;
        void Advance(ProgressFile const& progress);

        void ReportTraining(ForgeConfig const& config, SimSnapshot const& sim, ProgressFile const* progress,
            LineSink const& info, std::vector<std::string>& warnings) const;
        void ReportLocal(SimSnapshot const& sim, LineSink const& info) const;
        void ReportPlan(ForgeConfig const& config, std::vector<PlanRow> const& plan, ProgressFile const* current,
            LineSink const& info) const;

        std::string _scenario;
        std::optional<Sample> _lastSample;
        std::optional<double> _rateEma;
        std::optional<double> _peakRate;
        std::optional<double> _firstEntropy;
        Previous _previous;
    };

    /// runs/<scenario>/progress.json under the learner directory.
    std::filesystem::path ProgressPath(ForgeConfig const& config, std::string const& scenario);

    /// total_env_steps a scenario's learner will train for: the last --set total_env_steps= in
    /// AnimusForge.Learner.Args, else the key in its YAML config. Empty when neither says.
    std::optional<uint64> ConfiguredTotalEnvSteps(ForgeConfig const& config, std::string const& scenario);
}

#endif
