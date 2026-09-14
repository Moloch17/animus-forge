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

#include "Scenario.h"
#include "AnimusConfig.h"
#include "WarriorDummyScenario.h"
#include <functional>
#include <utility>

namespace
{
    using ScenarioFactory = std::function<std::unique_ptr<Animus::Scenario>(Animus::ForgeConfig const&)>;

    /// Every scenario the module can run, by the name used in AnimusForge.Scenario.
    /// Adding a scenario = implementing Animus::Scenario and adding one row here.
    std::vector<std::pair<std::string, ScenarioFactory>> const& Registry()
    {
        static std::vector<std::pair<std::string, ScenarioFactory>> const registry =
        {
            {
                "warrior_dummy",
                [](Animus::ForgeConfig const& config)
                {
                    return std::make_unique<Animus::WarriorDummyScenario>(config);
                }
            },
        };

        return registry;
    }
}

std::unique_ptr<Animus::Scenario> Animus::CreateScenario(ForgeConfig const& config)
{
    for (auto const& [name, factory] : Registry())
        if (name == config.Scenario)
            return factory(config);

    return nullptr;
}

std::vector<std::string> Animus::ScenarioNames()
{
    std::vector<std::string> names;
    for (auto const& entry : Registry())
        names.push_back(entry.first);

    return names;
}
