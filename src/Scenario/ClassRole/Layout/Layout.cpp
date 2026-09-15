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

#include "Layout.h"
#include "JsonWriter.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StageDefinition.h"

namespace
{
    /// Manifest format: 3 lists blocks generically (format 2 had one fixed field per stage block).
    constexpr uint32 MANIFEST_FORMAT = 3;
}

std::string_view AnimusForge::ClassRole::BlockName(BlockId id)
{
    switch (id)
    {
        case BlockId::Core:      return "core";
        case BlockId::Duel:      return "duel";
        case BlockId::Pack:      return "pack";
        case BlockId::Gauntlet:  return "gauntlet";
        case BlockId::Companion: return "companion";
        case BlockId::Party:     return "party";
        case BlockId::Pvp:       return "pvp";
        case BlockId::Count:     break;
    }

    return "unknown";
}

std::optional<AnimusForge::ClassRole::BlockId> AnimusForge::ClassRole::FindBlock(std::string_view name)
{
    for (std::size_t i = 0; i < BLOCK_COUNT; ++i)
        if (BlockName(BlockId(i)) == name)
            return BlockId(i);

    return std::nullopt;
}

AnimusForge::ClassRole::Layout AnimusForge::ClassRole::Layout::Build(ClassRoleProfile const& profile,
    StageDefinition const& stage)
{
    Layout layout;
    layout.Stage = &stage;
    layout.Profile = &profile;
    layout.Assets = &ClassRoleAssets::For(profile);
    layout.Blocks = stage.Blocks;

    // Heals that take a friendly unit target, resurrections and the soulstone can be cast on an ally (companion and
    // party blocks).
    if (stage.Has(BlockId::Companion) || stage.Has(BlockId::Party))
    {
        for (ActionCatalog::Action const& heal : layout.Catalog().Sustain())
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(heal.FirstRank);
                info && info->IsPositive() && info->NeedsExplicitUnitTarget())
                layout.AllyHeals.push_back(heal);

        layout.AllyRevives = layout.Catalog().Revives();
    }

    // Each block starts where the previous one ended.
    for (BlockId id : layout.Blocks)
    {
        BlockSize const size = GetBlock(id).Size(layout);
        layout.Slices[std::size_t(id)] = { layout.ObsDim, size.Obs, layout.NumActions, size.Actions };
        layout.ObsDim += size.Obs;
        layout.NumActions += size.Actions;
        layout._blockMask |= 1u << uint32(id);
    }

    return layout;
}

std::optional<AnimusForge::ClassRole::BlockId> AnimusForge::ClassRole::Layout::BlockOfAction(uint32 action) const
{
    for (BlockId id : Blocks)
        if (Slice(id).ContainsAction(action))
            return id;

    return std::nullopt;
}

void AnimusForge::ClassRole::WriteSpellList(JsonWriter& json, std::vector<ActionCatalog::Action> const& actions)
{
    json.Array(actions, [](JsonWriter& out, ActionCatalog::Action const& action) { out.Value(action.FirstRank); });
}

std::string AnimusForge::ClassRole::Layout::ModelName() const
{
    return Profile->Name + Stage->Suffix;
}

std::string AnimusForge::ClassRole::Layout::Manifest() const
{
    // Catalogs run to a few hundred actions: size the buffer once for the largest layouts.
    JsonWriter json(8192);

    json.BeginObject()
        .Key("format").Value(MANIFEST_FORMAT)
        .Key("model").Value(ModelName())
        .Key("stage").Value(Stage->Name)
        .Key("class_role").Value(Profile->Name)
        .Key("class").Value(Profile->Class)
        .Key("role").Value(RoleName(PlayRole()))
        .Key("obs_dim").Value(ObsDim)
        .Key("num_actions").Value(NumActions)
        .Key("specs").Array(Profile->Specs, [](JsonWriter& out, SpecProfile const& spec) { out.Value(spec.TabPage); });

    json.Key("blocks").BeginArray();
    for (BlockId id : Blocks)
    {
        BlockSlice const& slice = Slice(id);
        json.BeginObject()
            .Key("name").Value(BlockName(id))
            .Key("obs").Span(slice.ObsFirst, slice.ObsCount)
            .Key("actions").Span(slice.ActionFirst, slice.ActionCount);
        GetBlock(id).DescribeManifest(*this, json);
        json.EndObject();
    }

    json.EndArray().EndObject();
    return json.Str();
}
