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
#include "World.h"
#include <algorithm>
#include <cstring>
#include <filesystem>

AnimusForge::Forge* AnimusForge::Forge::Instance()
{
    static Forge instance;
    return &instance;
}

void AnimusForge::Forge::OnStartup()
{
    _config.Load();

    if (!_config.Enable)
    {
        LOG_INFO("module.animus", "Animus Forge is disabled (AnimusForge.Enable = 0)");
        return;
    }

    _queue = _config.Queue;
    if (_queue.empty())
        for (ClassRole::StageDefinition const& stage : ClassRole::ClassRoleStages())
            _queue.push_back(stage.Name);

    if (_queue.empty())
    {
        Fail("AnimusForge.Queue is empty and there are no class/role stages");
        return;
    }

    // A stage seeds from the closest trained stage it extends: queued before its base, it seeds from further up the
    // tree (or starts from scratch) unless that base already finished in an earlier run.
    for (std::size_t index = 0; index < _queue.size(); ++index)
    {
        ClassRole::StageDefinition const* stage = ClassRole::FindStage(_queue[index]);
        if (!stage || stage->Extends.empty())
            continue;

        auto const base = std::find(_queue.begin() + index + 1, _queue.end(), stage->Extends);
        if (base != _queue.end() && !AlreadyFinished(stage->Extends))
            LOG_WARN("module.animus", "Queue: {} comes before {}, which it extends and seeds from; it will not seed "
                "from it. Queue {} first.", stage->Name, stage->Extends, stage->Extends);
    }

    _queueIndex = 0;
    StartQueue();
}

bool AnimusForge::Forge::AlreadyFinished(std::string const& scenario) const
{
    // Only an auto-started learner moves the queue on, so only then does skipping mean anything.
    if (!_config.QueueSkipFinished || !_config.IsRemote() || !_config.LearnerAutoStart)
        return false;

    std::error_code error;
    return std::filesystem::exists(std::filesystem::path(_config.RunsDir()) / scenario / "finished.json", error);
}

bool AnimusForge::Forge::StartQueue()
{
    for (; _queueIndex < _queue.size() && AlreadyFinished(CurrentScenario()); ++_queueIndex)
        LOG_INFO("module.animus", "Queue: {} already finished (runs/{}/finished.json), skipping it. To train it again, "
            "move that run away or set AnimusForge.Queue.SkipFinished = 0.", CurrentScenario(), CurrentScenario());

    if (_queueIndex >= _queue.size())
    {
        LOG_INFO("module.animus", "Queue complete: every scenario in AnimusForge.Queue has finished training. The sim "
            "idles until the server is stopped.");
        return false;
    }

    LOG_INFO("module.animus", "Queue: starting {} ({} of {})", CurrentScenario(), _queueIndex + 1, _queue.size());
    if (!Start())
        return false;

    _running = true;
    return true;
}

bool AnimusForge::Forge::Start()
{
    _scenario = CreateScenario(CurrentScenario(), _config);
    if (!_scenario)
    {
        std::string available;
        for (std::string const& name : ScenarioNames())
            available += (available.empty() ? "" : ", ") + name;

        LOG_ERROR("module.animus", "Unknown scenario '{}'. Available: {}", CurrentScenario(), available);
        Fail("unknown scenario");
        return false;
    }

    LOG_INFO("module.animus", "Starting scenario {} with policy {}", _scenario->Name(), _config.Policy);

    ScenarioSpec const spec = _scenario->Spec();

    // Reject a local policy name before building anything, rather than on the first decision.
    if (!_config.IsRemote() && _config.Policy != "random")
    {
        std::vector<float> obs(spec.ObsDim, 0.0f);
        std::vector<uint8> mask(spec.NumActions, 0);
        int32 action = 0;

        if (!_scenario->ScriptedAction(_config.Policy, obs.data(), mask.data(), 0, action))
        {
            LOG_ERROR("module.animus", "Scenario {} has no policy '{}'", _scenario->Name(), _config.Policy);
            Fail("unknown policy");
            return false;
        }
    }

    _pool = std::make_unique<EnvPool>(*_scenario, _config);
    if (!_pool->Setup())
    {
        Fail("environment setup failed");
        return false;
    }

    if (_config.IsRemote())
    {
        if (!_server.Listen(_config.SocketPath))
        {
            Fail("cannot open the learner socket");
            return false;
        }

        // A learner that cannot be started is not fatal: the sim keeps waiting on the socket, so
        // one started by hand still works.
        if (_config.LearnerAutoStart && !_learner.Start(_config, CurrentScenario()))
            LOG_ERROR("module.animus", "Learner auto-start failed; start it manually: cd {} && {} -m animus.train "
                "--config {} --socket {} --run-name {} --runs-dir {} --layouts-dir {}", _config.LearnerWorkDir,
                _config.LearnerPython, _config.LearnerConfigFor(CurrentScenario()), _config.SocketPath,
                CurrentScenario(), _config.RunsDir(), _config.LayoutsDir());
    }

    _pool->ResetAll();
    return true;
}

bool AnimusForge::Forge::QueueScenarioFinished() const
{
    return _config.IsRemote() && _config.LearnerAutoStart && _learner.FinishedCleanly();
}

void AnimusForge::Forge::AdvanceQueue()
{
    LOG_INFO("module.animus", "Queue: {} finished ({} of {})", CurrentScenario(), _queueIndex + 1, _queue.size());

    _running = false;
    _pool->Teardown();
    _pool.reset();
    _scenario.reset();

    ++_queueIndex;
    StartQueue();
}

void AnimusForge::Forge::Fail(char const* reason)
{
    // The sim host exists to run the scenario; carrying on without it would only burn CPU.
    LOG_FATAL("module.animus", "Animus Forge cannot start: {}. Stopping the server.", reason);
    World::StopNow(ERROR_EXIT_CODE);
}

void AnimusForge::Forge::OnUpdate(uint32 diff)
{
    if (!_running)
        return;

    _tickMs = diff;
    _pool->AdvanceClock(diff);

    if (++_ticks % _config.DecisionTicks)
        return;

    if (_config.IsRemote())
        RemoteDecision();
    else
        LocalDecision();
}

void AnimusForge::Forge::OnShutdown()
{
    _running = false;

    // Closing the socket is what tells the learner to save and exit; give it time to do so.
    _server.Shutdown();
    _learner.Stop(std::chrono::seconds(10));

    if (_pool)
        _pool->Teardown();

    _pool.reset();
    _scenario.reset();
}

void AnimusForge::Forge::LocalDecision()
{
    _pool->Collect();

    // A queue under a local policy (smoke tests, baselines) moves on after a fixed number of episodes.
    if (_config.QueueLocalEpisodes && _pool->CompletedEpisodes() >= _config.QueueLocalEpisodes)
    {
        AdvanceQueue();
        return;
    }

    _pool->ChooseLocalActions(_config.Policy);
    _pool->ApplyActions();
}

void AnimusForge::Forge::RemoteDecision()
{
    if (!_server.HasClient())
    {
        // Blocks the world thread until the learner connects; returns false on shutdown, or when a
        // queued scenario's learner has finished its run. Polls the auto-started learner meanwhile,
        // so an early exit is logged instead of silent.
        bool const connected = _server.AcceptClient([this]()
        {
            _learner.Poll();
            return !QueueScenarioFinished();
        });

        if (!connected)
        {
            if (QueueScenarioFinished())
                AdvanceQueue();
            return;
        }

        if (!SendSpec())
            return;

        // A new learner starts from fresh episodes; whatever ran unobserved is discarded.
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
        if (!_server.ReceiveAny(type, payload, std::max(actionBytes, sizeof(ModeMsg))))
        {
            _server.DropClient();
            return;
        }

        if (type == MsgType::Act && payload.size() == actionBytes)
        {
            std::memcpy(_pool->Actions.data(), payload.data(), actionBytes);
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
    if (!_pool->EvalBaseline().empty())
        _pool->ChooseLocalActions(_pool->EvalBaseline());

    _pool->ApplyActions();
}

bool AnimusForge::Forge::ApplyMode(ModeMsg const& mode)
{
    std::string const baseline(mode.Baseline, strnlen(mode.Baseline, POLICY_NAME_SIZE));

    if (mode.Mode > 1)
    {
        LOG_ERROR("module.animus", "Learner asked for unknown mode {}", mode.Mode);
        return false;
    }

    if (mode.Mode == 1 && !baseline.empty() && baseline != "random")
    {
        ScenarioSpec const spec = _scenario->Spec();
        std::vector<float> obs(spec.ObsDim, 0.0f);
        std::vector<uint8> mask(spec.NumActions, 0);
        int32 action = 0;

        if (!_scenario->ScriptedAction(baseline, obs.data(), mask.data(), 0, action))
        {
            LOG_ERROR("module.animus", "Learner asked for baseline '{}', which scenario {} does not have", baseline,
                _scenario->Name());
            return false;
        }
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
    msg.DecisionTicks = _config.DecisionTicks;
    msg.EpisodeSeconds = _config.EpisodeSeconds;
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
        chunk(_pool->Rewards),
        chunk(_pool->Done),
        chunk(_pool->Terminated),
        chunk(_pool->FinalObs),
        chunk(_pool->FinalState),
        chunk(_pool->EpisodeInfo),
        chunk(_pool->EpisodeSeed),
    });
}
