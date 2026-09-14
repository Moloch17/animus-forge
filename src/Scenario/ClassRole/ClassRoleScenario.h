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
class Map;
class Player;
class SpellInfo;
class Unit;
class WorldSession;

namespace AnimusForge
{
    /// What a class/role scenario fights.
    enum class ArenaMode : uint8
    {
        /// Stage 1 (`<class>_<role>`): a training dummy in range that takes no damage; maximise damage.
        Dummy,
        /// Stage 2 (`<class>_<role>_duel`): a same-level hostile creature spawned out of aggro range that
        /// fights back; move to it and kill it quickly while taking little damage. Observations and
        /// actions are stage 1's with duel features and actions appended, so a stage 1 model can seed it.
        Duel,
        /// Stage 3 (`<class>_<role>_pack`): a pack of 2-4 same-level creatures (casters included), often
        /// linked; pick targets, interrupt, crowd-control and clear it fast with little damage taken.
        /// Appends enemy slots, target selection and tactical spells to stage 2's layout.
        Pack,
        /// Stage 4 (`<class>_<role>_gauntlet`): pull after pull (1-4 creatures, sometimes elite or higher
        /// level) with a short break between, until death or the episode ends; recover between pulls with
        /// heals, food and drink. Appends sustain spells, consumables and pull state to stage 3's layout.
        Gauntlet,
    };

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

        /// Duel observation features, after all of stage 1's.
        enum DuelObs : uint32
        {
            DUEL_OBS_DISTANCE           = 0,    // yards / 60
            DUEL_OBS_BEARING_SIN        = 1,    // direction to the opponent relative to the bot's facing
            DUEL_OBS_BEARING_COS        = 2,
            DUEL_OBS_BEHIND_TARGET      = 3,    // the bot is in the opponent's back arc
            DUEL_OBS_TARGET_FACING_BOT  = 4,
            DUEL_OBS_TARGET_IN_COMBAT   = 5,
            DUEL_OBS_TARGET_ATTACKS_BOT = 6,
            DUEL_OBS_TARGET_CASTING     = 7,
            DUEL_OBS_BOT_MOVING         = 8,
            DUEL_OBS_BOT_IN_COMBAT      = 9,
            DUEL_OBS_BOT_STEALTHED      = 10,
            DUEL_OBS_BOT_AUTO_ATTACKING = 11,
            DUEL_OBS_DAMAGE_TAKEN       = 12,   // since the last decision / bot max health
            DUEL_OBS_PET_OUT            = 13,
            DUEL_OBS_PET_HEALTH         = 14,
            DUEL_OBS_PET_ATTACKING      = 15,   // the pet's victim is the opponent
            DUEL_OBS_EPISODE_TIME       = 16,   // elapsed / episode length
            DUEL_OBS_STABLE_FIRST       = 17,   // hunters' stable: per slot STABLE_FEATURES
            DUEL_OBS_COUNT_WITHOUT_STABLE = 17
        };

        /// Hunters' stable: beasts per episode, and features per beast (offered, family / 50,
        /// ferocity, tenacity, cunning).
        static constexpr uint32 STABLE_SLOTS = 4;
        static constexpr uint32 STABLE_FEATURES = 5;

        /// Duel actions, after all of stage 1's.
        enum DuelAction : uint32
        {
            DUEL_ACTION_MOVE_TO_TARGET  = 0,    // run to melee reach, on the side the bot is on
            DUEL_ACTION_MOVE_BEHIND     = 1,    // run to melee reach behind the opponent
            DUEL_ACTION_MOVE_TO_RANGE   = 2,    // run to casting range (25 yd)
            DUEL_ACTION_BACK_OFF        = 3,    // run 10 yd further away
            DUEL_ACTION_STOP            = 4,
            DUEL_ACTION_START_ATTACK    = 5,    // start auto-attack on the opponent
            DUEL_ACTION_PET_ATTACK      = 6,    // send pets and guardians at the opponent
            DUEL_ACTION_CALL_BEAST_FIRST = 7,   // hunters: call stable slot 0..STABLE_SLOTS-1
            DUEL_ACTION_COUNT_WITHOUT_STABLE = 7
        };

        enum DuelInfoColumn : uint32
        {
            DUEL_INFO_KILLED            = 0,
            DUEL_INFO_DIED              = 1,
            DUEL_INFO_TIME_TO_KILL      = 2,    // seconds; the episode length when not killed
            DUEL_INFO_DAMAGE_TAKEN      = 3,
            DUEL_INFO_HEALTH_LEFT       = 4,    // fraction at the end
            DUEL_INFO_STEALTH_OPENERS   = 5,
            DUEL_INFO_PET_SUMMONED      = 6,
            DUEL_INFO_OPPONENT          = 7,    // creature entry
            DUEL_INFO_COUNT
        };

        /// Pack enemies observed, and features per enemy slot.
        static constexpr uint32 PACK_SLOTS = 4;

        enum PackSlotFeature : uint32
        {
            SLOT_PRESENT                = 0,
            SLOT_ALIVE                  = 1,
            SLOT_HEALTH                 = 2,
            SLOT_DISTANCE               = 3,    // yards / 60
            SLOT_BEARING_SIN            = 4,
            SLOT_BEARING_COS            = 5,
            SLOT_BEHIND                 = 6,    // the bot is in its back arc
            SLOT_ATTACKS_BOT            = 7,
            SLOT_ATTACKS_PET            = 8,
            SLOT_CASTING                = 9,
            SLOT_IN_COMBAT              = 10,
            SLOT_CROWD_CONTROLLED       = 11,   // stunned, feared, confused, rooted, silenced or polymorphed
            SLOT_CURRENT_TARGET         = 12,
            SLOT_ELITE                  = 13,
            SLOT_LEVEL_DIFFERENCE       = 14,   // (its level - the bot's) / 5
            SLOT_FEATURES
        };

        /// Pack observation features, after all of stage 2's: PACK_OBS_GLOBAL_COUNT globals, the enemy slots,
        /// then per tactical action: known, cooldown.
        enum PackObs : uint32
        {
            PACK_OBS_ALIVE              = 0,    // living enemies / PACK_SLOTS
            PACK_OBS_IN_COMBAT          = 1,    // enemies in combat / PACK_SLOTS
            PACK_OBS_GLOBAL_COUNT       = 2
        };

        /// Pack actions, after all of stage 2's: target slot 0..PACK_SLOTS-1, then the tactical spells.
        static constexpr uint32 PACK_ACTION_TARGET_FIRST = 0;

        enum PackInfoColumn : uint32
        {
            PACK_INFO_KILLS             = 0,
            PACK_INFO_INTERRUPTS        = 1,
            PACK_INFO_PACK_SIZE         = 2,    // creatures in the first pull
            PACK_INFO_LINKED            = 3,
            PACK_INFO_COUNT
        };

        /// Gauntlet observation features, after all of stage 3's, then per sustain action: known, cooldown.
        enum GauntletObs : uint32
        {
            GAUNTLET_OBS_PULLS_CLEARED  = 0,    // / 10
            GAUNTLET_OBS_PULL_ACTIVE    = 1,
            GAUNTLET_OBS_NEXT_PULL      = 2,    // time until the next pull / 20 s
            GAUNTLET_OBS_PULL_TIME      = 3,    // time into the current pull / 60 s
            GAUNTLET_OBS_ELITE_PULL     = 4,
            GAUNTLET_OBS_EATING         = 5,
            GAUNTLET_OBS_DRINKING       = 6,
            GAUNTLET_OBS_FOOD_LEFT      = 7,    // / CONSUMABLE_COUNT
            GAUNTLET_OBS_DRINK_LEFT     = 8,
            GAUNTLET_OBS_GLOBAL_COUNT   = 9
        };

        /// Gauntlet actions, after all of stage 3's: eat, drink, then the sustain spells.
        enum GauntletAction : uint32
        {
            GAUNTLET_ACTION_EAT         = 0,
            GAUNTLET_ACTION_DRINK       = 1,
            GAUNTLET_ACTION_SUSTAIN_FIRST = 2
        };

        enum GauntletInfoColumn : uint32
        {
            GAUNTLET_INFO_PULLS_CLEARED = 0,
            GAUNTLET_INFO_FOOD_USED     = 1,
            GAUNTLET_INFO_DRINK_USED    = 2,
            GAUNTLET_INFO_SUSTAIN_CASTS = 3,
            GAUNTLET_INFO_COUNT
        };

        static constexpr uint32 CONSUMABLE_COUNT = 5;

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

        ClassRoleScenario(ClassRoleProfile const& profile, ForgeConfig const& config, ArenaMode mode);
        ~ClassRoleScenario() override;

        /// `<class>_<role>` for the dummy, `<class>_<role>_duel` for the duel.
        [[nodiscard]] static std::string ScenarioName(ClassRoleProfile const& profile, ArenaMode mode);

        [[nodiscard]] char const* Name() const override { return _name.c_str(); }
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
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

            // Duel only.
            uint32 OpponentEntry = 0;
            std::vector<uint32> Stable;                 // hunters: beasts offered this episode
            float LastDistance = -1.0f;                 // < 0 until the first reward
            uint32 KillTimeMs = 0;
            uint64 DamageTaken = 0;
            float LastStepDamageTaken = 0.0f;
            uint32 StealthOpeners = 0;
            bool StepStealthOpener = false;
            bool PetSummoned = false;
            bool Killed = false;                        // pack: cleared
            bool Died = false;

            // Pack and gauntlet.
            uint32 TargetSlot = 0;
            bool PackLinked = false;
            uint32 PackSize = 0;
            uint32 PullKills = 0;                       // dead enemies of the current pull
            uint32 Kills = 0;
            uint32 Interrupts = 0;
            ObjectGuid PendingInterrupt;                // a casting enemy the bot just cast an interrupt at
            uint32 PullStartMs = 0;
            uint64 PullDamageTaken = 0;

            // Gauntlet.
            uint32 PullsCleared = 0;
            uint32 NextPullMs = 0;                      // spawn the next pull at this episode time
            bool EliteOrHigherPull = false;
            uint32 FoodItem = 0;
            uint32 DrinkItem = 0;
            uint32 FoodUsed = 0;
            uint32 DrinkUsed = 0;
            uint32 SustainCasts = 0;
        };

        /// Replace the env's bot and dummy with a newly rolled character.
        bool Rebuild(Env& env);
        void Configure(Player* bot, EnvData& data) const;
        void StartFight(Player* bot, Creature* dummy, EnvData const& data) const;

        [[nodiscard]] SpellInfo const* ResolveSpell(Player const* bot, uint32 action) const;
        [[nodiscard]] static SpellInfo const* TrinketSpell(Item const* item);
        [[nodiscard]] bool IsActionAllowed(Player* bot, Creature* dummy, uint32 action) const;
        [[nodiscard]] bool IsSpellActionAllowed(Player* bot, Unit* target, ActionCatalog::Action const& def) const;

        /// Casts a spell action at `target` (may be null: self-cast spells only). Returns true if it started.
        bool ApplySpellAction(Player* bot, Unit* target, ActionCatalog::Action const& def, EnvData& data) const;
        void UpdateDummyHealth(Env const& env, Creature* dummy) const;

        /// The creature the bot's actions aim at: the dummy or opponent, or the selected pack enemy
        /// (the nearest living one when the selection is dead). Null between gauntlet pulls.
        [[nodiscard]] Creature* CurrentTarget(Env const& env);

        [[nodiscard]] bool HasDuel() const { return _mode >= ArenaMode::Duel; }
        [[nodiscard]] bool HasPack() const { return _mode >= ArenaMode::Pack; }
        [[nodiscard]] bool HasGauntlet() const { return _mode >= ArenaMode::Gauntlet; }

        // Pack and gauntlet stages (ClassRolePack.cpp).
        bool StartPack(Env& env, Player* bot, Map* map, EnvData& data) const;
        bool SpawnPull(Env& env, Player* bot, Map* map, EnvData& data) const;
        void UpdatePack(Env& env, Player* bot, EnvData& data) const;
        void ObservePack(Env const& env, Player* bot, float* obs) const;
        void ObserveGauntlet(Env const& env, Player* bot, float* obs) const;
        [[nodiscard]] bool IsPackActionAllowed(Env const& env, Player* bot, uint32 packAction) const;
        void ApplyPackAction(Env& env, Player* bot, Unit* target, uint32 packAction, EnvData& data) const;
        [[nodiscard]] bool IsGauntletActionAllowed(Player* bot, Unit* target, uint32 gauntletAction,
            EnvData const& data) const;
        void ApplyGauntletAction(Player* bot, Unit* target, uint32 gauntletAction, EnvData& data) const;
        [[nodiscard]] float PackReward(Env& env, Player* bot, EnvData& data) const;
        void PackEpisodeInfo(Env const& env, float* info) const;
        void GauntletEpisodeInfo(Env const& env, float* info) const;

        // Duel stage (ClassRoleDuel.cpp).
        void StartDuel(Player* bot, Creature* opponent, EnvData& data) const;
        [[nodiscard]] bool IsDuelActionAllowed(Player* bot, Creature* opponent, uint32 duelAction,
            EnvData const& data) const;
        void ApplyDuelAction(Player* bot, Creature* opponent, uint32 duelAction, EnvData& data) const;
        void ObserveDuel(Env const& env, Player* bot, Creature* opponent, float* obs) const;
        [[nodiscard]] float DuelReward(Env const& env, Player* bot, Creature* opponent, EnvData& data) const;
        void DuelEpisodeInfo(Env const& env, float* info) const;
        [[nodiscard]] float DesiredRange(EnvData const& data) const;

        ArenaMode _mode;
        std::string _name;
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
        uint32 _duelObsFirst = 0;           // duel: first duel observation (= stage 1's ObsDim)
        uint32 _duelActionFirst = 0;        // duel: first duel action (= stage 1's NumActions)
        uint32 _duelActionCount = 0;
        uint32 _duelInfoFirst = INFO_COUNT;
        uint32 _packObsFirst = 0;           // pack: first pack observation (= stage 2's ObsDim)
        uint32 _packActionFirst = 0;
        uint32 _packActionCount = 0;
        uint32 _packInfoFirst = 0;
        uint32 _gauntletObsFirst = 0;       // gauntlet: first gauntlet observation (= stage 3's ObsDim)
        uint32 _gauntletActionFirst = 0;
        uint32 _gauntletActionCount = 0;
        uint32 _gauntletInfoFirst = 0;
        std::vector<EnvData> _data;
    };
}

#endif
