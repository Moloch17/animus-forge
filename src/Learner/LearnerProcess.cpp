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

#include "LearnerProcess.h"
#include "ForgeConfig.h"
#include "Log.h"
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <vector>

extern char** environ;

namespace
{
    /// Poll interval while waiting for the child to exit.
    constexpr std::chrono::milliseconds EXIT_POLL_INTERVAL{ 50 };
}

AnimusForge::LearnerProcess::~LearnerProcess()
{
    Stop(std::chrono::seconds(10));
}

bool AnimusForge::LearnerProcess::Start(ForgeConfig const& config)
{
    namespace fs = std::filesystem;

    fs::path const workDir = config.LearnerWorkDir;
    if (!fs::is_directory(workDir) || !fs::exists(workDir / "animus" / "train.py"))
    {
        LOG_ERROR("module.animus", "Learner directory '{}' does not contain animus/train.py. Set "
            "AnimusForge.Learner.WorkDir to mod-animus-forge/python, or AnimusForge.Learner.AutoStart = 0 "
            "to start the learner yourself.", workDir.string());
        return false;
    }

    fs::path const configPath = config.LearnerConfigFor(config.Scenario);
    if (!fs::exists(configPath))
    {
        LOG_ERROR("module.animus", "Learner config '{}' does not exist (scenario {}). Create it or set "
            "AnimusForge.Learner.Config.", configPath.string(), config.Scenario);
        return false;
    }

    _logFile = config.LearnerLogFile;
    _exitedCleanly = false;

    // The run is named after the scenario, so a shared config (AnimusForge.Learner.Config) still gives
    // every queued scenario its own runs/<scenario>/ and <scenario>.amdl.
    std::vector<std::string> args =
    {
        config.LearnerPython, "-u", "-m", "animus.train",
        "--config", configPath.string(),
        "--socket", config.SocketPath,
        "--run-name", config.Scenario,
    };

    args.insert(args.end(), config.LearnerArgs.begin(), config.LearnerArgs.end());

    std::vector<char*> argv;
    for (std::string& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    // Run in the learner directory, append both output streams to the log file, and do not leak
    // the server's descriptors (database connections, log files, the learner socket) into Python.
    posix_spawn_file_actions_addchdir_np(&actions, workDir.c_str());
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, _logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);

    pid_t pid = -1;
    int const error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    if (error)
    {
        LOG_ERROR("module.animus", "Could not start the learner '{}': {}", config.LearnerPython, std::strerror(error));
        return false;
    }

    _pid = pid;
    LOG_INFO("module.animus", "Started learner (pid {}): {} -m animus.train --config {} in {}; output in {}", _pid,
        config.LearnerPython, configPath.string(), workDir.string(), _logFile);
    return true;
}

void AnimusForge::LearnerProcess::Poll()
{
    if (_pid <= 0)
        return;

    int status = 0;
    if (::waitpid(_pid, &status, WNOHANG) == _pid)
        ReportExit(status);
}

void AnimusForge::LearnerProcess::Stop(std::chrono::milliseconds grace)
{
    if (_pid <= 0)
        return;

    if (WaitForExit(grace))
        return;

    LOG_WARN("module.animus", "Learner (pid {}) still running; interrupting it", _pid);
    ::kill(_pid, SIGINT);
    if (WaitForExit(grace))
        return;

    LOG_WARN("module.animus", "Learner (pid {}) ignored SIGINT; killing it", _pid);
    ::kill(_pid, SIGKILL);
    WaitForExit(grace);
}

bool AnimusForge::LearnerProcess::WaitForExit(std::chrono::milliseconds timeout)
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    while (_pid > 0)
    {
        Poll();
        if (_pid <= 0 || std::chrono::steady_clock::now() >= deadline)
            break;

        std::this_thread::sleep_for(EXIT_POLL_INTERVAL);
    }

    return _pid <= 0;
}

void AnimusForge::LearnerProcess::ReportExit(int status)
{
    _exitedCleanly = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    if (_exitedCleanly)
        LOG_INFO("module.animus", "Learner (pid {}) finished; output in {}", _pid, _logFile);
    else if (WIFEXITED(status))
        LOG_ERROR("module.animus", "Learner (pid {}) exited with code {}; see {}", _pid, WEXITSTATUS(status), _logFile);
    else if (WIFSIGNALED(status))
        LOG_ERROR("module.animus", "Learner (pid {}) killed by signal {}; see {}", _pid, WTERMSIG(status), _logFile);

    _pid = -1;
}
