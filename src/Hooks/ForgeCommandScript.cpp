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
#include "Chat.h"
#include "CommandScript.h"
#include "Optional.h"
#include "StringConvert.h"
#include "TextTable.h"

using namespace Acore::ChatCommands;

namespace
{
    /// Scenario names separated by spaces or commas.
    std::vector<std::string> SplitNames(std::string_view text)
    {
        std::vector<std::string> names;
        std::string name;
        for (char c : text)
        {
            if (c == ' ' || c == ',' || c == '\t')
            {
                if (!name.empty())
                    names.push_back(std::move(name));
                name.clear();
            }
            else
                name += c;
        }

        if (!name.empty())
            names.push_back(std::move(name));

        return names;
    }

    AnimusForge::LineSink Reply(ChatHandler* handler)
    {
        return [handler](std::string const& line) { handler->SendSysMessage(line); };
    }

    class ForgeCommandScript : public CommandScript
    {
    public:
        ForgeCommandScript() : CommandScript("ForgeCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable forgeCommandTable =
            {
                { "help",      HandleHelp,      SEC_ADMINISTRATOR, Console::Yes },
                { "status",    HandleStatus,    SEC_ADMINISTRATOR, Console::Yes },
                { "scenarios", HandleScenarios, SEC_ADMINISTRATOR, Console::Yes },
                { "start",     HandleStart,     SEC_ADMINISTRATOR, Console::Yes },
                { "resume",    HandleResume,    SEC_ADMINISTRATOR, Console::Yes },
                { "pause",     HandlePause,     SEC_ADMINISTRATOR, Console::Yes },
                { "cancel",    HandleCancel,    SEC_ADMINISTRATOR, Console::Yes },
                { "skip",      HandleSkip,      SEC_ADMINISTRATOR, Console::Yes },
                { "run",       HandleRun,       SEC_ADMINISTRATOR, Console::Yes },
                { "export",    HandleExport,    SEC_ADMINISTRATOR, Console::Yes },
                { "clean",     HandleClean,     SEC_ADMINISTRATOR, Console::Yes },
                { "progress",  HandleProgress,  SEC_ADMINISTRATOR, Console::Yes },
                { "",          HandleHelp,      SEC_ADMINISTRATOR, Console::Yes },
            };

            static ChatCommandTable commandTable =
            {
                { "forge", forgeCommandTable },
            };

            return commandTable;
        }

        static bool HandleHelp(ChatHandler* handler)
        {
            AnimusForge::TextTable table({ { "Command" }, { "What it does" } });
            table.AddRow({ "forge status", "what is running, progress, ETA and warnings (or the idle settings)" });
            table.AddRow({ "forge scenarios", "every scenario with its run: checkpoint, steps, best score" });
            table.AddRow({ "forge start [scenario ...]", "train these from scratch in order (default: "
                "AnimusForge.Queue)" });
            table.AddRow({ "forge resume [scenario ...]", "unpause; or continue the first from its latest.pt, then the "
                "rest (default: where the last plan stopped)" });
            table.AddRow({ "forge pause", "freeze the sim and the learner after the current decision" });
            table.AddRow({ "forge cancel", "stop the plan; the learner saves latest.pt first" });
            table.AddRow({ "forge skip", "end the current scenario and start the next one" });
            table.AddRow({ "forge run <scenario> <policy> [episodes]", "run a scripted or random policy, no learner" });
            table.AddRow({ "forge export [scenario] [best|latest]", "write the scenario's .amdl models to "
                "AnimusForge.ModelDir" });
            table.AddRow({ "forge clean archive", "delete runs/_archive/" });
            table.AddRow({ "forge clean scenario <scenario>", "delete runs/<scenario>/ (its checkpoints and logs)" });
            table.AddRow({ "forge clean exports", "delete the exported models" });
            table.AddRow({ "forge clean logs", "delete the learner and export logs" });
            table.AddRow({ "forge clean all", "all of the above, every run included (idle only)" });
            table.AddRow({ "forge progress [seconds|off]", "show or set the periodic progress report interval" });

            handler->SendSysMessage("Animus Forge console commands:");
            table.Write(Reply(handler), "  ");
            return true;
        }

        static bool HandleStatus(ChatHandler* handler)
        {
            sAnimusForge->CommandStatus(Reply(handler));
            return true;
        }

        static bool HandleScenarios(ChatHandler* handler)
        {
            sAnimusForge->CommandScenarios(Reply(handler));
            return true;
        }

        static bool HandleStart(ChatHandler* handler, Tail scenarios)
        {
            return sAnimusForge->CommandStart(SplitNames(scenarios), Reply(handler));
        }

        static bool HandleResume(ChatHandler* handler, Tail scenarios)
        {
            return sAnimusForge->CommandResume(SplitNames(scenarios), Reply(handler));
        }

        static bool HandlePause(ChatHandler* handler)
        {
            return sAnimusForge->CommandPause(Reply(handler));
        }

        static bool HandleCancel(ChatHandler* handler)
        {
            return sAnimusForge->CommandCancel(Reply(handler));
        }

        static bool HandleSkip(ChatHandler* handler)
        {
            return sAnimusForge->CommandSkip(Reply(handler));
        }

        static bool HandleRun(ChatHandler* handler, std::string scenario, std::string policy, Optional<uint32> episodes)
        {
            return sAnimusForge->CommandRun(scenario, policy, episodes.value_or(0), Reply(handler));
        }

        static bool HandleExport(ChatHandler* handler, Optional<std::string> first, Optional<std::string> second)
        {
            // `forge export best` exports the current scenario's best.pt.
            std::string scenario = first.value_or("");
            std::string checkpoint = second.value_or("");
            if (checkpoint.empty() && (scenario == "best" || scenario == "latest"))
                std::swap(scenario, checkpoint);

            return sAnimusForge->CommandExport(scenario, checkpoint, Reply(handler));
        }

        static bool HandleClean(ChatHandler* handler, Optional<std::string> target, Optional<std::string> scenario)
        {
            return sAnimusForge->CommandClean(target.value_or(""), scenario.value_or(""), Reply(handler));
        }

        static bool HandleProgress(ChatHandler* handler, Optional<std::string> interval)
        {
            if (!interval)
            {
                sAnimusForge->CommandProgress(std::nullopt, Reply(handler));
                return true;
            }

            if (*interval == "off")
            {
                sAnimusForge->CommandProgress(0, Reply(handler));
                return true;
            }

            Optional<uint32> const seconds = Acore::StringTo<uint32>(*interval);
            if (!seconds)
            {
                handler->SendSysMessage("Usage: forge progress [seconds|off]");
                return false;
            }

            sAnimusForge->CommandProgress(*seconds, Reply(handler));
            return true;
        }
    };
}

void AddSC_animus_forge_commands()
{
    new ForgeCommandScript();
}
