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

#ifndef MOD_ANIMUS_FORGE_CURRICULUM_BLOCK_H
#define MOD_ANIMUS_FORGE_CURRICULUM_BLOCK_H

#include "Define.h"
#include <array>
#include <boost/json/fwd.hpp>
#include <optional>
#include <string_view>

/*
 * A layout block: one group of observation features and actions of a class/role policy (the class's spells, the
 * duel's movement, the pack's enemy slots, ...). A stage is an ordered list of blocks (see StageDefinition), and a
 * layout places each block's features and actions after the previous block's (see Layout).
 *
 * Blocks are stateless: everything they read comes from the world and the SeatView, so the same code encodes a seat
 * in training and a companion in play.
 */
namespace AnimusForge::Curriculum
{
    struct Layout;
    struct SeatActionResult;
    struct SeatView;

    enum class BlockId : uint8
    {
        Core,           // the character, its spells, trinkets and talents
        Duel,           // movement, auto-attack, pets, stopping casts and forms, the opponent's position
        Pack,           // enemy slots, target selection, tactical spells
        Gauntlet,       // pull timing, food, drink, sustain spells
        Companion,      // the owner: follow, assist, guard, heal it
        Party,          // three teammates: follow the tank, assist, guard and heal them
        Pvp,            // the enemy player's class, role and state
        Count
    };

    constexpr std::size_t BLOCK_COUNT = std::size_t(BlockId::Count);

    // Sizes several blocks and the scenario agree on.
    constexpr uint32 MAX_SEATS = 4;         // learned agents per env: 1, an arena's 2 or a party's 4
    constexpr uint32 PARTY_MEMBERS = 3;     // a party seat's teammates
    constexpr uint32 PACK_SLOTS = 4;        // enemies observed
    constexpr uint32 STABLE_SLOTS = 4;      // a hunter's stabled beasts

    [[nodiscard]] std::string_view BlockName(BlockId id);
    [[nodiscard]] std::optional<BlockId> FindBlock(std::string_view name);

    /// A block's place in a layout's observation row and action mask.
    struct BlockSlice
    {
        uint32 ObsFirst = 0;
        uint32 ObsCount = 0;
        uint32 ActionFirst = 0;
        uint32 ActionCount = 0;

        [[nodiscard]] bool ContainsAction(uint32 action) const
        {
            return action >= ActionFirst && action < ActionFirst + ActionCount;
        }
    };

    struct BlockSize
    {
        uint32 Obs = 0;
        uint32 Actions = 0;
    };

    class Block
    {
    public:
        virtual ~Block() = default;

        [[nodiscard]] virtual BlockId Id() const = 0;

        /// The features and actions the block adds to `layout` (profile, assets and ally heals are set).
        [[nodiscard]] virtual BlockSize Size(Layout const& layout) const = 0;

        /// Block-specific manifest entries (spell lists, slot counts), written inside the block's manifest object.
        virtual void DescribeManifest(Layout const& /*layout*/, boost::json::object& /*block*/) const { }

        /// Write the block's features and action mask for a living bot: `obs` and `mask` point at the block's slice.
        /// `mask` is null when no mask is wanted (an ended episode's final observation): skip the cast checks.
        virtual void Observe(SeatView const& view, float* obs, uint8* mask) const = 0;

        /// Before an action of a layout with this block is applied (whichever block the action belongs to).
        virtual void BeforeApply(SeatView& /*view*/) const { }

        /// Apply the block's action `local` (0-based within the block) as the client would. Masked actions do nothing.
        virtual void Apply(SeatView& /*view*/, uint32 /*local*/, SeatActionResult& /*result*/) const { }
    };

    /// The block implementation of `id`.
    [[nodiscard]] Block const& GetBlock(BlockId id);
}

#endif
