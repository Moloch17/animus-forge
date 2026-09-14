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
#include <vector>

class Creature;
class Player;

namespace AnimusForge::CompanionOwner
{
    /// Everything needed to dress a bot as a player of one class: its damage role's kit, talents and gear.
    struct Template
    {
        ClassRoleProfile const* Profile = nullptr;
        std::vector<uint8> Races;
        std::unique_ptr<ClassKit> Kit;
        std::unique_ptr<TalentBuilder> Talents;
        std::unique_ptr<GearBuilder> Gear;
    };

    /// One template per class, built once (item pools and trainer data take a few seconds).
    class Templates
    {
    public:
        static Templates const& Instance();

        [[nodiscard]] Template const* ForClass(uint8 playerClass) const;

        /// Classes a player of `level` can be.
        [[nodiscard]] std::vector<uint8> ClassesForLevel(uint8 level) const;

    private:
        Templates();

        std::map<uint8, Template> _byClass;
    };

    /// The scripted owner's timers and repertoire.
    struct State
    {
        uint32 EngageMs = 0;            // don't engage a pull before this episode time
        uint32 NextMoveMs = 0;
        uint32 NextSpellMs = 0;
        uint32 NextRegenMs = 0;
        std::vector<uint32> Spells;     // damage spells the owner knows (highest ranks)
    };

    /// Dress a placed bot of the template's class: proficiencies, a random build of a random spec of
    /// the class's damage role, trainer spells for its level, gear. Fills state.Spells.
    void Configure(Player* owner, Template const& owner_template, State& state);

    /// One decision of the scripted owner. Between pulls it wanders near `home`, recovering health and
    /// mana; once a pull is up (and state.EngageMs has passed) it walks to the nearest enemy, preferring
    /// ones already attacking it, auto-attacks and now and then casts one of its damage spells.
    void Update(Player* owner, std::vector<Creature*> const& enemies, uint32 nowMs, Position const& home, State& state);
}

#endif
