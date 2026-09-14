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

#ifndef MOD_ANIMUS_FORGE_COMPANION_OWNER_H
#define MOD_ANIMUS_FORGE_COMPANION_OWNER_H

#include "ClassKit.h"
#include "ClassRoleProfile.h"
#include "GearBuilder.h"
#include "Position.h"
#include "TalentBuilder.h"
#include <map>
#include <memory>
#include <utility>
#include <vector>

class Creature;
class Player;
class Unit;

/*
 * Scripted players: the companion stage's owner, a party's other members, and the PvP stage's opponent.
 * Deliberately simple and readable -- they are the situations the learned policies train in, not policies
 * themselves.
 */
namespace AnimusForge::CompanionOwner
{
    /// Everything needed to dress a bot as a player of one class in one role: its kit, talents and gear.
    struct Template
    {
        ClassRoleProfile const* Profile = nullptr;
        std::vector<uint8> Races;
        std::unique_ptr<ClassKit> Kit;
        std::unique_ptr<TalentBuilder> Talents;
        std::unique_ptr<GearBuilder> Gear;
    };

    /// One template per class and role, built once (item pools and trainer data take a few seconds).
    class Templates
    {
    public:
        static Templates const& Instance();

        /// The class's damage role.
        [[nodiscard]] Template const* ForClass(uint8 playerClass) const { return ForClassRole(playerClass, Role::Dps); }
        [[nodiscard]] Template const* ForClassRole(uint8 playerClass, Role role) const;

        /// Classes a player of `level` can be (damage role), and those that can fill `role`.
        [[nodiscard]] std::vector<uint8> ClassesForLevel(uint8 level) const { return ClassesForRole(level, Role::Dps); }
        [[nodiscard]] std::vector<uint8> ClassesForRole(uint8 level, Role role) const;

    private:
        Templates();

        std::map<std::pair<uint8, Role>, Template> _byClassRole;
    };

    /// A scripted player's role, timers and repertoire.
    struct State
    {
        Role PlayRole = Role::Dps;
        bool Ranged = false;            // its spec fights at range
        uint32 EngageMs = 0;            // don't engage a pull before this episode time
        uint32 NextMoveMs = 0;
        uint32 NextSpellMs = 0;
        uint32 NextHealMs = 0;
        uint32 NextRegenMs = 0;
        std::vector<uint32> Spells;     // harmful single-target combat spells it knows (highest ranks)
        std::vector<uint32> Heals;      // single-target heals it knows
        std::vector<uint32> Taunts;     // single-target taunts it knows
    };

    /// Dress a placed bot of the template's class and role: proficiencies, a random build of one of the
    /// role's specs, trainer spells for its level, gear. Fills state's repertoire and role.
    void Configure(Player* player, Template const& player_template, State& state);

    /// One decision of a scripted damage dealer (the companion's owner). Between pulls it wanders near
    /// `home`, recovering health and mana; once a pull is up (and state.EngageMs has passed) it walks to
    /// `preferred` if given and alive, else the enemy attacking it, else the nearest, auto-attacks and now
    /// and then casts one of its damage spells.
    void Update(Player* player, std::vector<Unit*> const& enemies, uint32 nowMs, Position const& home, State& state,
        Unit* preferred = nullptr);

    /// One decision of a scripted party member (see State::PlayRole):
    /// - tank: engages first, goes for enemies attacking someone else and taunts them off;
    /// - healer: heals the most hurt party member (below 85%), keeps within 30 yd of the tank, and only
    ///   casts damage spells when nobody needs healing;
    /// - damage dealer: Update, on the tank's target once the tank has one.
    /// `party` is every party player, the member itself included; `tank` the party's tank (may be null).
    void UpdateMember(Player* member, std::vector<Player*> const& party, Player* tank,
        std::vector<Unit*> const& enemies, uint32 nowMs, Position const& home, State& state);

    /// One decision of a scripted PvP opponent fighting `enemy`: a healer heals itself below 60% health, a ranged
    /// spec keeps 10-30 yd (closing to 25) and casts, a melee spec closes in and fights; all use damage spells.
    void UpdateOpponent(Player* player, Player* enemy, uint32 nowMs, State& state);
}

#endif
