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

#ifndef MOD_ANIMUS_FORGE_CURRICULUM_STAGE_STATE_H
#define MOD_ANIMUS_FORGE_CURRICULUM_STAGE_STATE_H

#include "Block.h"
#include "BotSlot.h"
#include "RewardLedger.h"
#include "Supplies.h"
#include "TalentBuilder.h"
#include <array>
#include <vector>

/*
 * The state every curriculum stage has: the seats' characters and their episode totals. What only some stages have
 * (pulls, the owner, the party group, the enemy player) is kept by the encounter that needs it.
 */
namespace AnimusForge::Curriculum
{
    struct Layout;

    /// A seat's fight against whatever fights back (duel, pulls, PvP): what the rewards and episode info share.
    struct CombatTally
    {
        uint64 DamageTaken = 0;
        float LastDistance = -1.0f;             // approach shaping: excess distance at the last reward; < 0 = none yet
        bool Killed = false;                    // its opponent died (pack: the pull was cleared)
        uint32 KillTimeMs = 0;
        bool Died = false;                      // died at least once
        uint32 Deaths = 0;
        bool DeathCounted = false;              // the current death has been paid for (again after standing up)
        uint32 DeathMs = 0;                     // episode time of the current death
        uint32 StealthOpeners = 0;
        bool StepStealthOpener = false;         // one started since the last reward
        bool PetSummoned = false;
        uint32 CastsCompleted = 0;
        uint32 CastsCancelled = 0;
        uint64 CastMsWasted = 0;
        uint32 CastsStopped = 0;
        uint32 CastsMoved = 0;
        uint32 CastsTargetLost = 0;
        uint32 CastsOther = 0;
    };

    /// One learned agent: its character, as built for the episode, and its episode totals.
    struct SeatState
    {
        Layout const* L = nullptr;              // null for a party seat left empty this episode
        BotSlot Bot;

        uint8 Race = 0;
        uint8 Level = 1;
        uint8 Spec = 0;
        TalentBuilder::Build Build;
        uint32 UnspentTalentPoints = 0;
        uint32 EquippedItems = 0;
        float DamageScale = 1.0f;
        std::vector<uint32> Stable;             // hunters: beasts offered this episode

        uint32 LastPower = 0;
        float LastStepDamage = 0.0f;
        float LastStepPowerDelta = 0.0f;
        float LastStepDamageTaken = 0.0f;
        uint32 SpellCasts = 0;
        uint32 TrinketUses = 0;
        bool InCombat = false;
        uint32 CombatStartMs = 0;               // episode time the bot entered its current combat
        uint32 TargetSlot = 0;                  // the selected enemy (pulls)

        // What the character brought (potions, bandages, stones), and what it did with it.
        BattleSupplies Supplies;
        uint32 ConsumablesUsed = 0;
        uint32 SelfResurrections = 0;
        uint32 Revives = 0;                     // dead allies (owner, teammates) the seat resurrected
        bool StepRevivedAlly = false;           // an ally the seat resurrected stood up this decision

        CombatTally Combat;
        RewardLedger Rewards;

        /// Clear the episode totals (not the character).
        void ResetEpisode()
        {
            LastStepDamage = 0.0f;
            LastStepPowerDelta = 0.0f;
            LastStepDamageTaken = 0.0f;
            SpellCasts = 0;
            TrinketUses = 0;
            InCombat = false;
            CombatStartMs = 0;
            TargetSlot = 0;
            Supplies = BattleSupplies();
            ConsumablesUsed = 0;
            SelfResurrections = 0;
            Revives = 0;
            StepRevivedAlly = false;
            Combat = CombatTally();
            Rewards.ResetEpisode();
        }
    };

    struct EnvState
    {
        std::array<SeatState, MAX_SEATS> Seats;
        uint32 ActiveSeats = 1;                 // seats with a character this episode (the first ones)
        bool Fresh = false;                     // built by Setup, not yet reset
        bool BuildFailed = false;               // the last reset could not build the episode: end it and retry
        uint32 OpponentEntry = 0;               // creature entry: the duel's opponent, the first pull's first member
    };
}

#endif
