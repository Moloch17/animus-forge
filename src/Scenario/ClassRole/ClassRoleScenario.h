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
#include "ClassRoleAssets.h"
#include "ClassRoleProfile.h"
#include "CompanionOwner.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Scenario.h"
#include "TalentBuilder.h"
#include <array>
#include <memory>

class Creature;
class Group;
class Item;
class Map;
class Player;
class SpellInfo;
class Unit;
class WorldSession;

namespace AnimusForge
{
    struct AgentStats;

    /// A curriculum stage. Each stage is its own scenario (`class_role`, `class_role_duel`, ...), so every earlier
    /// stage stays repeatable, and each stage's observations and actions extend the previous stage's per layout,
    /// so a stage's model seeds the next.
    enum class ArenaMode : uint8
    {
        /// Stage 1 (`class_role`): a training dummy in range that takes no damage; maximise damage.
        Dummy,
        /// Stage 2 (`class_role_duel`): a same-level hostile creature spawned out of aggro range that fights back;
        /// move to it and kill it quickly while taking little damage.
        Duel,
        /// Stage 3 (`class_role_pack`): a pack of 2-4 same-level creatures (casters included), often linked; pick
        /// targets, interrupt, crowd-control and clear it fast with little damage taken.
        Pack,
        /// Stage 4 (`class_role_gauntlet`): pull after pull (1-4 creatures, sometimes elite or higher level) with a
        /// short break between, until death or the episode ends; recover with heals, food and drink.
        Gauntlet,
        /// Stage 5 (`class_role_companion`): the gauntlet beside a scripted owner (a player of a random class near
        /// the bot's level); follow, assist, guard and heal it.
        Companion,
        /// Stage 6 (`class_role_party`): four learned agents -- a tank, a healer and two damage dealers of random
        /// classes -- and the scripted owner as the fifth player, against elite-heavy pulls. Every agent sees and
        /// can assist, guard and heal the other three.
        Party,
        /// Stage 7 (`class_role_pvp`): one-on-one against a scripted enemy player of a random class and role.
        Pvp,
        /// Stage 8 (`class_role_arena`): self-play one-on-one: two learned agents of random classes and roles, in
        /// one env, played by the same policy.
        Arena,
    };

    /// Every class/role, every level, race, spec and build, at one curriculum stage, as layouts of one policy.
    ///
    /// Each learned agent of an env is a seat: every episode it becomes a new character of a class/role drawn from
    /// AnimusForge.ClassRoles (all 18 by default) -- a race the class allows, random gender, level (1-80, 55-80 for
    /// death knights), one of the role's specs with a random talent build that fills the spec's tree to its capstone
    /// first, the trainer spells of the level, and random level-appropriate gear including trinkets. A seat's
    /// layout is its class/role's: observation features and actions fixed per class/role (see ActionCatalog and
    /// ObserveSeat), padded to the largest layout's on the wire (see LayoutSpec). The learner shares one trunk
    /// between all layouts, with an input adapter and an action head per layout.
    ///
    /// The critic state is class-agnostic (BuildState): every seat's and enemy's essentials, the owner and the
    /// pull timing.
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
            DUEL_OBS_CAST_PROGRESS      = 17,   // fraction of the current cast time done; 0 when not casting
            DUEL_OBS_CAST_REMAINING     = 18,   // seconds left of the current cast / 3
            DUEL_OBS_SHAPESHIFTED       = 19,   // in a form the bot can cancel
            DUEL_OBS_STABLE_FIRST       = 20,   // hunters' stable: per slot STABLE_FEATURES
            DUEL_OBS_COUNT_WITHOUT_STABLE = 20
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
            DUEL_ACTION_STOP_CASTING    = 7,    // cancel the current cast or channel
            DUEL_ACTION_CANCEL_FORM     = 8,    // leave the current shapeshift form, as right-clicking it does
            DUEL_ACTION_CALL_BEAST_FIRST = 9,   // hunters: call stable slot 0..STABLE_SLOTS-1
            DUEL_ACTION_COUNT_WITHOUT_STABLE = 9
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
            DUEL_INFO_CASTS_COMPLETED   = 8,    // cast-time spells that finished casting
            DUEL_INFO_CASTS_CANCELLED   = 9,    // ... that were cut short
            DUEL_INFO_CAST_TIME_WASTED  = 10,   // seconds spent on the cut-short casts
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

        /// Companion observation features, after all of stage 4's, then per owner-heal action: known,
        /// cooldown.
        enum CompanionObs : uint32
        {
            COMPANION_OBS_OWNER_PRESENT     = 0,
            COMPANION_OBS_OWNER_ALIVE       = 1,
            COMPANION_OBS_OWNER_HEALTH      = 2,
            COMPANION_OBS_OWNER_MANA        = 3,    // 0 without mana
            COMPANION_OBS_OWNER_DISTANCE    = 4,    // yards / 40
            COMPANION_OBS_OWNER_BEARING_SIN = 5,
            COMPANION_OBS_OWNER_BEARING_COS = 6,
            COMPANION_OBS_OWNER_IN_COMBAT   = 7,
            COMPANION_OBS_OWNER_MOVING      = 8,
            COMPANION_OBS_OWNER_LEVEL_DIFF  = 9,    // (owner level - bot level) / 5
            COMPANION_OBS_OWNER_CLASS_FIRST = 10,   // one-hot over OWNER_CLASSES (10)
            COMPANION_OBS_OWNER_ATTACKERS   = 20,   // enemies attacking the owner / PACK_SLOTS
            COMPANION_OBS_OWNER_TARGET_FIRST = 21,  // one-hot: which enemy slot the owner attacks
            COMPANION_OBS_OWNER_NO_TARGET   = 25,
            COMPANION_OBS_SLOT_ON_OWNER_FIRST = 26, // per enemy slot: attacking the owner
            COMPANION_OBS_GLOBAL_COUNT      = 30
        };

        /// Companion actions, after all of stage 4's: follow, assist, guard, then one "cast on the owner"
        /// action per single-target heal.
        enum CompanionAction : uint32
        {
            COMPANION_ACTION_FOLLOW         = 0,    // run to just behind the owner
            COMPANION_ACTION_ASSIST         = 1,    // target the owner's target
            COMPANION_ACTION_GUARD          = 2,    // target an enemy attacking the owner
            COMPANION_ACTION_HEAL_FIRST     = 3
        };

        enum CompanionInfoColumn : uint32
        {
            COMPANION_INFO_OWNER_CLASS      = 0,
            COMPANION_INFO_OWNER_DIED       = 1,
            COMPANION_INFO_OWNER_DAMAGE_TAKEN = 2,
            COMPANION_INFO_OWNER_HEALING    = 3,    // effective healing the bot did on the owner
            COMPANION_INFO_THREAT_ON_BOT    = 4,    // enemy-decisions spent attacking the bot
            COMPANION_INFO_THREAT_ON_OWNER  = 5,    // ... attacking the owner
            COMPANION_INFO_COUNT
        };

        /// A party seat's teammates: the other three seats (the owner has the companion block).
        static constexpr uint32 PARTY_MEMBERS = 3;

        /// Party features per member slot.
        enum PartyMemberFeature : uint32
        {
            MEMBER_PRESENT              = 0,
            MEMBER_ALIVE                = 1,
            MEMBER_HEALTH               = 2,
            MEMBER_MANA                 = 3,
            MEMBER_DISTANCE             = 4,    // yards / 40
            MEMBER_BEARING_SIN          = 5,
            MEMBER_BEARING_COS          = 6,
            MEMBER_IN_COMBAT            = 7,
            MEMBER_ROLE_FIRST           = 8,    // one-hot: damage, tank, healer
            MEMBER_CLASS_FIRST          = 11,   // one-hot over the 10 classes
            MEMBER_ATTACKERS            = 21,   // enemies attacking it / PACK_SLOTS
            MEMBER_TARGET_FIRST         = 22,   // one-hot: which enemy slot it attacks
            MEMBER_NO_TARGET            = 26,
            MEMBER_SLOT_ON_FIRST        = 27,   // per enemy slot: attacking it
            MEMBER_FEATURES             = 31
        };

        /// Party observation features, after all of stage 5's: globals, then PARTY_MEMBERS teammate slots.
        enum PartyObs : uint32
        {
            PARTY_OBS_ALIVE             = 0,    // living party players (bot and owner included) / 5
            PARTY_OBS_LOWEST_HEALTH     = 1,    // the most hurt living ally's health (owner and teammates)
            PARTY_OBS_HAS_TANK          = 2,    // a living tank other than the bot
            PARTY_OBS_HAS_HEALER        = 3,    // a living healer other than the bot
            PARTY_OBS_GLOBAL_COUNT      = 4
        };

        /// Party actions, after all of stage 5's: follow the tank, then per teammate assist and guard, then per
        /// teammate one "cast on it" action per ally heal.
        enum PartyAction : uint32
        {
            PARTY_ACTION_FOLLOW_TANK    = 0,
            PARTY_ACTION_ASSIST_FIRST   = 1,    // + member
            PARTY_ACTION_GUARD_FIRST    = 1 + PARTY_MEMBERS,
            PARTY_ACTION_HEAL_FIRST     = 1 + 2 * PARTY_MEMBERS  // + teammate * ally heals + heal
        };

        enum PartyInfoColumn : uint32
        {
            PARTY_INFO_TEAMMATES_DIED   = 0,
            PARTY_INFO_TEAMMATE_DAMAGE_TAKEN = 1,
            PARTY_INFO_TEAMMATE_HEALING = 2,    // effective healing the seat did on its teammates
            PARTY_INFO_THREAT_ON_TEAMMATES = 3, // enemy-decisions spent attacking non-tank teammates
            PARTY_INFO_SEAT             = 4,
            PARTY_INFO_COUNT
        };

        /// PvP observation features (stages 7 and 8), after all of stage 6's.
        enum PvpObs : uint32
        {
            PVP_OBS_OPPONENT_CLASS_FIRST = 0,   // one-hot over the 10 classes
            PVP_OBS_OPPONENT_ROLE_FIRST = 10,   // one-hot: damage, tank, healer
            PVP_OBS_OPPONENT_LEVEL_DIFF = 13,   // (its level - the bot's) / 5
            PVP_OBS_OPPONENT_MANA       = 14,
            PVP_OBS_OPPONENT_RAGE_ENERGY = 15,  // rage, energy or runic power as a fraction
            PVP_OBS_OPPONENT_CONTROLLED = 16,   // stunned, feared, confused, rooted, silenced or polymorphed
            PVP_OBS_OPPONENT_STEALTHED  = 17,
            PVP_OBS_OPPONENT_PET_OUT    = 18,
            PVP_OBS_OPPONENT_HEALING    = 19,   // casting a heal
            PVP_OBS_BOT_STUNNED         = 20,   // stunned, feared or confused: no actions land
            PVP_OBS_BOT_ROOTED          = 21,
            PVP_OBS_BOT_SILENCED        = 22,
            PVP_OBS_MIRROR              = 23,   // stage 8: the opponent is a learned agent too
            PVP_OBS_COUNT               = 24
        };

        enum PvpInfoColumn : uint32
        {
            PVP_INFO_WON                = 0,
            PVP_INFO_OPPONENT_CLASS     = 1,
            PVP_INFO_OPPONENT_ROLE      = 2,    // 0 damage, 1 tank, 2 healer
            PVP_INFO_COUNT
        };

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
            INFO_CLASS                  = 11,
            INFO_ROLE                   = 12,   // 0 damage, 1 tank, 2 healer
            INFO_COUNT
        };

        /// Learned agents per env: 1, an arena's 2 or a party's 4.
        static constexpr uint32 MAX_SEATS = 4;

        /// Class-agnostic critic state (BuildState), per seat and per enemy slot.
        enum StateGlobal : uint32
        {
            STATE_EPISODE_TIME          = 0,
            STATE_PULL_ACTIVE           = 1,
            STATE_PULLS_CLEARED         = 2,    // / 10
            STATE_NEXT_PULL             = 3,    // time until the next pull / 20 s
            STATE_ELITE_PULL            = 4,
            STATE_LINKED_PULL           = 5,
            STATE_OWNER_PRESENT         = 6,
            STATE_OWNER_ALIVE           = 7,
            STATE_OWNER_HEALTH          = 8,
            STATE_OWNER_MANA            = 9,
            STATE_OWNER_X               = 10,   // relative to the arena, / 40
            STATE_OWNER_Y               = 11,
            STATE_OWNER_IN_COMBAT       = 12,
            STATE_GLOBAL_COUNT          = 13
        };

        enum StateSeat : uint32
        {
            STATE_SEAT_PRESENT          = 0,
            STATE_SEAT_ALIVE            = 1,
            STATE_SEAT_HEALTH           = 2,
            STATE_SEAT_MANA             = 3,
            STATE_SEAT_OTHER_POWER      = 4,    // rage, energy or runic power as a fraction
            STATE_SEAT_LEVEL            = 5,    // / 80
            STATE_SEAT_ROLE_FIRST       = 6,    // one-hot: damage, tank, healer
            STATE_SEAT_CLASS_FIRST      = 9,    // one-hot over the 10 classes
            STATE_SEAT_IN_COMBAT        = 19,
            STATE_SEAT_CASTING          = 20,
            STATE_SEAT_X                = 21,   // relative to the arena, / 40
            STATE_SEAT_Y                = 22,
            STATE_SEAT_FEATURES         = 23
        };

        enum StateEnemy : uint32
        {
            STATE_ENEMY_PRESENT         = 0,
            STATE_ENEMY_ALIVE           = 1,
            STATE_ENEMY_HEALTH          = 2,
            STATE_ENEMY_X               = 3,
            STATE_ENEMY_Y               = 4,
            STATE_ENEMY_CASTING         = 5,
            STATE_ENEMY_ELITE           = 6,
            STATE_ENEMY_LEVEL_DIFF      = 7,    // (its level - seat 0's) / 5
            STATE_ENEMY_IN_COMBAT       = 8,
            STATE_ENEMY_ON_OWNER        = 9,    // its victim is the owner
            STATE_ENEMY_ON_SEAT_FIRST   = 10,   // one-hot: its victim is seat s (MAX_SEATS columns)
            STATE_ENEMY_FEATURES        = 10 + MAX_SEATS
        };

        ClassRoleScenario(ForgeConfig const& config, ArenaMode mode);
        ~ClassRoleScenario() override;

        /// `class_role` for the dummy, `class_role_duel` for the duel, ...
        [[nodiscard]] static std::string ScenarioName(ArenaMode mode);

        [[nodiscard]] char const* Name() const override { return _name.c_str(); }
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        [[nodiscard]] ScenarioSpec Spec() const override { return _spec; }

        bool Setup(Env& env) override;
        void Reset(Env& env) override;
        void ApplyActions(Env& env, int32 const* actions) override;
        void Observe(Env& env, float* obs, float* state, uint8* mask) override;
        void AgentLayouts(Env const& env, uint16* layout) const override;
        void Reward(Env& env, float* reward) override;
        void EpisodeInfo(Env const& env, float* info) const override;
        [[nodiscard]] std::vector<std::string> EpisodeInfoNames() const override;
        bool ScriptedAction(std::string const& policy, float const* obs, uint8 const* mask, uint16 layout,
            int32& action) const override;
        void Teardown(Env& env) override;

    private:
        /// One class/role's observation and action layout at this stage: offsets of each stage's block.
        struct Layout
        {
            uint16 Index = 0;
            ClassRoleProfile const* Profile = nullptr;
            ClassRoleAssets const* Assets = nullptr;
            uint32 ObsDim = 0;
            uint32 NumActions = 0;
            uint32 ActionObsFirst = OBS_GLOBAL_COUNT;
            uint32 TalentObsFirst = 0;
            uint32 TreeObsFirst = 0;
            uint32 DuelObsFirst = 0;
            uint32 DuelActionFirst = 0;
            uint32 DuelActionCount = 0;
            uint32 PackObsFirst = 0;
            uint32 PackActionFirst = 0;
            uint32 PackActionCount = 0;
            uint32 GauntletObsFirst = 0;
            uint32 GauntletActionFirst = 0;
            uint32 GauntletActionCount = 0;
            uint32 CompanionObsFirst = 0;
            uint32 CompanionActionFirst = 0;
            uint32 CompanionActionCount = 0;
            uint32 PartyObsFirst = 0;
            uint32 PartyActionFirst = 0;
            uint32 PartyActionCount = 0;
            uint32 PvpObsFirst = 0;
            std::vector<ActionCatalog::Action> AllyHeals;   // single-target heals that can be cast on an ally

            [[nodiscard]] ActionCatalog const& Catalog() const { return *Assets->Catalog; }
            [[nodiscard]] Role PlayRole() const { return Profile->PlayRole; }
        };

        /// One learned agent's character and episode totals.
        struct Seat
        {
            Layout const* L = nullptr;
            std::array<WorldSession*, 2> Sessions{};    // alternate so the old bot outlives the new one's placement
            std::array<ObjectGuid::LowType, 2> Guids{}; // one GUID per session slot, reused (see BotSpec::GuidLow)
            uint8 ActiveSession = 0;

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

            // Duel on.
            std::vector<uint32> Stable;                 // hunters: beasts offered this episode
            float LastDistance = -1.0f;                 // < 0 until the first reward
            uint32 KillTimeMs = 0;
            uint64 DamageTaken = 0;
            float LastStepDamageTaken = 0.0f;
            uint32 StealthOpeners = 0;
            bool StepStealthOpener = false;
            bool PetSummoned = false;
            uint32 CastsCompleted = 0;
            uint32 CastsCancelled = 0;
            uint64 CastMsWasted = 0;
            bool Killed = false;                        // its opponent died (pack: the pull was cleared)
            bool Died = false;

            // Pack on.
            uint32 TargetSlot = 0;
            uint32 Interrupts = 0;
            ObjectGuid PendingInterrupt;                // a casting enemy the bot just cast an interrupt at
            uint64 PullDamageTaken = 0;

            // Gauntlet on.
            uint32 FoodItem = 0;
            uint32 DrinkItem = 0;
            uint32 FoodUsed = 0;
            uint32 DrinkUsed = 0;
            uint32 SustainCasts = 0;

            // Companion on.
            uint64 OwnerHealing = 0;
            uint64 ThreatOnBot = 0;
            bool OwnerDeathSeen = false;

            // Party.
            uint64 TeammateDamageTaken = 0;
            uint64 TeammateHealing = 0;
            uint64 ThreatOnTeammates = 0;
            uint32 TeammatesDied = 0;
            std::array<bool, MAX_SEATS> TeammateDeathSeen{};

            /// Clear the episode totals (not the character).
            void ResetEpisode();
        };

        struct EnvData
        {
            std::array<Seat, MAX_SEATS> Seats;
            bool Fresh = false;                         // built by Setup, not yet reset

            // Duel and pack on: the current pull.
            uint32 OpponentEntry = 0;
            bool PackLinked = false;
            uint32 PackSize = 0;
            uint32 PullKills = 0;                       // dead enemies of the current pull
            uint32 Kills = 0;
            uint32 PullStartMs = 0;
            bool PullCleared = false;                   // decided once per decision, before the seats' rewards
            uint32 NewKills = 0;                        // ... and the kills since the last decision

            // Gauntlet on.
            uint32 PullsCleared = 0;
            uint32 NextPullMs = 0;                      // spawn the next pull at this episode time
            bool EliteOrHigherPull = false;

            // Companion and party: the scripted owner (Env::Allies[0]).
            std::array<WorldSession*, 2> OwnerSessions{};
            std::array<ObjectGuid::LowType, 2> OwnerGuids{};
            uint8 OwnerActiveSession = 0;
            uint8 OwnerClass = 0;
            CompanionOwner::State Owner;
            bool OwnerDied = false;
            uint64 OwnerDamageTaken = 0;
            uint64 ThreatOnOwner = 0;

            // Party: the real (sim) group of the owner and the seats, rebuilt every episode.
            Group* PartyGroup = nullptr;

            // PvP: the scripted opponent.
            std::array<WorldSession*, 2> OpponentSessions{};
            std::array<ObjectGuid::LowType, 2> OpponentGuids{};
            uint8 OpponentActiveSession = 0;
            uint8 OpponentClass = 0;
            Role OpponentRole = Role::Dps;
            CompanionOwner::State Opponent;
        };

        [[nodiscard]] bool HasDuel() const { return _mode >= ArenaMode::Duel; }
        [[nodiscard]] bool HasPack() const { return _mode >= ArenaMode::Pack; }
        [[nodiscard]] bool HasGauntlet() const { return _mode >= ArenaMode::Gauntlet; }
        [[nodiscard]] bool HasCompanion() const { return _mode >= ArenaMode::Companion; }
        [[nodiscard]] bool HasParty() const { return _mode >= ArenaMode::Party; }
        [[nodiscard]] bool HasPvp() const { return _mode >= ArenaMode::Pvp; }
        /// Stages 1-6 fight creatures; 7 and 8 keep their layouts but fight a player, without pulls or allies.
        [[nodiscard]] bool IsPve() const { return _mode < ArenaMode::Pvp; }
        [[nodiscard]] bool IsArena() const { return _mode == ArenaMode::Arena; }
        [[nodiscard]] bool IsParty() const { return _mode == ArenaMode::Party; }

        // Core (ClassRoleScenario.cpp).
        void BuildLayout(Layout& layout) const;
        [[nodiscard]] Layout const& PickLayout(Role role, uint8 maxMinLevel) const;
        [[nodiscard]] Player* SeatBot(EnvData const& data, uint32 seat) const;
        bool Rebuild(Env& env);
        /// Create, place and dress seat `seat`'s next character on its idle session (`newSession`).
        bool BuildSeat(Env& env, uint32 seat, Map*& map, uint8 level, Position const& start, uint8& newSession);
        void Configure(Player* bot, Seat& seat) const;
        void StartFight(Player* bot, Unit* dummy, Seat const& seat) const;
        [[nodiscard]] static SpellInfo const* TrinketSpell(Item const* item);
        [[nodiscard]] bool IsActionAllowed(Seat const& seat, Player* bot, Unit* target, uint32 action) const;
        [[nodiscard]] bool IsSpellActionAllowed(Player* bot, Unit* target, ActionCatalog::Action const& def) const;
        /// Casts a spell action at `target` (may be null: self-cast spells only). Returns true if it started.
        bool ApplySpellAction(Player* bot, Unit* target, ActionCatalog::Action const& def, Seat& seat) const;
        void UpdateDummyHealth(Env const& env, Seat const& seat, Unit* dummy) const;
        /// What the seat's actions aim at: the dummy or opponent, the selected pack enemy (the nearest living one
        /// when the selection is dead), the enemy player. Null between gauntlet pulls.
        [[nodiscard]] Unit* CurrentTarget(Env const& env, uint32 seat);
        void ApplySeatAction(Env& env, uint32 seat, int32 action);
        void ObserveSeat(Env& env, uint32 seat, float* obs, uint8* mask);
        [[nodiscard]] float SeatReward(Env& env, uint32 seat);
        void SeatEpisodeInfo(Env const& env, uint32 seat, float* info) const;

        // Critic state (ClassRoleState.cpp).
        void BuildState(Env const& env, float* state) const;

        // Duel stage (ClassRoleDuel.cpp).
        void StartDuel(Player* bot, Seat& seat) const;
        [[nodiscard]] bool IsDuelActionAllowed(Player* bot, Unit* opponent, uint32 duelAction, Seat const& seat) const;
        void ApplyDuelAction(Player* bot, Unit* opponent, uint32 duelAction, Seat& seat) const;
        void ObserveDuel(Env const& env, Seat const& seat, Player* bot, Unit* opponent, float* obs) const;
        /// `opponentDead` defaults to whether `opponent` is dead.
        [[nodiscard]] float DuelReward(Env const& env, uint32 seat, Player* bot, Unit* opponent,
            int8 opponentDead = -1);
        [[nodiscard]] static float CastReward(Player* bot, AgentStats const& step, Seat& seat);
        void DuelEpisodeInfo(Env const& env, uint32 seat, float* info) const;
        [[nodiscard]] float DesiredRange(Seat const& seat) const;

        // Pack and gauntlet stages (ClassRolePack.cpp).
        void StartSeatPack(Player* bot, Seat& seat) const;
        bool SpawnPull(Env& env, Map* map);
        void UpdatePack(Env& env);
        /// Once per decision before the seats' rewards: whether the pull was just cleared, and new kills.
        void AssessPull(Env& env);
        /// Once per decision after the seats' rewards: clear a finished gauntlet pull and schedule the next.
        void FinishPull(Env& env);
        void ObservePack(Env const& env, uint32 seat, Player* bot, float* obs) const;
        void ObserveGauntlet(Env const& env, uint32 seat, Player* bot, float* obs) const;
        [[nodiscard]] bool IsPackActionAllowed(Env const& env, uint32 seat, Player* bot, uint32 packAction) const;
        void ApplyPackAction(Env& env, uint32 seat, Player* bot, Unit* target, uint32 packAction);
        [[nodiscard]] bool IsGauntletActionAllowed(Player* bot, Unit* target, uint32 gauntletAction,
            Seat const& seat) const;
        void ApplyGauntletAction(Player* bot, Unit* target, uint32 gauntletAction, Seat& seat) const;
        [[nodiscard]] float PackReward(Env& env, uint32 seat, Player* bot);
        void PackEpisodeInfo(Env const& env, uint32 seat, float* info) const;
        void GauntletEpisodeInfo(Env const& env, uint32 seat, float* info) const;

        // Companion stage (ClassRoleCompanion.cpp).
        [[nodiscard]] Player* FindOwner(EnvData const& data) const;
        bool RebuildOwner(Env& env, Player* anchor, Map* map, uint8 level);
        void DestroyOwner(Env& env);
        void UpdateOwner(Env& env);
        void ObserveCompanion(Env const& env, uint32 seat, Player* bot, float* obs) const;
        [[nodiscard]] bool IsCompanionActionAllowed(Env const& env, uint32 seat, Player* bot,
            uint32 companionAction) const;
        void ApplyCompanionAction(Env& env, uint32 seat, Player* bot, uint32 companionAction);
        [[nodiscard]] float CompanionReward(Env& env, uint32 seat, Player* bot);
        void CompanionEpisodeInfo(Env const& env, uint32 seat, float* info) const;

        // Party stage (ClassRoleParty.cpp).
        /// The seat index of teammate slot `slot` (0..PARTY_MEMBERS-1) of `seat`: the other seats in order.
        [[nodiscard]] static uint32 TeammateSeat(uint32 seat, uint32 slot) { return slot < seat ? slot : slot + 1; }
        /// The party's living tank (a tank seat), or null.
        [[nodiscard]] Player* PartyTank(EnvData const& data) const;
        /// Group the owner (the leader) and every seat into a party: a core Group flagged as a sim group, so party
        /// spells, auras and group heals work as in play while nothing is written to the database.
        void FormParty(Env& env);
        void DisbandParty(Env& env);
        void ObserveParty(Env const& env, uint32 seat, Player* bot, float* obs) const;
        [[nodiscard]] bool IsPartyActionAllowed(Env const& env, uint32 seat, Player* bot, uint32 partyAction) const;
        void ApplyPartyAction(Env& env, uint32 seat, Player* bot, uint32 partyAction);
        [[nodiscard]] float PartyReward(Env& env, uint32 seat, Player* bot);
        void PartyEpisodeInfo(Env const& env, uint32 seat, float* info) const;

        // PvP stages (ClassRolePvp.cpp).
        /// The seat's enemy player: the scripted opponent (stage 7) or the other seat's bot (stage 8).
        [[nodiscard]] Player* FindOpponent(Env const& env, uint32 seat) const;
        bool StartPvp(Env& env, Map* map);
        bool RebuildOpponent(Env& env, Player* bot, Map* map);
        void DestroyOpponent(Env& env);
        void UpdatePvp(Env& env);
        void ObservePvp(Env const& env, uint32 seat, Player* bot, float* obs) const;
        [[nodiscard]] float PvpReward(Env& env, uint32 seat, Player* bot);
        void PvpEpisodeInfo(Env const& env, uint32 seat, float* info) const;

        ArenaMode _mode;
        std::string _name;
        uint32 _arenaMapId;
        Position _arenaPosition;
        uint32 _seatCount = 1;

        std::vector<Layout> _layouts;
        ScenarioSpec _spec;
        uint32 _duelInfoFirst = INFO_COUNT;
        uint32 _packInfoFirst = 0;
        uint32 _gauntletInfoFirst = 0;
        uint32 _companionInfoFirst = 0;
        uint32 _partyInfoFirst = 0;
        uint32 _pvpInfoFirst = 0;
        std::vector<EnvData> _data;
    };
}

#endif
