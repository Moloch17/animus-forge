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
#include "BotAccounts.h"
#include "Config.h"
#include "Log.h"
#include "Tokenize.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace
{
    namespace fs = std::filesystem;

    /// This module's python/ directory, from the path the compiler saw for this source file
    /// (<module>/src/ForgeConfig.cpp). Valid wherever the module source tree still exists at the
    /// path it was built from: native builds and the bind-mounted Docker services, not the runtime images.
    fs::path ModuleRoot()
    {
        return fs::path(__FILE__).parent_path().parent_path();
    }

    fs::path DefaultLearnerWorkDir()
    {
        return ModuleRoot() / "python";
    }

    /// A comma-separated config list, whitespace removed, empty entries dropped.
    std::vector<std::string> GetList(std::string const& key)
    {
        std::vector<std::string> entries;
        std::string const value = sConfigMgr->GetOption<std::string>(key, "");
        for (std::string_view name : Acore::Tokenize(value, ',', false))
        {
            std::string entry(name);
            entry.erase(std::remove_if(entry.begin(), entry.end(), [](unsigned char c) { return std::isspace(c); }),
                entry.end());

            if (!entry.empty())
                entries.push_back(std::move(entry));
        }

        return entries;
    }
}

void AnimusForge::ForgeConfig::Load()
{
    Enable = sConfigMgr->GetOption<bool>("AnimusForge.Enable", true);

    Queue = GetList("AnimusForge.Queue");
    QueueSkipFinished = sConfigMgr->GetOption<bool>("AnimusForge.Queue.SkipFinished", true);
    QueueLocalEpisodes = sConfigMgr->GetOption<uint32>("AnimusForge.Queue.LocalEpisodes", 0);
    ClassRoles = GetList("AnimusForge.ClassRoles");

    Envs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Envs", 64));
    if (Envs > BotAccounts::MAX_ENVS)
    {
        LOG_ERROR("module.animus", "AnimusForge.Envs = {} is more than bot account ids allow; using {}", Envs,
            BotAccounts::MAX_ENVS);
        Envs = BotAccounts::MAX_ENVS;
    }
    DecisionTicks = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.DecisionTicks", 2));
    EpisodeSeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.EpisodeSeconds", 60));

    Policy = sConfigMgr->GetOption<std::string>("AnimusForge.Policy", "remote");
    ReportEpisodes = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.ReportEpisodes", 256));
    SocketPath = sConfigMgr->GetOption<std::string>("AnimusForge.Socket", "/tmp/animus-forge.sock");

    LearnerAutoStart = sConfigMgr->GetOption<bool>("AnimusForge.Learner.AutoStart", true);

    fs::path workDir = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.WorkDir", "");
    if (workDir.empty())
        workDir = DefaultLearnerWorkDir();
    LearnerWorkDir = workDir.string();

    fs::path outputDir = sConfigMgr->GetOption<std::string>("AnimusForge.OutputDir", "");
    if (outputDir.empty())
        outputDir = workDir;
    else if (outputDir.is_relative())
        outputDir = workDir / outputDir;
    OutputDir = outputDir.lexically_normal().string();

    LearnerPython = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Python", "");
    if (LearnerPython.empty())
    {
        fs::path const venvPython = workDir / ".venv" / "bin" / "python";
        std::error_code error;
        LearnerPython = fs::exists(venvPython, error) ? venvPython.string() : "python3";
    }

    LearnerConfig = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Config", "");

    LearnerArgs.clear();
    std::istringstream extraArgs(sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Args", ""));
    for (std::string arg; extraArgs >> arg;)
        LearnerArgs.push_back(arg);

    LearnerLogFile = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.LogFile", "");
    if (LearnerLogFile.empty())
    {
        fs::path logsDir = sConfigMgr->GetOption<std::string>("LogsDir", "");
        LearnerLogFile = (logsDir / "animus-learner.log").string();
    }

    // Exported models stay in the forge's own folder; copying them to a game server is done by hand.
    fs::path modelDir = sConfigMgr->GetOption<std::string>("AnimusForge.ModelDir", "");
    if (modelDir.empty())
        modelDir = ModuleRoot() / "models";
    else if (modelDir.is_relative())
        modelDir = ModuleRoot() / modelDir;
    ModelDir = modelDir.lexically_normal().string();

    ProgressInterval = sConfigMgr->GetOption<uint32>("AnimusForge.Progress.Interval", 60);

    SpawnMapId = sConfigMgr->GetOption<uint32>("AnimusForge.SpawnPoint.MapId", 560);
    SpawnPosition.Relocate(
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.X", 2741.9f),
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.Y", 1315.2f),
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.Z", 14.0f),
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.O", 2.96f));

    WarriorDummy20HsRageThreshold = sConfigMgr->GetOption<uint32>("AnimusForge.WarriorDummy20.HsRageThreshold", 15,
        false);
}

fs::path AnimusForge::ForgeConfig::RunsDir() const
{
    return fs::path(OutputDir) / "runs";
}

fs::path AnimusForge::ForgeConfig::LayoutsDir() const
{
    return fs::path(OutputDir) / "layouts";
}

std::string AnimusForge::ForgeConfig::LearnerConfigFor(std::string const& scenario) const
{
    fs::path const config = LearnerConfig.empty()
        ? fs::path("configs") / (scenario + ".yaml") : fs::path(LearnerConfig);
    return (config.is_relative() ? fs::path(LearnerWorkDir) / config : config).string();
}
