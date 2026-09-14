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

#ifndef MOD_ANIMUS_FORGE_ACTION_CATALOG_H
#define MOD_ANIMUS_FORGE_ACTION_CATALOG_H

#include "Define.h"
#include <string>
#include <vector>

class Player;
class SpellInfo;

namespace AnimusForge
{
    class ClassKit;
    class TalentBuilder;

    /// The fixed action space of one class: every combat spell any of its races can know by level 80
    /// (trainer spells, starting spells, racials, active talents), plus trinket uses.
    ///
    /// A spell action stands for a whole rank chain and casts the highest rank the bot knows. Actions
    /// the current bot cannot use (other race, level too low, talent not taken, on cooldown, wrong
    /// stance) are masked every decision.
    class ActionCatalog
    {
    public:
        enum class Kind : uint8
        {
            Noop,
            CancelQueued,       // cancel a queued on-next-swing ability
            Spell,
            Trinket,
        };

        struct Action
        {
            Kind Type = Kind::Noop;
            std::string Name;
            uint32 FirstRank = 0;       // Spell: first spell of the rank chain
            uint8 EquipmentSlot = 0;    // Trinket: EQUIPMENT_SLOT_TRINKET1/2
            bool NextSwing = false;
        };

        ActionCatalog(uint8 playerClass, ClassKit const& kit, TalentBuilder const& talents);

        [[nodiscard]] std::vector<Action> const& Actions() const { return _actions; }

        /// Highest rank of a spell action the bot knows, or nullptr.
        [[nodiscard]] static SpellInfo const* KnownRank(Player const* bot, uint32 firstRank);

        /// Whether a spell is worth an action slot for damage on a target dummy.
        [[nodiscard]] static bool IsCombatSpell(SpellInfo const* info);

    private:
        std::vector<Action> _actions;
    };
}

#endif
