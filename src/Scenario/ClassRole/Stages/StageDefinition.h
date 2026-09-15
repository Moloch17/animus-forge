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

#ifndef MOD_ANIMUS_FORGE_CLASS_ROLE_STAGE_DEFINITION_H
#define MOD_ANIMUS_FORGE_CLASS_ROLE_STAGE_DEFINITION_H

#include "Block.h"
#include <string>
#include <string_view>
#include <vector>

namespace AnimusForge::ClassRole
{
    /// Who the learned agents of an env are.
    enum class SeatPlan : uint8
    {
        Solo,           // one seat
        Party,          // four seats (1-4 with a character each episode): a tank, a healer and damage dealers
        Mirror,         // two seats that fight each other (self-play)
    };

    /// What the seats fight.
    enum class Opposition : uint8
    {
        Creature,       // one same-level creature spawned out of aggro range
        Pulls,          // packs of creatures (see PullSchedule)
        ScriptedPlayer, // an enemy player played by a script
        MirrorSeat,     // the other seat (SeatPlan::Mirror)
    };

    enum class PullSchedule : uint8
    {
        None,
        SinglePack,     // one pack; the episode ends when it is cleared
        Gauntlet,       // pull after pull with a break between, until the episode ends
    };

    /// One curriculum stage: its own scenario (`stage1_duel`, ...), its blocks and what its envs contain.
    ///
    /// A stage extends one earlier stage, whose best model seeds it: the base's blocks this stage keeps are seeded
    /// block by block (their features and actions may move), dropped ones are left behind and new ones start fresh.
    /// Several stages may extend the same base, so the curriculum is a tree.
    struct StageDefinition
    {
        std::string Name;               // the scenario name
        std::string Suffix;             // added to a class/role's name for the stage's models (warrior_dps_duel)
        std::string Extends;            // the stage it builds on and seeds from; empty for the first
        std::string Summary;
        std::vector<BlockId> Blocks;    // in layout order
        SeatPlan Seats = SeatPlan::Solo;
        Opposition Against = Opposition::Creature;
        PullSchedule Schedule = PullSchedule::None;
        bool Owner = false;             // a scripted owner the seats fight for
        bool PartyGroup = false;        // the owner and seats form a core group

        [[nodiscard]] bool Has(BlockId block) const;
        [[nodiscard]] uint32 SeatCount() const;
    };

    /// Every class/role stage, every base before the stages that extend it. Invalid definitions (an unknown or later
    /// base, a repeated block, parts that need a missing block) are logged and left out.
    [[nodiscard]] std::vector<StageDefinition> const& ClassRoleStages();

    [[nodiscard]] StageDefinition const* FindStage(std::string_view name);
}

#endif
