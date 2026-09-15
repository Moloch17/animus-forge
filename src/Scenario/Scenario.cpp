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
#include "ClassRoleScenario.h"
#include "ForgeConfig.h"
#include "StageDefinition.h"
#include "WarriorDummy20Scenario.h"

/*
 * Every scenario the module can run, by the name used in AnimusForge.Queue: the class/role curriculum's stages
 * (ClassRoleStages) and the standalone scenarios below. Adding a standalone scenario = implementing Scenario and one
 * branch here.
 */

std::unique_ptr<AnimusForge::Scenario> AnimusForge::CreateScenario(std::string const& name, ForgeConfig const& config)
{
    if (ClassRole::StageDefinition const* stage = ClassRole::FindStage(name))
        return std::make_unique<ClassRole::ClassRoleScenario>(config, *stage);

    if (name == "warrior_dummy_20")
        return std::make_unique<WarriorDummy20Scenario>(config);

    return nullptr;
}

std::vector<std::string> AnimusForge::ScenarioNames()
{
    std::vector<std::string> names;
    for (ClassRole::StageDefinition const& stage : ClassRole::ClassRoleStages())
        names.push_back(stage.Name);

    names.push_back("warrior_dummy_20");
    return names;
}
