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

#ifndef MOD_ANIMUS_FORGE_CURRICULUM_PVP_BLOCK_H
#define MOD_ANIMUS_FORGE_CURRICULUM_PVP_BLOCK_H

#include "Block.h"

namespace AnimusForge::Curriculum
{
    /// An enemy player: its class, role, resources and state, and the loss-of-control effects on the bot. No actions:
    /// the core and duel actions fight it.
    class PvpBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_OPPONENT_CLASS_FIRST    = 0,    // one-hot over PLAYABLE_CLASSES
            OBS_OPPONENT_ROLE_FIRST     = 10,   // one-hot: damage, tank, healer
            OBS_OPPONENT_LEVEL_DIFF     = 13,   // (its level - the bot's) / 5
            OBS_OPPONENT_MANA           = 14,
            OBS_OPPONENT_RAGE_ENERGY    = 15,   // rage, energy or runic power as a fraction
            OBS_OPPONENT_CONTROLLED     = 16,   // stunned, feared, confused, rooted, silenced or polymorphed
            OBS_OPPONENT_STEALTHED      = 17,
            OBS_OPPONENT_PET_OUT        = 18,
            OBS_OPPONENT_HEALING        = 19,   // casting a heal
            OBS_BOT_STUNNED             = 20,   // stunned, feared or confused: no actions land
            OBS_BOT_ROOTED              = 21,
            OBS_BOT_SILENCED            = 22,
            OBS_MIRROR                  = 23,   // the opponent is a learned agent too
            OBS_COUNT                   = 24
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Pvp; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
    };
}

#endif
