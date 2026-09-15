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
    /// and pet, what it carries (potions, healthstones, bandages, a soulstone), death, and a hunter's stable. Actions:
    /// movement, auto-attack, pet attack, stop casting, cancel form, the consumables, resurrecting itself when dead,
    /// call a stabled beast.
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
            OBS_HEALTH_POTIONS          = 20,   // carried / CONSUMABLE_COUNT
            OBS_MANA_POTIONS            = 21,
            OBS_HEALTHSTONES            = 22,   // carried (at most 1)
            OBS_BANDAGES                = 23,   // carried / CONSUMABLE_COUNT
            OBS_POTION_COOLDOWN         = 24,   // the shared potion cooldown left, as a fraction
            OBS_HEALTHSTONE_COOLDOWN    = 25,
            OBS_RECENTLY_BANDAGED       = 26,   // no bandage can be used yet
            OBS_SOULSTONE_ON_BOT        = 27,   // the bot will be able to resurrect itself if it dies
            OBS_DEAD                    = 28,
            OBS_SELF_RESURRECT          = 29,   // dead, and able to resurrect itself (Soulstone, Reincarnation)
            OBS_STABLE_FIRST            = 30,   // hunters: per stable slot STABLE_FEATURES
            OBS_COUNT_WITHOUT_STABLE    = 30
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
            ACTION_HEALTH_POTION        = 9,    // drink a healing potion
            ACTION_MANA_POTION          = 10,
            ACTION_HEALTHSTONE          = 11,
            ACTION_BANDAGE              = 12,   // bandage itself (a channel, broken by damage)
            ACTION_SOULSTONE_SELF       = 13,   // warlocks: soulstone itself
            ACTION_SELF_RESURRECT       = 14,   // dead: use its Soulstone or Reincarnation (not in the PvP stages)
            ACTION_CALL_BEAST_FIRST     = 15,   // hunters: call stable slot 0..STABLE_SLOTS-1
            ACTION_COUNT_WITHOUT_STABLE = 15
        };

        static constexpr float MOVE_TO_RANGE_DISTANCE = 24.0f;
        static constexpr float BACK_OFF_DISTANCE = 10.0f;

        [[nodiscard]] BlockId Id() const override { return BlockId::Duel; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void BeforeApply(SeatView& view) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;

        /// A dead bot's features and mask (every other block stays empty): dead, and whether it can resurrect itself.
        static void ObserveDead(SeatView const& view, float* obs, uint8* mask);
    };
}

#endif
