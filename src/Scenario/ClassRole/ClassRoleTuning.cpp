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

#include "ClassRoleTuning.h"
#include "Config.h"
#include "JsonWriter.h"
#include <algorithm>

namespace
{
    constexpr char const* KEY_PREFIX = "AnimusForge.ClassRole.";
}

AnimusForge::ClassRole::ClassRoleTuning AnimusForge::ClassRole::ClassRoleTuning::Load()
{
    ClassRoleTuning tuning;
    Visit(tuning, [](char const* key, auto& value)
    {
        // Every key is optional: a missing one keeps its default without a "missing property" warning.
        value = sConfigMgr->GetOption(std::string(KEY_PREFIX) + key, value, false);
    });

    auto const order = [](uint32& low, uint32& high) { if (low > high) std::swap(low, high); };
    order(tuning.Pulls.NextPullMinMs, tuning.Pulls.NextPullMaxMs);
    order(tuning.Pulls.OwnerEngageMinMs, tuning.Pulls.OwnerEngageMaxMs);
    order(tuning.Pulls.PartyOwnerEngageMinMs, tuning.Pulls.PartyOwnerEngageMaxMs);
    order(tuning.Pulls.OwnerPullsMinMs, tuning.Pulls.OwnerPullsMaxMs);
    order(tuning.ScriptedPlayers.SpellMinMs, tuning.ScriptedPlayers.SpellMaxMs);
    order(tuning.ScriptedPlayers.HealMinMs, tuning.ScriptedPlayers.HealMaxMs);
    order(tuning.ScriptedPlayers.WanderMinMs, tuning.ScriptedPlayers.WanderMaxMs);

    tuning.Party.SizeWeight1 = std::max(0, tuning.Party.SizeWeight1);
    tuning.Party.SizeWeight2 = std::max(0, tuning.Party.SizeWeight2);
    tuning.Party.SizeWeight3 = std::max(0, tuning.Party.SizeWeight3);
    tuning.Party.SizeWeight4 = std::max(0, tuning.Party.SizeWeight4);
    return tuning;
}

std::string AnimusForge::ClassRole::ClassRoleTuning::Json() const
{
    JsonWriter json(2048);
    json.BeginObject();
    Visit(*this, [&json](char const* key, auto const& value) { json.Key(key).Value(value); });
    json.EndObject();
    return json.Str();
}
