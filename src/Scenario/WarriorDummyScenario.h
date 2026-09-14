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

#ifndef ANIMUS_WARRIOR_DUMMY_SCENARIO_H
#define ANIMUS_WARRIOR_DUMMY_SCENARIO_H

#include "Position.h"
#include "Scenario.h"
#include <vector>

class Creature;
class Map;
class Player;

namespace Animus
{
    /// One level 1 human warrior auto-attacking a training dummy. The only decision is when to
    /// spend rage on Heroic Strike (on next swing, no cooldown, no GCD) versus letting white swings
    /// build rage. Reward is damage dealt, so episode return is proportional to DPS.
    class WarriorDummyScenario final : public Scenario
    {
    public:
        enum Action : int32
        {
            ACTION_NOOP                 = 0,
            ACTION_QUEUE_HEROIC_STRIKE  = 1,
            ACTION_CANCEL_QUEUED        = 2,
            ACTION_COUNT
        };

        enum Obs : uint32
        {
            OBS_RAGE                    = 0,    // rage / max rage
            OBS_SWING_REMAINING         = 1,    // main-hand swing timer remaining / weapon speed
            OBS_WEAPON_SPEED            = 2,    // weapon speed in seconds / 4
            OBS_HEROIC_STRIKE_QUEUED    = 3,
            OBS_AUTO_ATTACKING          = 4,
            OBS_IN_MELEE_FRONT          = 5,    // target in melee range and in the frontal arc
            OBS_HEROIC_STRIKE_COST      = 6,    // cost / max rage
            OBS_LAST_STEP_DAMAGE        = 7,    // damage since the last decision / damage scale
            OBS_LAST_STEP_RAGE_DELTA    = 8,    // rage change since the last decision / max rage
            OBS_COUNT
        };

        enum EpisodeInfoColumn : uint32
        {
            INFO_DAMAGE                 = 0,
            INFO_DPS                    = 1,
            INFO_WHITE_HITS             = 2,
            INFO_SPECIAL_HITS           = 3,
            INFO_WHITE_DAMAGE           = 4,
            INFO_SPECIAL_DAMAGE         = 5,
            INFO_COUNT
        };

        explicit WarriorDummyScenario(ForgeConfig const& config);

        [[nodiscard]] char const* Name() const override { return "warrior_dummy"; }
        [[nodiscard]] ScenarioSpec Spec() const override;

        bool Setup(Env& env) override;
        void Reset(Env& env) override;
        void ApplyActions(Env& env, int32 const* actions) override;
        void Observe(Env& env, float* obs, float* state, uint8* mask) override;
        void Reward(Env& env, float* reward) override;
        void EpisodeInfo(Env const& env, float* info) const override;
        [[nodiscard]] std::vector<std::string> EpisodeInfoNames() const override;
        bool ScriptedAction(std::string const& policy, float const* obs, uint8 const* mask,
            int32& action) const override;
        void Teardown(Env& env) override;

    private:
        struct EnvData
        {
            Position Home;
            float DamageScale = 1.0f;       // weapon max hit, fixed at setup
            uint32 LastRage = 0;
            float LastStepDamage = 0.0f;
            float LastStepRageDelta = 0.0f;
        };

        void ClearArena(Player* bot) const;
        bool SpawnDummy(Env& env, Player* bot, Map* map);

        [[nodiscard]] static bool IsHeroicStrikeQueued(Player const* bot);
        [[nodiscard]] static uint32 HeroicStrikeCost(Player* bot);
        [[nodiscard]] static bool CanQueueHeroicStrike(Player* bot);

        uint32 _arenaMapId;
        Position _arenaPosition;
        uint32 _hsRageThreshold;
        uint32 _agentAccountBase;

        std::vector<EnvData> _data;
    };
}

#endif
