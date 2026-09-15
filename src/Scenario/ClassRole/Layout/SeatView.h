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

#ifndef MOD_ANIMUS_FORGE_CLASS_ROLE_SEAT_VIEW_H
#define MOD_ANIMUS_FORGE_CLASS_ROLE_SEAT_VIEW_H

#include "Block.h"
#include "ClassRoleProfile.h"
#include "ObjectGuid.h"
#include "TalentBuilder.h"
#include <array>

class Player;
class Unit;

namespace AnimusForge::ClassRole
{
    struct Layout;

    /// One bot's situation at a decision: what the blocks cannot read from the world themselves. The scenario fills
    /// it; each part is only used by the blocks that need it.
    struct SeatView
    {
        Layout const* L = nullptr;
        Player* Bot = nullptr;
        /// What the actions aim at: the dummy, the opponent, the selected enemy. May be null (between pulls).
        Unit* Target = nullptr;

        // Core: the character, as built, and what happened since the last decision.
        uint8 Level = 1;
        uint8 Race = 0;
        uint8 Spec = 0;
        TalentBuilder::Build const* Build = nullptr;
        float LastStepDamage = 0.0f;                // damage done / the level's damage scale
        float LastStepPowerDelta = 0.0f;            // primary power change, as a fraction of max
        float LastStepDamageTaken = 0.0f;           // / the bot's max health

        // Duel: time in combat, and a hunter's beasts on offer.
        float CombatTime = 0.0f;                    // time in combat / 60 s, clamped; 0 out of combat
        std::array<uint32, STABLE_SLOTS> Stable{};
        uint32 StableCount = 0;

        // Pack: the current pull's enemies, in slot order (null for a slot whose enemy is gone).
        std::array<Unit*, PACK_SLOTS> Enemies{};
        uint32 EnemyCount = 0;                      // slots in use; 0 between pulls
        uint32 TargetSlot = 0;                      // the selected enemy; target selection updates it

        // Gauntlet.
        uint32 PullsCleared = 0;
        float QuietTime = 0.0f;                     // time since the last fight ended / 20 s, clamped
        float PullTime = 0.0f;                      // time into the current pull / 60 s, clamped
        bool ElitePull = false;
        uint32 FoodItem = 0;
        uint32 DrinkItem = 0;

        // Companion: the player the bot fights for.
        Player* Owner = nullptr;

        // Party: the other learned players, and the party's living tank (may be the bot).
        struct Teammate
        {
            Player* Bot = nullptr;
            Role PlayRole = Role::Dps;
            uint8 Class = 0;
        };

        std::array<Teammate, PARTY_MEMBERS> Teammates{};
        Player* Tank = nullptr;

        // PvP: the enemy player.
        Player* Opponent = nullptr;
        uint8 OpponentClass = 0;
        Role OpponentRole = Role::Dps;
        bool Mirror = false;                        // the opponent is a learned agent too
    };

    /// What an applied action did, for the scenario's bookkeeping and rewards.
    struct SeatActionResult
    {
        uint32 SpellCasts = 0;
        uint32 TrinketUses = 0;
        uint32 SustainCasts = 0;
        uint32 FoodUsed = 0;
        uint32 DrinkUsed = 0;
        bool StealthOpener = false;                 // a stealth opener at the target started
        ObjectGuid PendingInterrupt;                // an interrupt was cast at this casting enemy
        uint32 CallBeast = 0;                       // hunters: call this stable beast (the scenario creates the pet)
    };
}

#endif
