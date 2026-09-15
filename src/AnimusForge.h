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

#ifndef MOD_ANIMUS_FORGE_H
#define MOD_ANIMUS_FORGE_H

#include "ChildProcess.h"
#include "EnvPool.h"
#include "ForgeConfig.h"
#include "LearnerProcess.h"
#include "LockstepServer.h"
#include "Progress.h"
#include "Scenario.h"
#include "TextTable.h"
#include <chrono>
#include <memory>
#include <optional>

namespace AnimusForge
{
    /// Module root: owns the scenario, the env pool and the learner connection, and runs one decision step every
    /// AnimusForge.DecisionTicks world ticks while a plan runs.
    ///
    /// The sim starts idle; console commands (Hooks/ForgeCommandScript.cpp) start, pause, resume, skip and cancel
    /// plans. A command only records a request: OnUpdate applies it at the start of a tick, never in the middle of
    /// a decision. While the world thread waits on the learner (accepting it, or waiting for its ACT) it keeps
    /// running console commands, so the console answers within a fraction of a second throughout.
    class Forge
    {
    public:
        static Forge* Instance();

        void OnStartup();
        void OnUpdate(uint32 diff);
        void OnShutdown();

        /// The running pool, or nullptr when no scenario is running. Set and cleared on the world thread while no
        /// map is updating, so map-thread hooks may read it without a lock.
        [[nodiscard]] EnvPool* ActivePool() const { return _poolLive ? _pool.get() : nullptr; }

        /// Console commands, run on the world thread. Each writes its reply to `out` and returns false when it
        /// refuses (the reply says why).
        void CommandStatus(LineSink const& out);
        void CommandScenarios(LineSink const& out);
        bool CommandStart(std::vector<std::string> scenarios, LineSink const& out);
        bool CommandFast(std::vector<std::string> scenarios, LineSink const& out);
        bool CommandResume(std::vector<std::string> scenarios, LineSink const& out);
        bool CommandPause(LineSink const& out);
        bool CommandCancel(LineSink const& out);
        bool CommandSkip(LineSink const& out);
        bool CommandRun(std::string const& scenario, std::string const& policy, uint32 episodes, LineSink const& out);
        bool CommandExport(std::string scenario, std::string const& checkpoint, LineSink const& out);
        bool CommandClean(std::string const& target, std::string const& scenario, LineSink const& out);
        void CommandProgress(std::optional<uint32> seconds, LineSink const& out);

    private:
        enum class State : uint8
        {
            Idle,
            Training,       // remote policy: lock-step with the learner
            Running,        // local policy: scripted or random actions
            Paused,
        };

        enum class Request : uint8
        {
            None,
            Start,
            Cancel,
            Skip,
        };

        struct PlanEntry
        {
            std::string Scenario;
            bool Resume = false;
            std::string Outcome;            // empty while pending or running; "done", "skipped", "failed", ...
        };

        /// Scenarios run one after another.
        struct Plan
        {
            std::vector<PlanEntry> Entries;
            uint32 Index = 0;
            std::string Policy;             // "remote" trains; anything else runs locally
            uint64 LocalEpisodes = 0;       // local policy: episodes per scenario (0 = until cancelled)
            bool Fast = false;              // `forge fast`: trained with ForgeConfig::FastProfile

            [[nodiscard]] bool Remote() const { return Policy == "remote"; }
        };

        Forge() = default;

        void ApplyRequest();
        void HoldWhilePaused();

        /// Build and start the plan's current scenario. False (logged) when it cannot start.
        bool StartCurrent();

        /// Tear the running scenario down. With `stopLearner` the learner is disconnected and waited for (it saves
        /// latest.pt when the socket closes); a learner that finished by itself is already gone.
        void TeardownScenario(bool stopLearner);

        /// The current scenario ended with `outcome`: tear it down and start the next one, or finish the plan.
        void FinishCurrent(std::string const& outcome);

        /// The plan stops here (finished, cancelled or failed); the sim goes idle.
        void EndPlan(char const* reason);

        /// The running scenario's auto-started learner finished its run and moved on (exit 0: converged and past its
        /// target, or at its step limit).
        [[nodiscard]] bool LearnerFinished() const;

        /// The running scenario's auto-started learner stopped below its stage target after its restarts (exit 3).
        [[nodiscard]] bool LearnerHalted() const;

        /// AnimusForge.Queue, or every class/role stage in order when it is empty.
        [[nodiscard]] std::vector<std::string> DefaultQueue() const;

        /// The run of `scenario` finished and moved on (<RunsDir>/<scenario>/finished.json with "advanced": true, or
        /// a finished.json from before stage targets).
        [[nodiscard]] bool RunAdvanced(ForgeConfig const& config, std::string const& scenario) const;

        /// Warn about stages listed before the stage they extend and seed from (unless that one already advanced).
        void WarnSeedOrder(ForgeConfig const& config, std::vector<std::string> const& scenarios,
            LineSink const& out) const;

        /// The settings a plan runs with: the configured ones, or the fast profile for `forge fast`.
        [[nodiscard]] ForgeConfig const& ConfigFor(Plan const& plan) const { return plan.Fast ? _fastConfig : _config; }

        /// The settings of the running (or last started) plan.
        [[nodiscard]] ForgeConfig const& RunConfig() const { return ConfigFor(_plan); }

        /// Console commands, the export process and the periodic report, while the world thread waits.
        void Pump();
        void PollExport();
        void MaybeReport();

        void LocalDecision();
        void RemoteDecision();
        bool SendSpec();
        bool SendStep();
        bool ApplyMode(ModeMsg const& mode);

        [[nodiscard]] SimSnapshot Snapshot(bool advanceRates);
        [[nodiscard]] std::vector<PlanRow> PlanRows(Plan const& plan, bool live) const;
        [[nodiscard]] std::string StateName() const;
        [[nodiscard]] bool Enabled(LineSink const& out) const;
        [[nodiscard]] bool ValidScenario(std::string const& scenario, LineSink const& out) const;

        ForgeConfig _config;
        ForgeConfig _fastConfig;            // _config.FastProfile()
        std::unique_ptr<Scenario> _scenario;
        std::unique_ptr<EnvPool> _pool;
        LockstepServer _server;
        LearnerProcess _learner;
        ChildProcess _export{ "Export" };
        ProgressMonitor _monitor;

        State _state = State::Idle;
        State _pausedFrom = State::Idle;
        bool _poolLive = false;
        bool _pauseRequested = false;
        bool _resumeRequested = false;
        bool _pumping = false;
        bool _learnerStarted = false;      // the auto-started learner belongs to the running scenario

        Request _request = Request::None;
        Plan _requested;
        Plan _plan;
        std::optional<Plan> _lastPlan;     // the last plan that ended, for `forge resume` without arguments

        uint64 _ticks = 0;
        uint64 _decisions = 0;
        uint32 _tickMs = 0;
        uint32 _progressInterval = 60;

        std::chrono::steady_clock::time_point _scenarioStarted;
        std::chrono::steady_clock::time_point _lastReport;
        std::optional<std::chrono::steady_clock::time_point> _lastAct;

        std::chrono::steady_clock::time_point _rateTime;
        uint64 _rateTicks = 0;
        uint64 _rateEpisodes = 0;
        double _ticksPerSecond = 0.0;
        double _episodesPerSecond = 0.0;

        std::string _exportScenario;
        std::string _exportModelDir;

        /// The scenario running, or the last one started.
        std::string _current;
    };
}

#define sAnimusForge AnimusForge::Forge::Instance()

#endif
