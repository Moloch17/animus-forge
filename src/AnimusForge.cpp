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
#include "World.h"
#include <algorithm>
#include <cstring>

Animus::Forge* Animus::Forge::Instance()
{
    static Forge instance;
    return &instance;
}

void Animus::Forge::OnStartup()
{
    _config.Load();

    if (!_config.Enable)
    {
        LOG_INFO("module.animus", "Animus Forge is disabled (AnimusForge.Enable = 0)");
        return;
    }

    if (!Start())
        return;

    _running = true;
}

bool Animus::Forge::Start()
{
    _scenario = CreateScenario(_config);
    if (!_scenario)
    {
        std::string available;
        for (std::string const& name : ScenarioNames())
            available += (available.empty() ? "" : ", ") + name;

        LOG_ERROR("module.animus", "Unknown scenario '{}'. Available: {}", _config.Scenario, available);
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

        if (!_scenario->ScriptedAction(_config.Policy, obs.data(), mask.data(), action))
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

    if (_config.IsRemote() && !_server.Listen(_config.SocketPath))
    {
        Fail("cannot open the learner socket");
        return false;
    }

    _pool->ResetAll();
    return true;
}

void Animus::Forge::Fail(char const* reason)
{
    // The sim host exists to run the scenario; carrying on without it would only burn CPU.
    LOG_FATAL("module.animus", "Animus Forge cannot start: {}. Stopping the server.", reason);
    World::StopNow(ERROR_EXIT_CODE);
}

void Animus::Forge::OnUpdate(uint32 diff)
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

void Animus::Forge::OnShutdown()
{
    _running = false;
    _server.Shutdown();

    if (_pool)
        _pool->Teardown();

    _pool.reset();
    _scenario.reset();
}

void Animus::Forge::LocalDecision()
{
    _pool->Collect();
    _pool->ChooseLocalActions(_config.Policy);
    _pool->ApplyActions();
}

void Animus::Forge::RemoteDecision()
{
    if (!_server.HasClient())
    {
        // Blocks the world thread until the learner connects; returns false only on shutdown.
        if (!_server.AcceptClient() || !SendSpec())
            return;

        // A new learner starts from fresh episodes; whatever ran unobserved is discarded.
        _pool->ResetAll();
    }
    else
        _pool->Collect();

    if (!SendStep())
        return;

    MsgType type;
    std::size_t const actionBytes = _pool->Actions.size() * sizeof(int32);
    if (!_server.Receive(type, _pool->Actions.data(), actionBytes) || type != MsgType::Act)
    {
        // CLOSE or a protocol error: the client is gone, the next decision waits for a new one.
        _server.DropClient();
        return;
    }

    _pool->ApplyActions();
}

bool Animus::Forge::SendSpec()
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

    std::string names;
    for (std::string const& name : _scenario->EpisodeInfoNames())
        names += (names.empty() ? "" : ",") + name;

    return _server.Send(MsgType::Spec, { { &msg, sizeof(msg) }, { names.data(), names.size() } });
}

bool Animus::Forge::SendStep()
{
    StepHeader header{ _decisions++ };

    auto chunk = [](auto const& vec) { return Chunk{ vec.data(), vec.size() * sizeof(vec[0]) }; };

    return _server.Send(MsgType::Step,
    {
        { &header, sizeof(header) },
        chunk(_pool->Obs),
        chunk(_pool->State),
        chunk(_pool->Mask),
        chunk(_pool->Rewards),
        chunk(_pool->Done),
        chunk(_pool->Terminated),
        chunk(_pool->FinalObs),
        chunk(_pool->FinalState),
        chunk(_pool->EpisodeInfo),
    });
}
