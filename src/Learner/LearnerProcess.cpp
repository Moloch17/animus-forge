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
#include <filesystem>
#include <vector>

namespace
{
    std::vector<std::string> LearnerArgs(AnimusForge::ForgeConfig const& config, std::string const& scenario,
        bool resume)
    {
        // The run is named after the scenario, so a shared config (configs/class_role.yaml) still gives every
        // scenario its own runs/<scenario>/.
        std::vector<std::string> args =
        {
            config.LearnerPython, "-u", "-m", "animus.train",
            "--config", config.LearnerConfigFor(scenario),
            "--socket", config.SocketPath,
            "--run-name", scenario,
        };

        if (resume)
            args.emplace_back("--resume");

        args.insert(args.end(), config.LearnerArgs.begin(), config.LearnerArgs.end());
        return args;
    }
}

bool AnimusForge::LearnerProcess::Start(ForgeConfig const& config, std::string const& scenario, bool resume)
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

    fs::path const configPath = config.LearnerConfigFor(scenario);
    if (!fs::exists(configPath))
    {
        LOG_ERROR("module.animus", "Learner config '{}' does not exist (scenario {}). Create it or set "
            "AnimusForge.Learner.Config.", configPath.string(), scenario);
        return false;
    }

    if (!ChildProcess::Start(LearnerArgs(config, scenario, resume), workDir.string(), config.LearnerLogFile))
        return false;

    LOG_INFO("module.animus", "Started learner (pid {}) for {}{}: config {}; output in {}", Pid(), scenario,
        resume ? ", resuming latest.pt" : "", configPath.string(), config.LearnerLogFile);
    return true;
}

std::string AnimusForge::LearnerProcess::ManualCommand(ForgeConfig const& config, std::string const& scenario,
    bool resume)
{
    std::string command = "cd " + config.LearnerWorkDir + " &&";
    for (std::string const& arg : LearnerArgs(config, scenario, resume))
        command += " " + arg;

    return command;
}
