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

#include "AnimusForge.h"
#include "Log.h"
#include "StageDefinition.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <cstring>
#include <thread>

namespace
{
    /// How long an idle or paused world thread sleeps per tick: an idle sim would otherwise spin a core.
    constexpr std::chrono::milliseconds IDLE_SLEEP{ 50 };

    /// How long a cancelled or skipped learner gets to save its checkpoint before it is interrupted.
    constexpr std::chrono::seconds LEARNER_STOP_GRACE{ 15 };

    void LogInfo(std::string const& line)
    {
        LOG_INFO("module.animus", "{}", line);
    }

    void LogWarn(std::string const& line)
    {
        LOG_WARN("module.animus", "{}", line);
    }
}

AnimusForge::Forge* AnimusForge::Forge::Instance()
{
    static Forge instance;
    return &instance;
}

void AnimusForge::Forge::OnStartup()
{
    _config.Load();
    _fastConfig = _config.FastProfile();
    _progressInterval = _config.ProgressInterval;

    if (!_config.Enable)
    {
        LOG_INFO("module.animus", "Animus Forge is disabled (AnimusForge.Enable = 0)");
        return;
    }

    // Open the socket now, so a learner started by hand can connect as soon as a plan starts.
    if (_config.IsRemote())
        _server.Listen(_config.SocketPath);

    LOG_INFO("module.animus", "Animus Forge is idle. Type `forge start` on the console to train AnimusForge.Queue, "
        "or `forge help` for every command.");
    CommandStatus(LogInfo);
}

void AnimusForge::Forge::OnUpdate(uint32 diff)
{
    if (!_config.Enable)
        return;

    PollExport();
    ApplyRequest();

    if (_pauseRequested && (_state == State::Training || _state == State::Running))
    {
        _pauseRequested = false;
        _pausedFrom = _state;
        _state = State::Paused;
        LOG_INFO("module.animus", "Paused {}: the sim is frozen{}. `forge resume` continues, `forge cancel` stops.",
            _current, _plan.Remote() ? " and the learner waits" : "");
    }

    if (_state == State::Paused)
    {
        HoldWhilePaused();
        return;
    }

    if (_state == State::Idle)
    {
        std::this_thread::sleep_for(IDLE_SLEEP);
        return;
    }

    _tickMs = diff;
    ++_ticks;
    _pool->AdvanceClock(diff);

    MaybeReport();

    if (_ticks % RunConfig().DecisionTicks)
        return;

    if (_plan.Remote())
        RemoteDecision();
    else
        LocalDecision();
}

void AnimusForge::Forge::OnShutdown()
{
    _poolLive = false;

    // Closing the socket is what tells the learner to save and exit; give it time to do so.
    if (_learner.IsRunning())
        _learner.ExpectExit();

    // An export cut short by the shutdown is expected, not a failure.
    if (_export.IsRunning())
        _export.ExpectExit();

    _server.Shutdown();
    _learner.Stop(std::chrono::seconds(10));
    _export.Stop(std::chrono::seconds(10));

    if (_pool)
        _pool->Teardown();

    _pool.reset();
    _scenario.reset();
    _state = State::Idle;
}

void AnimusForge::Forge::ApplyRequest()
{
    Request const request = _request;
    _request = Request::None;

    switch (request)
    {
        case Request::None:
            break;
        case Request::Start:
            _plan = std::move(_requested);
            _requested = {};
            _plan.Index = 0;
            LOG_INFO("module.animus", "Plan: {} scenario{} with policy {}", _plan.Entries.size(),
                _plan.Entries.size() == 1 ? "" : "s", _plan.Policy);
            if (!StartCurrent())
            {
                _plan.Entries[_plan.Index].Result = Outcome::Failed;
                TeardownScenario(true);
                EndPlan("its first scenario failed to start");
            }
            break;
        case Request::Cancel:
        {
            if (_state == State::Idle)
                break;
            // Only a learner that is running or connected has a run to save.
            bool const learnerSaves = _plan.Remote() && (_learner.IsRunning() || _server.HasClient());
            _plan.Entries[_plan.Index].Result = Outcome::Cancelled;
            TeardownScenario(true);
            EndPlan(learnerSaves ? "cancelled; the learner saved latest.pt, `forge resume` continues it" : "cancelled");
            break;
        }
        case Request::Skip:
            if (_state != State::Idle)
                FinishCurrent(Outcome::Skipped);
            break;
    }
}

void AnimusForge::Forge::HoldWhilePaused()
{
    // Nothing ticks while paused: maps, episode clocks and the learner all wait, so a paused episode resumes
    // exactly where it stopped. Console commands still run here.
    while (_state == State::Paused && !World::IsStopped())
    {
        if (_resumeRequested)
        {
            _resumeRequested = false;
            _state = _pausedFrom;
            _lastReport = std::chrono::steady_clock::now();
            _rateTime = _lastReport;
            _rateTicks = _ticks;
            _rateEpisodes = _pool ? _pool->CompletedEpisodes() : 0;
            if (_lastAct)
                _lastAct = _lastReport;
            LOG_INFO("module.animus", "Resumed {}", _current);
            return;
        }

        if (_request != Request::None)
        {
            ApplyRequest();
            return;
        }

        _learner.Poll();
        Pump();
        std::this_thread::sleep_for(IDLE_SLEEP);
    }
}

bool AnimusForge::Forge::StartCurrent()
{
    PlanEntry const& entry = _plan.Entries[_plan.Index];
    ForgeConfig const& config = RunConfig();
    _current = entry.Scenario;

    std::string const position = _plan.Entries.size() > 1
        ? Acore::StringFormat(" ({} of {})", _plan.Index + 1, _plan.Entries.size()) : "";

    _scenario = CreateScenario(entry.Scenario, config);
    if (!_scenario)
    {
        LOG_ERROR("module.animus", "Unknown scenario '{}'", entry.Scenario);
        return false;
    }

    LOG_INFO("module.animus", "Starting {}{}{} with policy {}{}", entry.Scenario, position,
        entry.Resume ? ", resuming its latest checkpoint" : "", _plan.Policy, _plan.Fast
        ? Acore::StringFormat(" (fast: {} envs, a decision every {} ms, {} s episodes, in {})", config.Envs,
            SIM_TICK_MS * config.DecisionTicks, config.EpisodeSeconds, config.OutputDir) : "");

    // Reject a local policy name before building anything, rather than on the first decision.
    if (!_plan.Remote() && !KnowsPolicy(_plan.Policy))
    {
        LOG_ERROR("module.animus", "Scenario {} has no policy '{}'", _scenario->Name(), _plan.Policy);
        return false;
    }

    _pool = std::make_unique<EnvPool>(*_scenario, config);
    if (!_pool->Setup())
        return false;

    _learnerStarted = false;
    if (_plan.Remote())
    {
        if (!_server.Listen(config.SocketPath))
            return false;

        // A learner that cannot be started is not fatal: the sim keeps waiting on the socket, so one started by
        // hand still works (and `forge cancel` gives up).
        if (config.LearnerAutoStart)
        {
            _learnerStarted = _learner.Start(config, entry.Scenario, entry.Resume);
            if (!_learnerStarted)
                LOG_ERROR("module.animus", "Learner auto-start failed; start it by hand: {}",
                    LearnerProcess::ManualCommand(config, entry.Scenario, entry.Resume));
        }
        else
            LOG_INFO("module.animus", "Waiting for a learner started by hand: {}",
                LearnerProcess::ManualCommand(config, entry.Scenario, entry.Resume));
    }

    _pool->ResetAll();

    auto const now = std::chrono::steady_clock::now();
    _ticks = 0;
    _decisions = 0;
    _scenarioStarted = now;
    _lastReport = now;
    _lastAct.reset();
    _rateTime = now;
    _rateTicks = 0;
    _rateEpisodes = 0;
    _ticksPerSecond = 0.0;
    _episodesPerSecond = 0.0;
    _monitor.Begin(entry.Scenario);

    _poolLive = true;
    _state = _plan.Remote() ? State::Training : State::Running;
    return true;
}

void AnimusForge::Forge::TeardownScenario(bool stopLearner)
{
    _poolLive = false;

    if (stopLearner && _learner.IsRunning())
    {
        _learner.ExpectExit();
        _server.DropClient();
        LOG_INFO("module.animus", "Waiting for the learner (pid {}) to save and exit...", _learner.Pid());
        _learner.Stop(LEARNER_STOP_GRACE);
    }
    else
        _server.DropClient();

    if (_pool)
        _pool->Teardown();

    _pool.reset();
    _scenario.reset();
    _learnerStarted = false;
    _pauseRequested = false;
    _resumeRequested = false;
    _lastAct.reset();
}

char const* AnimusForge::Forge::OutcomeName(Outcome outcome)
{
    switch (outcome)
    {
        case Outcome::None:        return "not started";
        case Outcome::Done:        return "done";
        case Outcome::Skipped:     return "skipped";
        case Outcome::Failed:      return "failed";
        case Outcome::Cancelled:   return "cancelled";
        case Outcome::BelowTarget: return "below target";
    }

    return "unknown";
}

void AnimusForge::Forge::FinishCurrent(Outcome outcome)
{
    PlanEntry& entry = _plan.Entries[_plan.Index];
    entry.Result = outcome;

    LOG_INFO("module.animus", "{} {} after {}{}", entry.Scenario, OutcomeName(outcome),
        Format::Duration(std::chrono::duration<double>(std::chrono::steady_clock::now() - _scenarioStarted).count()),
        _plan.Entries.size() > 1 ? Acore::StringFormat(" ({} of {})", _plan.Index + 1, _plan.Entries.size()) : "");

    // A learner that finished its run has already exited; any other ending stops it.
    TeardownScenario(outcome != Outcome::Done);

    if (++_plan.Index >= _plan.Entries.size())
    {
        _plan.Index = uint32(_plan.Entries.size()) - 1;
        EndPlan("every scenario has ended");
        return;
    }

    if (!StartCurrent())
    {
        _plan.Entries[_plan.Index].Result = Outcome::Failed;
        TeardownScenario(true);
        EndPlan("the next scenario failed to start");
    }
}

void AnimusForge::Forge::EndPlan(char const* reason)
{
    _state = State::Idle;
    _lastPlan = _plan;

    LOG_INFO("module.animus", "Plan ended: {}. The sim is idle.", reason);

    if (_plan.Entries.size() > 1)
    {
        TextTable table({ { "#", TextTable::Align::Right }, { "Scenario" }, { "Outcome" } });
        for (std::size_t i = 0; i < _plan.Entries.size(); ++i)
            table.AddRow({ std::to_string(i + 1), _plan.Entries[i].Scenario, OutcomeName(_plan.Entries[i].Result) });

        table.Write(LogInfo, "  ");
    }
}

bool AnimusForge::Forge::LearnerFinished() const
{
    return _plan.Remote() && _learnerStarted && _learner.FinishedCleanly();
}

bool AnimusForge::Forge::LearnerHalted() const
{
    return _plan.Remote() && _learnerStarted && _learner.HaltedBelowTarget();
}

std::vector<std::string> AnimusForge::Forge::DefaultQueue() const
{
    if (!_config.Queue.empty())
        return _config.Queue;

    std::vector<std::string> stages;
    for (ClassRole::StageDefinition const& stage : ClassRole::ClassRoleStages())
        stages.push_back(stage.Name);

    return stages;
}

bool AnimusForge::Forge::RunAdvanced(ForgeConfig const& config, std::string const& scenario) const
{
    ProgressFile finished;
    if (!finished.Load(config.RunsDir() / scenario / "finished.json"))
        return false;

    // A stage below its target writes finished.json too, with "advanced": false: it has not been passed.
    std::optional<double> const advanced = finished.Number("advanced");
    return !advanced || *advanced != 0.0;
}

void AnimusForge::Forge::WarnSeedOrder(ForgeConfig const& config, std::vector<std::string> const& scenarios,
    LineSink const& out) const
{
    // A stage seeds from the closest trained stage it extends: listed before its base, it seeds from further up the
    // tree (or starts from scratch) unless that base already advanced in an earlier run.
    for (std::size_t index = 0; index < scenarios.size(); ++index)
    {
        ClassRole::StageDefinition const* stage = ClassRole::FindStage(scenarios[index]);
        if (!stage || stage->Extends.empty())
            continue;

        if (std::find(scenarios.begin() + index + 1, scenarios.end(), stage->Extends) != scenarios.end()
            && !RunAdvanced(config, stage->Extends))
            out(Acore::StringFormat("  Warning: {} comes before {}, which it extends and seeds from; it will not seed "
                "from it. List {} first.", stage->Name, stage->Extends, stage->Extends));
    }
}

void AnimusForge::Forge::Pump()
{
    if (_pumping)
        return;

    _pumping = true;
    sWorld->ProcessCliCommands();
    PollExport();
    MaybeReport();
    _pumping = false;
}

void AnimusForge::Forge::PollExport()
{
    if (!_export.IsRunning())
        return;

    _export.Poll();
    if (_export.FinishedCleanly())
        LOG_INFO("module.animus", "Export of {} finished: models in {}", _exportScenario, _exportModelDir);
}

void AnimusForge::Forge::MaybeReport()
{
    if (!_progressInterval || (_state != State::Training && _state != State::Running))
        return;

    auto const now = std::chrono::steady_clock::now();
    if (now - _lastReport < std::chrono::seconds(_progressInterval))
        return;

    _lastReport = now;
    _learner.Poll();
    _monitor.Report(RunConfig(), Snapshot(true), PlanRows(_plan, true), LogInfo, LogWarn, true);
}

AnimusForge::SimSnapshot AnimusForge::Forge::Snapshot(bool advanceRates)
{
    SimSnapshot sim;
    auto const now = std::chrono::steady_clock::now();

    sim.Scenario = _current;
    sim.State = StateName() + (_plan.Fast && _state != State::Idle ? " (fast)" : "");
    sim.PlanPosition = _plan.Index + 1;
    sim.PlanSize = uint32(_plan.Entries.size());
    sim.Remote = _plan.Remote();
    sim.Decisions = _decisions;
    sim.EpisodeLimit = _plan.Remote() ? 0 : _plan.LocalEpisodes;
    sim.ScenarioSeconds = std::chrono::duration<double>(now - _scenarioStarted).count();

    if (_pool)
    {
        sim.Envs = _pool->NumEnvs();
        sim.AgentsPerEnv = _pool->Spec().AgentsPerEnv;
        sim.Episodes = _pool->CompletedEpisodes();
        sim.EpisodeMeans = _pool->LastEpisodeMeans();
        sim.EpisodeMeansCount = _pool->LastEpisodeMeansCount();
    }

    // Rates over the time since the last periodic report; a status in between shows the rate so far.
    double const seconds = std::chrono::duration<double>(now - _rateTime).count();
    if (seconds >= 1.0 && _state != State::Paused)
    {
        _ticksPerSecond = double(_ticks - _rateTicks) / seconds;
        _episodesPerSecond = double(sim.Episodes - std::min(sim.Episodes, _rateEpisodes)) / seconds;
    }

    if (advanceRates)
    {
        _rateTime = now;
        _rateTicks = _ticks;
        _rateEpisodes = sim.Episodes;
    }

    sim.TicksPerSecond = _ticksPerSecond;
    sim.EpisodesPerSecond = _episodesPerSecond;

    sim.LearnerRunning = _learner.IsRunning();
    sim.LearnerPid = _learner.IsRunning() ? int32(_learner.Pid()) : -1;
    sim.LearnerConnected = _server.HasClient();
    sim.LearnerFailed = _learnerStarted && _learner.FailedUnexpectedly();
    sim.SecondsSinceAct = _lastAct ? std::chrono::duration<double>(now - *_lastAct).count() : -1.0;
    return sim;
}

std::vector<AnimusForge::PlanRow> AnimusForge::Forge::PlanRows(Plan const& plan, bool live) const
{
    std::vector<PlanRow> rows;
    for (std::size_t i = 0; i < plan.Entries.size(); ++i)
    {
        PlanEntry const& entry = plan.Entries[i];

        PlanRow row;
        row.Scenario = entry.Scenario;
        row.Resume = entry.Resume;
        row.Current = live && i == plan.Index;
        row.Pending = entry.Result == Outcome::None && !row.Current;
        row.Status = entry.Result != Outcome::None ? OutcomeName(entry.Result) : row.Current ? StateName() : "pending";
        rows.push_back(std::move(row));
    }

    return rows;
}

std::string AnimusForge::Forge::StateName() const
{
    switch (_state)
    {
        case State::Idle:
            return "idle";
        case State::Training:
            return _server.HasClient() ? "training" : "waiting for learner";
        case State::Running:
            return "running " + _plan.Policy;
        case State::Paused:
            return "paused";
    }

    return "unknown";
}

void AnimusForge::Forge::LocalDecision()
{
    _pool->Collect();

    if (_plan.LocalEpisodes && _pool->CompletedEpisodes() >= _plan.LocalEpisodes)
    {
        FinishCurrent(Outcome::Done);
        return;
    }

    if (!_pool->ChooseLocalActions(_plan.Policy))
    {
        LOG_ERROR("module.animus", "Scenario {} could not choose actions with policy '{}'", _current, _plan.Policy);
        FinishCurrent(Outcome::Failed);
        return;
    }

    _pool->ApplyActions();
}

void AnimusForge::Forge::RemoteDecision()
{
    // While the world thread waits on the learner: answer console commands, report progress, and stop waiting
    // when a command needs this scenario to end.
    auto const onIdle = [this]()
    {
        _learner.Poll();
        Pump();
        return _request == Request::None;
    };

    // Waiting for a learner to connect holds no decision, so a pause applies right away (OnUpdate pauses next tick).
    auto const onAccepting = [this, &onIdle]()
    {
        return onIdle() && !_pauseRequested && !LearnerFinished() && !LearnerHalted();
    };

    if (!_server.HasClient())
    {
        // Blocks until the learner connects; returns false on shutdown, a cancel or skip, or when the scenario's
        // learner has finished its run.
        bool const connected = _server.AcceptClient(onAccepting);

        if (!connected)
        {
            if (LearnerFinished())
                FinishCurrent(Outcome::Done);
            else if (LearnerHalted())
            {
                // The stage stayed below its target after its restarts: later stages must not train on top of it.
                _plan.Entries[_plan.Index].Result = Outcome::BelowTarget;
                LOG_ERROR("module.animus", "{} stayed below its stage target after its restarts (see runs/{}/"
                    "finished.json and stage.jsonl). The plan halts here: tune the target or the stage, then `forge "
                    "start {}`.", _current, _current, _current);
                TeardownScenario(false);
                EndPlan("a stage stayed below its target");
            }
            return;
        }

        if (!SendSpec())
            return;

        // A new learner starts from fresh training episodes; whatever ran unobserved is discarded. An evaluation the
        // previous learner left unfinished ends here, or its baseline would keep replacing this learner's actions.
        _pool->SetEvaluation(false, 0, 0, {});
        _pool->ResetAll();
    }
    else
        _pool->Collect();

    if (!SendStep())
        return;

    std::size_t const actionBytes = _pool->Actions.size() * sizeof(int32);

    // ACT, or MODE first: a mode switch resets every env and answers with a fresh STEP before the ACT.
    for (;;)
    {
        MsgType type;
        std::vector<char> payload;
        if (!_server.ReceiveAny(type, payload, std::max(actionBytes, sizeof(ModeMsg)), onIdle))
        {
            _server.DropClient();
            return;
        }

        if (type == MsgType::Act && payload.size() == actionBytes)
        {
            std::memcpy(_pool->Actions.data(), payload.data(), actionBytes);
            _lastAct = std::chrono::steady_clock::now();
            break;
        }

        if (type == MsgType::Mode && payload.size() == sizeof(ModeMsg))
        {
            ModeMsg mode{};
            std::memcpy(&mode, payload.data(), sizeof(mode));
            if (!ApplyMode(mode))
            {
                _server.DropClient();
                return;
            }

            _pool->ResetAll();
            if (!SendStep())
                return;

            continue;
        }

        // CLOSE (the client is already gone) or a protocol error: the next decision waits for a new learner.
        if (type != MsgType::Close)
            LOG_ERROR("module.animus", "Learner sent message type {} with {} bytes where ACT or MODE was expected",
                uint32(type), payload.size());

        _server.DropClient();
        return;
    }

    // Scoring a scripted baseline on the evaluation seeds: its actions replace the learner's.
    if (!_pool->EvalBaseline().empty() && !_pool->ChooseLocalActions(_pool->EvalBaseline()))
    {
        LOG_ERROR("module.animus", "Scenario {} could not run baseline '{}'; dropping the learner", _current,
            _pool->EvalBaseline());
        _server.DropClient();
        return;
    }

    _pool->ApplyActions();
}

bool AnimusForge::Forge::KnowsPolicy(std::string const& policy) const
{
    if (policy == "random")
        return true;

    // ScriptedAction answers whether the scenario has the policy; a blank row is enough to ask.
    ScenarioSpec const spec = _scenario->Spec();
    std::vector<float> obs(spec.ObsDim, 0.0f);
    std::vector<uint8> mask(spec.NumActions, 0);
    int32 action = 0;
    return _scenario->ScriptedAction(policy, obs.data(), mask.data(), 0, action);
}

bool AnimusForge::Forge::ApplyMode(ModeMsg const& mode)
{
    std::string const baseline(mode.Baseline, strnlen(mode.Baseline, POLICY_NAME_SIZE));

    if (mode.Mode > 1)
    {
        LOG_ERROR("module.animus", "Learner asked for unknown mode {}", mode.Mode);
        return false;
    }

    if (mode.Mode == 1 && !baseline.empty() && !KnowsPolicy(baseline))
    {
        LOG_ERROR("module.animus", "Learner asked for baseline '{}', which scenario {} does not have", baseline,
            _scenario->Name());
        return false;
    }

    _pool->SetEvaluation(mode.Mode == 1, mode.SeedBase, mode.Episodes, baseline);

    if (mode.Mode == 1)
        LOG_INFO("module.animus", "Evaluation: {} seeded episodes from seed {}, policy {}", mode.Episodes,
            mode.SeedBase, baseline.empty() ? "learner" : baseline);
    else
        LOG_INFO("module.animus", "Evaluation finished; training");

    return true;
}

bool AnimusForge::Forge::SendSpec()
{
    ScenarioSpec const spec = _pool->Spec();

    SpecMsg msg{};
    msg.Version = PROTOCOL_VERSION;
    msg.NumEnvs = _pool->NumEnvs();
    msg.AgentsPerEnv = spec.AgentsPerEnv;
    msg.ObsDim = spec.ObsDim;
    msg.StateDim = spec.StateDim;
    msg.NumActions = spec.NumActions;
    msg.EpisodeInfoDim = spec.EpisodeInfoDim;
    msg.TickMs = _tickMs;
    msg.DecisionTicks = RunConfig().DecisionTicks;
    msg.EpisodeSeconds = RunConfig().EpisodeSeconds;
    std::strncpy(msg.Scenario, _scenario->Name(), SCENARIO_NAME_SIZE - 1);

    uint32 const layoutCount = uint32(spec.Layouts.size());
    std::vector<LayoutMsg> layouts(layoutCount);
    for (uint32 i = 0; i < layoutCount; ++i)
    {
        layouts[i].ObsDim = spec.Layouts[i].ObsDim;
        layouts[i].NumActions = spec.Layouts[i].NumActions;
        std::strncpy(layouts[i].Name, spec.Layouts[i].Name.c_str(), LAYOUT_NAME_SIZE - 1);
    }

    std::string names;
    for (std::string const& name : _scenario->EpisodeInfoNames())
        names += (names.empty() ? "" : ",") + name;

    return _server.Send(MsgType::Spec, { { &msg, sizeof(msg) }, { &layoutCount, sizeof(layoutCount) },
        { layouts.data(), layouts.size() * sizeof(LayoutMsg) }, { names.data(), names.size() } });
}

bool AnimusForge::Forge::SendStep()
{
    StepHeader header{ _decisions++ };

    auto chunk = [](auto const& vec) { return Chunk{ vec.data(), vec.size() * sizeof(vec[0]) }; };

    return _server.Send(MsgType::Step,
    {
        { &header, sizeof(header) },
        chunk(_pool->Obs),
        chunk(_pool->State),
        chunk(_pool->Mask),
        chunk(_pool->Layout),
        chunk(_pool->Present),
        chunk(_pool->Rewards),
        chunk(_pool->Done),
        chunk(_pool->Terminated),
        chunk(_pool->FinalObs),
        chunk(_pool->FinalState),
        chunk(_pool->EpisodeInfo),
        chunk(_pool->EpisodeSeed),
    });
}
