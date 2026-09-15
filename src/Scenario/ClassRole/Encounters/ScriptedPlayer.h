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

#ifndef MOD_ANIMUS_FORGE_CLASS_ROLE_SCRIPTED_PLAYER_H
#define MOD_ANIMUS_FORGE_CLASS_ROLE_SCRIPTED_PLAYER_H

#include "ClassRoleAssets.h"
#include "ClassRoleTuning.h"
#include "Position.h"
#include <vector>

class Player;
class Unit;

/*
 * Scripted players: the companion and party stages' owner, and the PvP stage's opponent. Deliberately simple and
 * readable -- they are the situations the learned policies train in, not policies themselves. How fast and how well
 * they play is tuning (AnimusForge.ClassRole.ScriptedPlayers.*).
 */
namespace AnimusForge::ClassRole::ScriptedPlayer
{
    using Tuning = ClassRoleTuning::ScriptedPlayerTuning;

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

    /// Dress a placed bot of the assets' class and role: proficiencies, a random build of one of the role's specs,
    /// trainer spells for its level, gear (PvP gear too when `pvp`). Fills state's repertoire and role.
    void Configure(Player* player, ClassRoleAssets const& assets, State& state, bool pvp);

    /// One decision of a scripted party member (see State::PlayRole):
    /// - tank: engages first, goes for enemies attacking someone else and taunts them off;
    /// - healer: heals the most hurt party member, keeps near the tank, and only casts damage spells when nobody needs
    ///   healing;
    /// - damage dealer: between pulls it wanders near `home`, recovering health and mana; once a pull is up (and
    ///   state.EngageMs has passed) it fights the tank's target, else the enemy attacking it, else the nearest.
    /// `party` is every party player, the member itself included; `tank` the party's tank (may be null).
    void UpdateMember(Player* member, std::vector<Player*> const& party, Player* tank,
        std::vector<Unit*> const& enemies, uint32 nowMs, Position const& home, State& state, Tuning const& tuning);

    /// One decision of a scripted PvP opponent fighting `enemy`: a healer heals itself when hurt, a ranged spec keeps
    /// its distance and casts, a melee spec closes in and fights; all use damage spells.
    void UpdateOpponent(Player* player, Player* enemy, uint32 nowMs, State& state, Tuning const& tuning);
}

#endif
