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

#include "ForgeConfig.h"
#include "Config.h"
#include "Tokenize.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace
{
    /// This module's python/ directory, from the path the compiler saw for this source file
    /// (<module>/src/ForgeConfig.cpp). Valid wherever the module source tree still exists at the
    /// path it was built from: native builds and the dev-server container, not the runtime images.
    std::filesystem::path DefaultLearnerWorkDir()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path() / "python";
    }
}

void AnimusForge::ForgeConfig::Load()
{
    Enable = sConfigMgr->GetOption<bool>("AnimusForge.Enable", true);

    Scenario = sConfigMgr->GetOption<std::string>("AnimusForge.Scenario", "warrior_dummy");

    Queue.clear();
    std::string const queue = sConfigMgr->GetOption<std::string>("AnimusForge.Queue", "");
    for (std::string_view name : Acore::Tokenize(queue, ',', false))
    {
        std::string entry(name);
        entry.erase(std::remove_if(entry.begin(), entry.end(), [](unsigned char c) { return std::isspace(c); }),
            entry.end());

        if (!entry.empty())
            Queue.push_back(entry);
    }

    if (!Queue.empty())
        Scenario = Queue.front();

    QueueLocalEpisodes = sConfigMgr->GetOption<uint32>("AnimusForge.Queue.LocalEpisodes", 0);

    Envs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Envs", 64));
    DecisionTicks = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.DecisionTicks", 1));
    EpisodeSeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.EpisodeSeconds", 60));

    Policy = sConfigMgr->GetOption<std::string>("AnimusForge.Policy", "remote");
    HsRageThreshold = sConfigMgr->GetOption<uint32>("AnimusForge.HsRageThreshold", 15);
    ReportEpisodes = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.ReportEpisodes", 256));
    SocketPath = sConfigMgr->GetOption<std::string>("AnimusForge.Socket", "/tmp/animus-forge.sock");

    LearnerAutoStart = sConfigMgr->GetOption<bool>("AnimusForge.Learner.AutoStart", true);

    std::filesystem::path workDir = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.WorkDir", "");
    if (workDir.empty())
        workDir = DefaultLearnerWorkDir();
    LearnerWorkDir = workDir.string();

    LearnerPython = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Python", "");
    if (LearnerPython.empty())
    {
        std::filesystem::path const venvPython = workDir / ".venv" / "bin" / "python";
        LearnerPython = std::filesystem::exists(venvPython) ? venvPython.string() : "python3";
    }

    LearnerConfig = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Config", "");

    LearnerCleanRun = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.CleanRun", "");

    LearnerArgs.clear();
    std::istringstream extraArgs(sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Args", ""));
    for (std::string arg; extraArgs >> arg;)
        LearnerArgs.push_back(arg);

    LearnerLogFile = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.LogFile", "");
    if (LearnerLogFile.empty())
    {
        std::filesystem::path logsDir = sConfigMgr->GetOption<std::string>("LogsDir", "");
        LearnerLogFile = (logsDir / "animus-learner.log").string();
    }

    ArenaMapId = sConfigMgr->GetOption<uint32>("AnimusForge.Arena.MapId", 560);
    ArenaPosition.Relocate(
        sConfigMgr->GetOption<float>("AnimusForge.Arena.X", 2741.9f),
        sConfigMgr->GetOption<float>("AnimusForge.Arena.Y", 1315.2f),
        sConfigMgr->GetOption<float>("AnimusForge.Arena.Z", 14.0f),
        sConfigMgr->GetOption<float>("AnimusForge.Arena.O", 2.96f));
}

std::string AnimusForge::ForgeConfig::LearnerConfigFor(std::string const& scenario) const
{
    namespace fs = std::filesystem;

    fs::path const workDir = LearnerWorkDir;
    auto const absolute = [&workDir](fs::path const& path) { return path.is_relative() ? workDir / path : path; };

    if (!LearnerConfig.empty())
        return absolute(LearnerConfig).string();

    fs::path const own = absolute(fs::path("configs") / (scenario + ".yaml"));
    if (fs::exists(own))
        return own.string();

    // Class/role stages share a config per stage.
    for (char const* stage : { "duel", "pack", "gauntlet", "companion", "party", "pvp", "arena" })
    {
        std::string const suffix = std::string("_") + stage;
        if (scenario.size() > suffix.size() && scenario.ends_with(suffix))
            return absolute(fs::path("configs") / (std::string("class_role_") + stage + ".yaml")).string();
    }

    return absolute("configs/class_role.yaml").string();
}
