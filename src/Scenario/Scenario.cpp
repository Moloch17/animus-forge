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
#include "ClassRoleProfile.h"
#include "ClassRoleScenario.h"
#include "ForgeConfig.h"
#include "WarriorDummy20Scenario.h"
#include "WarriorDummyScenario.h"
#include <functional>
#include <utility>

namespace
{
    using ScenarioFactory = std::function<std::unique_ptr<AnimusForge::Scenario>(AnimusForge::ForgeConfig const&)>;

    /// Every scenario the module can run, by the name used in AnimusForge.Scenario.
    /// Adding a scenario = implementing AnimusForge::Scenario and adding one row here; class/role
    /// scenarios come from ClassRoleProfiles().
    std::vector<std::pair<std::string, ScenarioFactory>> const& Registry()
    {
        static std::vector<std::pair<std::string, ScenarioFactory>> const registry = []()
        {
            std::vector<std::pair<std::string, ScenarioFactory>> scenarios =
            {
                {
                    "warrior_dummy",
                    [](AnimusForge::ForgeConfig const& config)
                    {
                        return std::make_unique<AnimusForge::WarriorDummyScenario>(config);
                    }
                },
                {
                    "warrior_dummy_20",
                    [](AnimusForge::ForgeConfig const& config)
                    {
                        return std::make_unique<AnimusForge::WarriorDummy20Scenario>(config);
                    }
                },
            };

            // Curriculum stages, each its own scenario so every stage stays repeatable.
            using AnimusForge::ArenaMode;
            using AnimusForge::ClassRoleScenario;

            for (ArenaMode mode : { ArenaMode::Dummy, ArenaMode::Duel })
            {
                for (AnimusForge::ClassRoleProfile const& profile : AnimusForge::ClassRoleProfiles())
                {
                    scenarios.emplace_back(ClassRoleScenario::ScenarioName(profile, mode),
                        [&profile, mode](AnimusForge::ForgeConfig const& config)
                        {
                            return std::unique_ptr<AnimusForge::Scenario>(
                                std::make_unique<ClassRoleScenario>(profile, config, mode));
                        });
                }
            }

            return scenarios;
        }();

        return registry;
    }
}

std::unique_ptr<AnimusForge::Scenario> AnimusForge::CreateScenario(ForgeConfig const& config)
{
    for (auto const& [name, factory] : Registry())
        if (name == config.Scenario)
            return factory(config);

    return nullptr;
}

std::vector<std::string> AnimusForge::ScenarioNames()
{
    std::vector<std::string> names;
    for (auto const& entry : Registry())
        names.push_back(entry.first);

    return names;
}
