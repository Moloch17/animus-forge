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
#include "ForgeConfig.h"
#include "StageDefinition.h"
#include "StageScenario.h"

/*
 * Every scenario the module can run, by the name used in AnimusForge.Queue: the curriculum's stages (CurriculumStages).
 * Adding a standalone scenario = implementing Scenario, one branch in CreateScenario and its name in ScenarioNames.
 */

std::unique_ptr<AnimusForge::Scenario> AnimusForge::CreateScenario(std::string const& name, ForgeConfig const& config)
{
    if (Curriculum::StageDefinition const* stage = Curriculum::FindStage(name))
        return std::make_unique<Curriculum::StageScenario>(config, *stage);

    return nullptr;
}

std::vector<std::string> AnimusForge::ScenarioNames()
{
    std::vector<std::string> names;
    for (Curriculum::StageDefinition const& stage : Curriculum::CurriculumStages())
        names.push_back(stage.Name);

    return names;
}
