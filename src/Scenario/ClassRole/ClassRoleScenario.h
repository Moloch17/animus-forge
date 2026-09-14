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

#ifndef MOD_ANIMUS_FORGE_CLASS_ROLE_SCENARIO_H
#define MOD_ANIMUS_FORGE_CLASS_ROLE_SCENARIO_H

#include "ActionCatalog.h"
#include "ClassKit.h"
#include "ClassRoleProfile.h"
#include "GearBuilder.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Scenario.h"
#include "TalentBuilder.h"
#include <array>
#include <memory>

class Creature;
class Item;
class Player;
class SpellInfo;
class Unit;
class WorldSession;

namespace AnimusForge
{
    /// One class in one role on a training dummy, maximising damage, for every level, race and spec.
    ///
    /// Every episode builds a new bot: a race the class allows, a random gender and level (1-80;
    /// 55-80 for death knights), one of the role's specs with a random talent build that fills the
    /// spec's tree to its capstone first, the trainer spells of that level, and random level-appropriate
    /// gear for the spec including trinkets. The dummy is summoned at the bot's level, at melee or
    /// casting range for the spec, and its health follows a random fight curve (it takes no damage) so
    /// execute-range abilities come up.
    ///
    /// The action and observation layouts are fixed per class (see ActionCatalog and Observe), so one
    /// model covers every race, level, spec and build of the class/role.
    class ClassRoleScenario final : public Scenario
    {
    public:
        enum Obs : uint32
        {
            OBS_LEVEL                   = 0,    // level / 80
            OBS_RACE_FIRST              = 1,    // one-hot over PLAYABLE_RACES
            OBS_SPEC_FIRST              = 11,   // one-hot over the role's specs (3 slots)
            OBS_HEALTH                  = 14,
            OBS_MANA                    = 15,   // fraction of max; 0 without mana
            OBS_RAGE                    = 16,   // / 100
            OBS_ENERGY                  = 17,   // fraction of max
            OBS_RUNIC_POWER             = 18,   // / 100
            OBS_RUNE_FIRST              = 19,   // 6 runes: 1 ready, else 1 - cooldown / 10 s
            OBS_COMBO_POINTS            = 25,   // on the dummy, / 5
            OBS_FORM_FIRST              = 26,   // one-hot over TRACKED_FORMS
            OBS_GCD                     = 39,   // remaining / 1.5 s
            OBS_CASTING                 = 40,   // casting or channeling
            OBS_QUEUED_NEXT_SWING       = 41,
            OBS_MAIN_HAND_SWING         = 42,   // swing timer remaining / weapon speed
            OBS_OFF_HAND_SWING          = 43,
            OBS_RANGED_SWING            = 44,
            OBS_MAIN_HAND_SPEED         = 45,   // seconds / 4
            OBS_TARGET_HEALTH           = 46,
            OBS_TARGET_DISTANCE         = 47,   // yards / 40
            OBS_IN_MELEE_FRONT          = 48,
            OBS_ATTACK_POWER            = 49,   // / (100 + 50 * level)
            OBS_SPELL_POWER             = 50,   // / (50 + 30 * level)
            OBS_MELEE_CRIT              = 51,   // percent / 100
            OBS_SPELL_CRIT              = 52,
            OBS_MELEE_HASTE             = 53,   // rating bonus percent / 100
            OBS_SPELL_HASTE             = 54,
            OBS_MELEE_HIT               = 55,
            OBS_SPELL_HIT               = 56,
            OBS_EXPERTISE               = 57,   // / 30
            OBS_ARMOR_PENETRATION       = 58,   // rating bonus percent / 100
            OBS_LAST_STEP_DAMAGE        = 59,   // damage since the last decision / damage scale
            OBS_LAST_STEP_POWER_DELTA   = 60,   // primary power change since the last decision, as a fraction
            OBS_GLOBAL_COUNT            = 61

            // Then, per action: ACTION_FEATURES features (known, cooldown, aura on target, aura on self,
            // stacks). Then per talent of the class: rank / max rank. Then per tree: points / 71.
        };

        static constexpr uint32 ACTION_FEATURES = 5;
        static constexpr uint32 MAX_SPECS = 3;

        enum EpisodeInfoColumn : uint32
        {
            INFO_DAMAGE                 = 0,
            INFO_DPS                    = 1,
            INFO_WHITE_DAMAGE           = 2,
            INFO_SPECIAL_DAMAGE         = 3,
            INFO_LEVEL                  = 4,
            INFO_RACE                   = 5,
            INFO_SPEC                   = 6,
            INFO_UNSPENT_TALENT_POINTS  = 7,
            INFO_EQUIPPED_ITEMS         = 8,
            INFO_SPELL_CASTS            = 9,
            INFO_TRINKET_USES           = 10,
            INFO_COUNT
        };

        ClassRoleScenario(ClassRoleProfile const& profile, ForgeConfig const& config);
        ~ClassRoleScenario() override;

        [[nodiscard]] char const* Name() const override { return _profile.ScenarioName.c_str(); }
        [[nodiscard]] ScenarioSpec Spec() const override { return _spec; }

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
            std::array<WorldSession*, 2> Sessions{};    // alternate so the old bot outlives the new one's placement
            std::array<ObjectGuid::LowType, 2> Guids{}; // one GUID per session slot, reused (see BotSpec::GuidLow)
            uint8 ActiveSession = 0;
            bool Fresh = false;                         // built by Setup, not yet reset

            uint8 Race = 0;
            uint8 Level = 1;
            uint8 Spec = 0;
            TalentBuilder::Build Build;
            uint32 UnspentTalentPoints = 0;
            uint32 EquippedItems = 0;

            float DamageScale = 1.0f;
            float StartHealth = 1.0f;                   // dummy health fraction at episode start
            float EndHealth = 0.0f;                     // ... and at the episode time limit

            uint32 LastPower = 0;
            float LastStepDamage = 0.0f;
            float LastStepPowerDelta = 0.0f;
            uint32 SpellCasts = 0;
            uint32 TrinketUses = 0;
        };

        /// Replace the env's bot and dummy with a newly rolled character.
        bool Rebuild(Env& env);
        void Configure(Player* bot, EnvData& data) const;
        void StartFight(Player* bot, Creature* dummy, EnvData const& data) const;

        [[nodiscard]] SpellInfo const* ResolveSpell(Player const* bot, uint32 action) const;
        [[nodiscard]] static SpellInfo const* TrinketSpell(Item const* item);
        [[nodiscard]] bool IsActionAllowed(Player* bot, Creature* dummy, uint32 action) const;
        void UpdateDummyHealth(Env const& env, Creature* dummy) const;

        ClassRoleProfile const& _profile;
        uint32 _arenaMapId;
        Position _arenaPosition;
        std::vector<uint8> _races;

        std::unique_ptr<ClassKit> _kit;
        std::unique_ptr<TalentBuilder> _talents;
        std::unique_ptr<ActionCatalog> _catalog;
        std::unique_ptr<GearBuilder> _gear;

        ScenarioSpec _spec;
        uint32 _actionObsFirst = OBS_GLOBAL_COUNT;
        uint32 _talentObsFirst = 0;
        uint32 _treeObsFirst = 0;
        std::vector<EnvData> _data;
    };
}

#endif
