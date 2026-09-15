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

#ifndef MOD_ANIMUS_FORGE_CLASS_ROLE_COMBAT_REWARD_H
#define MOD_ANIMUS_FORGE_CLASS_ROLE_COMBAT_REWARD_H

#include "ClassRoleTuning.h"
#include "Define.h"

class Player;
class Unit;

namespace AnimusForge
{
    struct AgentStats;
    struct Env;
}

namespace AnimusForge::ClassRole
{
    class ClassRoleScenario;
    class RewardLedger;
    struct CombatTally;
    struct SeatState;

    /// Reward pieces every fight against something that fights back shares (duel, pulls, PvP).
    namespace CombatReward
    {
        /// The range the approach shaping aims for: the spec's melee or ranged range.
        [[nodiscard]] float DesiredRange(SeatState const& seat, ClassRoleTuning::DuelTuning const& duel);

        /// The step's cast counts into the tally, and the casting term: cast time wasted on casts cut short, and cast
        /// time of casts that finished in combat.
        void Casting(Player* bot, AgentStats const& step, CombatTally& tally,
            ClassRoleTuning::CastingTuning const& tuning, RewardLedger& ledger);

        /// Potential-based shaping on the distance still to close to `desiredRange` from `target`: it pays for getting
        /// there and takes it back for leaving, so it cannot be farmed. A null target restarts it.
        void Approach(Player* bot, Unit* target, float desiredRange, float weight, CombatTally& tally,
            RewardLedger& ledger);

        /// A seat's reward against one opponent (a creature or a player): damage dealt as a fraction of its health,
        /// damage taken, casting, approach, stealth openers, the kill (faster and healthier pays more), death.
        void OneOnOne(ClassRoleScenario& scenario, Env const& env, uint32 seat, Player* bot, Unit* opponent,
            RewardLedger& ledger);

        /// The fraction of the episode still left.
        [[nodiscard]] float TimeLeft(Env const& env);
    }
}

#endif
