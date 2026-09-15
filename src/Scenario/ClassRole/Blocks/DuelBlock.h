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

#ifndef MOD_ANIMUS_FORGE_CLASS_ROLE_DUEL_BLOCK_H
#define MOD_ANIMUS_FORGE_CLASS_ROLE_DUEL_BLOCK_H

#include "Block.h"

namespace AnimusForge::ClassRole
{
    /// Fighting something that fights back: where the target is and what it does, the bot's movement, casting, form
    /// and pet, and a hunter's stable. Actions: movement, auto-attack, pet attack, stop casting, cancel form, call a
    /// stabled beast.
    class DuelBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_DISTANCE                = 0,    // yards / 60
            OBS_BEARING_SIN             = 1,    // direction to the target relative to the bot's facing
            OBS_BEARING_COS             = 2,
            OBS_BEHIND_TARGET           = 3,    // the bot is in the target's back arc
            OBS_TARGET_FACING_BOT       = 4,
            OBS_TARGET_IN_COMBAT        = 5,
            OBS_TARGET_ATTACKS_BOT      = 6,
            OBS_TARGET_CASTING          = 7,
            OBS_BOT_MOVING              = 8,
            OBS_BOT_IN_COMBAT           = 9,
            OBS_BOT_STEALTHED           = 10,
            OBS_BOT_AUTO_ATTACKING      = 11,
            OBS_DAMAGE_TAKEN            = 12,   // since the last decision / bot max health
            OBS_PET_OUT                 = 13,
            OBS_PET_HEALTH              = 14,
            OBS_PET_ATTACKING           = 15,   // the pet's victim is the target
            OBS_COMBAT_TIME             = 16,   // time the bot has been in combat / 60 s; 0 out of combat
            OBS_CAST_PROGRESS           = 17,   // fraction of the current cast time done; 0 when not casting
            OBS_CAST_REMAINING          = 18,   // seconds left of the current cast / 3
            OBS_SHAPESHIFTED            = 19,   // in a form the bot can cancel
            OBS_STABLE_FIRST            = 20,   // hunters: per stable slot STABLE_FEATURES
            OBS_COUNT_WITHOUT_STABLE    = 20
        };

        /// Per stabled beast: offered, family / 50, ferocity, tenacity, cunning.
        static constexpr uint32 STABLE_FEATURES = 5;

        enum Action : uint32
        {
            ACTION_MOVE_TO_TARGET       = 0,    // run to melee reach, on the side the bot is on
            ACTION_MOVE_BEHIND          = 1,    // run to melee reach behind the target
            ACTION_MOVE_TO_RANGE        = 2,    // run to casting range (MOVE_TO_RANGE_DISTANCE)
            ACTION_BACK_OFF             = 3,    // run BACK_OFF_DISTANCE further away
            ACTION_STOP                 = 4,
            ACTION_START_ATTACK         = 5,    // start auto-attack on the target
            ACTION_PET_ATTACK           = 6,    // send pets and guardians at the target
            ACTION_STOP_CASTING         = 7,    // cancel the current cast or channel
            ACTION_CANCEL_FORM          = 8,    // leave the current shapeshift form, as right-clicking it does
            ACTION_CALL_BEAST_FIRST     = 9,    // hunters: call stable slot 0..STABLE_SLOTS-1
            ACTION_COUNT_WITHOUT_STABLE = 9
        };

        static constexpr float MOVE_TO_RANGE_DISTANCE = 24.0f;
        static constexpr float BACK_OFF_DISTANCE = 10.0f;

        [[nodiscard]] BlockId Id() const override { return BlockId::Duel; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, JsonWriter& json) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void BeforeApply(SeatView& view) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
    };
}

#endif
