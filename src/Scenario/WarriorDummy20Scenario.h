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

#ifndef MOD_ANIMUS_FORGE_WARRIOR_DUMMY_20_SCENARIO_H
#define MOD_ANIMUS_FORGE_WARRIOR_DUMMY_20_SCENARIO_H

#include "Position.h"
#include "Scenario.h"
#include <array>
#include <vector>

class Player;
class SpellInfo;
class Unit;

namespace AnimusForge
{
    /// A level 20 Arms warrior in level-appropriate gear on a training dummy, with a new random
    /// Arms talent build every episode.
    ///
    /// The kit is everything a warrior trainer teaches by level 20, and the action space is its
    /// damage-relevant subset. Each episode the talents are reset and one build is drawn uniformly
    /// from every valid allocation of the level's talent points within the Arms tree, so the policy
    /// trains across all of them; the build's ranks are part of the observation.
    ///
    /// Talent-granted active abilities are appended to the action space automatically (masked out
    /// when the drawn build lacks them). At level 20 the reachable Arms rows are all passive, so
    /// there are none yet; the same code picks up Sweeping Strikes and later abilities at higher
    /// levels.
    class WarriorDummy20Scenario final : public Scenario
    {
    public:
        /// Fixed actions. Talent actions follow, starting at ACTION_FIXED_COUNT.
        enum Action : int32
        {
            ACTION_NOOP                 = 0,
            ACTION_HEROIC_STRIKE        = 1,    // on next swing
            ACTION_CLEAVE               = 2,    // on next swing
            ACTION_CANCEL_QUEUED        = 3,    // cancel a queued Heroic Strike or Cleave
            ACTION_REND                 = 4,
            ACTION_THUNDER_CLAP         = 5,
            ACTION_BATTLE_SHOUT         = 6,
            ACTION_BLOODRAGE            = 7,
            ACTION_OVERPOWER            = 8,
            ACTION_HAMSTRING            = 9,
            ACTION_MOCKING_BLOW         = 10,
            ACTION_FIXED_COUNT
        };

        /// Fixed observation features. One feature per reachable Arms talent follows, starting at
        /// OBS_FIXED_COUNT, in the scenario's talent order: learned rank / max rank.
        enum Obs : uint32
        {
            OBS_RAGE                    = 0,    // rage / max rage
            OBS_SWING_REMAINING         = 1,    // main-hand swing timer remaining / weapon speed
            OBS_WEAPON_SPEED            = 2,    // weapon speed in seconds / 4
            OBS_HEROIC_STRIKE_QUEUED    = 3,
            OBS_CLEAVE_QUEUED           = 4,
            OBS_IN_MELEE_FRONT          = 5,    // target in melee range and in the frontal arc
            OBS_GCD_REMAINING           = 6,    // / 1.5 s
            OBS_THUNDER_CLAP_COOLDOWN   = 7,    // remaining / full cooldown
            OBS_BLOODRAGE_COOLDOWN      = 8,
            OBS_OVERPOWER_COOLDOWN      = 9,
            OBS_MOCKING_BLOW_COOLDOWN   = 10,
            OBS_OVERPOWER_WINDOW        = 11,   // 1 while the target's dodge has opened Overpower
            OBS_REND_REMAINING          = 12,   // bot's Rend on the target, remaining / duration
            OBS_THUNDER_CLAP_REMAINING  = 13,
            OBS_HAMSTRING_REMAINING     = 14,
            OBS_BATTLE_SHOUT_REMAINING  = 15,   // on the bot
            OBS_BLOODRAGE_REMAINING     = 16,   // rage-over-time aura on the bot
            OBS_LAST_STEP_DAMAGE        = 17,   // damage since the last decision / damage scale
            OBS_LAST_STEP_RAGE_DELTA    = 18,   // rage change since the last decision / max rage
            OBS_FIXED_COUNT
        };

        enum EpisodeInfoColumn : uint32
        {
            INFO_DAMAGE                 = 0,
            INFO_DPS                    = 1,
            INFO_WHITE_HITS             = 2,
            INFO_SPECIAL_HITS           = 3,
            INFO_WHITE_DAMAGE           = 4,
            INFO_SPECIAL_DAMAGE         = 5,
            INFO_TALENT_BUILD           = 6,    // index of the episode's build in the build list
            INFO_CASTS_FIRST            = 7     // then successful casts per spell action, in action order
        };

        explicit WarriorDummy20Scenario(ForgeConfig const& config);

        [[nodiscard]] char const* Name() const override { return "warrior_dummy_20"; }
        [[nodiscard]] ScenarioSpec Spec() const override;

        bool Setup(Env& env) override;
        void Reset(Env& env) override;
        void ApplyActions(Env& env, int32 const* actions) override;
        void Observe(Env& env, float* obs, float* state, uint8* mask) override;
        void Reward(Env& env, float* reward) override;
        void EpisodeInfo(Env const& env, float* info) const override;
        [[nodiscard]] std::vector<std::string> EpisodeInfoNames() const override;
        bool ScriptedAction(std::string const& policy, float const* obs, uint8 const* mask, uint16 layout,
            int32& action) const override;
        void Teardown(Env& env) override;

    private:
        /// One action that casts a spell. For talent abilities `TalentIndex` is set and the spell
        /// is the highest rank of that talent the bot currently has.
        struct SpellAction
        {
            std::string Name;
            uint32 SpellId = 0;
            bool NextSwing = false;
            int32 TalentIndex = -1;
        };

        /// A talent reachable with the level's points, in a valid learning order.
        struct TalentSlot
        {
            uint32 TalentId = 0;
            uint32 Row = 0;
            uint8 MaxRank = 0;
            int32 DependsOnSlot = -1;       // slot index of the prerequisite, if any
            uint8 DependsOnRank = 0;        // prerequisite rank needed (1-based)
            std::array<uint32, 5> RankSpells{};
        };

        struct EnvData
        {
            Position Home;
            float DamageScale = 1.0f;       // weapon max hit, fixed at setup
            uint32 LastRage = 0;
            float LastStepDamage = 0.0f;
            float LastStepRageDelta = 0.0f;
            uint32 Build = 0;               // index into _builds
            std::vector<uint32> Casts;      // successful casts per action this episode
        };

        void BuildTalentTable(uint32 talentPoints);
        void EnumerateBuilds(size_t slot, uint32 remaining, std::vector<uint8>& ranks);
        [[nodiscard]] bool IsValidBuild(std::vector<uint8> const& ranks) const;
        void ApplyTalentBuild(Player* bot, uint32 build) const;

        bool EquipGear(Player* bot) const;
        void LearnKit(Player* bot) const;

        [[nodiscard]] SpellInfo const* ResolveSpell(Player const* bot, int32 action) const;
        [[nodiscard]] static bool CanCast(Player* bot, SpellInfo const* info, Unit* target);
        [[nodiscard]] bool IsActionAllowed(Player* bot, Unit* target, int32 action) const;

        uint32 _spawnMapId;
        Position _spawnPoint;
        uint32 _hsRageThreshold;
        uint32 _talentPoints;
        ScenarioSpec _spec;

        std::vector<SpellAction> _actions;          // index = action id; fixed actions first
        std::vector<TalentSlot> _talents;
        std::vector<std::vector<uint8>> _builds;    // every valid full allocation, ranks per slot

        std::vector<EnvData> _data;
    };
}

#endif
