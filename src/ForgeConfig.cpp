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

    /// The directory of the worldserver config file this server loaded: what every relative path key is relative to.
    fs::path ConfigDir()
    {
        fs::path const file = sConfigMgr->GetFilename();
        fs::path const dir = file.has_parent_path() ? file.parent_path() : fs::path(".");
        std::error_code error;
        fs::path const absolute = fs::absolute(dir, error);
        return error ? dir : absolute;
    }

    /// A path key's value: as given when absolute, else under `base`.
    fs::path Resolve(fs::path const& value, fs::path const& base)
    {
        return (value.is_relative() ? base / value : value).lexically_normal();
    }

    /// A comma-separated config list, whitespace removed, empty entries dropped.
    std::vector<std::string> GetList(std::string const& key, std::string const& fallback = "")
    {
        std::vector<std::string> entries;
        std::string const value = sConfigMgr->GetOption<std::string>(key, fallback);
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
    // Relative path keys are relative to the directory of the worldserver config file, whatever the server's working
    // directory; empty ones take the defaults below.
    fs::path const configDir = ConfigDir();

    SocketPath = Resolve(sConfigMgr->GetOption<std::string>("AnimusForge.Socket", "/tmp/animus-forge.sock"),
        configDir).string();

    LearnerAutoStart = sConfigMgr->GetOption<bool>("AnimusForge.Learner.AutoStart", true);

    fs::path workDir = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.WorkDir", "");
    workDir = workDir.empty() ? DefaultLearnerWorkDir() : Resolve(workDir, configDir);
    LearnerWorkDir = workDir.string();

    fs::path const outputDir = sConfigMgr->GetOption<std::string>("AnimusForge.OutputDir", "");
    OutputDir = (outputDir.empty() ? workDir : Resolve(outputDir, configDir)).lexically_normal().string();

    LearnerPython = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Python", "");
    if (LearnerPython.empty())
    {
        fs::path const venvPython = workDir / ".venv" / "bin" / "python";
        std::error_code error;
        LearnerPython = fs::exists(venvPython, error) ? venvPython.string() : "python3";
    }
    else if (LearnerPython.find('/') != std::string::npos)
        LearnerPython = Resolve(LearnerPython, configDir).string();     // a bare name ("python3") is found on PATH

    // Set: relative to the config directory. Empty: configs/<scenario>.yaml in the work directory (LearnerConfigFor).
    fs::path const learnerConfig = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Config", "");
    LearnerConfig = learnerConfig.empty() ? std::string() : Resolve(learnerConfig, configDir).string();

    LearnerArgs.clear();
    std::istringstream extraArgs(sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Args", ""));
    for (std::string arg; extraArgs >> arg;)
        LearnerArgs.push_back(arg);

    fs::path const logFile = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.LogFile", "");
    if (logFile.empty())
    {
        // Beside the server's own logs (LogsDir is the core's key, resolved as the core resolves it).
        fs::path logsDir = sConfigMgr->GetOption<std::string>("LogsDir", "");
        LearnerLogFile = (logsDir / "animus-learner.log").string();
    }
    else
        LearnerLogFile = Resolve(logFile, configDir).string();

    // Exported models stay in the forge's own folder; copying them to a game server is done by hand.
    fs::path const modelDir = sConfigMgr->GetOption<std::string>("AnimusForge.ModelDir", "");
    ModelDir = (modelDir.empty() ? ModuleRoot() / "models" : Resolve(modelDir, configDir)).lexically_normal().string();

    ProgressInterval = sConfigMgr->GetOption<uint32>("AnimusForge.Progress.Interval", 60);

    FastEnvs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Fast.Envs", 16));
    FastDecisionTicks = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Fast.DecisionTicks", 4));
    FastEpisodeSeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Fast.EpisodeSeconds", 30));
    FastClassRoles = GetList("AnimusForge.Fast.ClassRoles", "warrior_tank, priest_heal, rogue_dps, hunter_dps");

    fs::path fastOutputDir = sConfigMgr->GetOption<std::string>("AnimusForge.Fast.OutputDir", "fast");
    if (fastOutputDir.empty())
        fastOutputDir = "fast";
    if (fastOutputDir.is_relative())
        fastOutputDir = outputDir / fastOutputDir;
    fastOutputDir = fastOutputDir.lexically_normal();

    // Fast runs must stay out of the real runs: they archive what they replace, and `forge clean fast` deletes it all.
    fs::path const inside = fs::path(OutputDir).lexically_relative(fastOutputDir);
    if (!inside.empty() && *inside.begin() != "..")
    {
        LOG_ERROR("module.animus", "AnimusForge.Fast.OutputDir '{}' is or contains AnimusForge.OutputDir; fast runs go "
            "to {}/fast instead", fastOutputDir.string(), OutputDir);
        fastOutputDir = fs::path(OutputDir) / "fast";
    }
    FastOutputDir = fastOutputDir.string();

    fs::path fastOverlay = sConfigMgr->GetOption<std::string>("AnimusForge.Fast.Learner.Overlay", "");
    if (fastOverlay.empty())
        fastOverlay = fs::path("configs") / "fast.yaml";
    if (fastOverlay.is_relative())
        fastOverlay = workDir / fastOverlay;
    FastLearnerOverlay = fastOverlay.lexically_normal().string();

    FastLearnerArgs.clear();
    std::istringstream fastArgs(sConfigMgr->GetOption<std::string>("AnimusForge.Fast.Learner.Args", ""));
    for (std::string arg; fastArgs >> arg;)
        FastLearnerArgs.push_back(arg);

    SpawnMapId = sConfigMgr->GetOption<uint32>("AnimusForge.SpawnPoint.MapId", 560);
    SpawnPosition.Relocate(
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.X", 2741.9f),
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.Y", 1315.2f),
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.Z", 14.0f),
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.O", 2.96f));
}

fs::path AnimusForge::ForgeConfig::RunsDir() const
{
    return fs::path(OutputDir) / "runs";
}

fs::path AnimusForge::ForgeConfig::LayoutsDir() const
{
    return fs::path(OutputDir) / "layouts";
}

AnimusForge::ForgeConfig AnimusForge::ForgeConfig::FastProfile() const
{
    ForgeConfig fast = *this;
    fast.Policy = "remote";
    fast.Envs = FastEnvs;
    fast.DecisionTicks = FastDecisionTicks;
    fast.EpisodeSeconds = FastEpisodeSeconds;
    fast.ReportEpisodes = std::min<uint32>(ReportEpisodes, 64);
    if (!FastClassRoles.empty())
        fast.ClassRoles = FastClassRoles;

    fast.OutputDir = FastOutputDir;
    fast.ModelDir = (fs::path(FastOutputDir) / "models").string();

    // The overlay goes first: AnimusForge.Learner.Args and AnimusForge.Fast.Learner.Args (--set) still win over it.
    fast.LearnerArgs = { "--overlay", FastLearnerOverlay };
    fast.LearnerArgs.insert(fast.LearnerArgs.end(), LearnerArgs.begin(), LearnerArgs.end());
    fast.LearnerArgs.insert(fast.LearnerArgs.end(), FastLearnerArgs.begin(), FastLearnerArgs.end());
    return fast;
}

std::string AnimusForge::ForgeConfig::LearnerConfigFor(std::string const& scenario) const
{
    fs::path const config = LearnerConfig.empty()
        ? fs::path("configs") / (scenario + ".yaml") : fs::path(LearnerConfig);
    return (config.is_relative() ? fs::path(LearnerWorkDir) / config : config).string();
}
