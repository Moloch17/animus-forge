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

#include "ClassRoleCommon.h"
#include "ClassRoleLayout.h"
#include "CompanionOwner.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Scenario.h"
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

    using ArenaMode = AnimusForge::ClassRole::Stage;

    /// Every class/role, every level, race, spec and build, at one curriculum stage, as layouts of one policy.
    ///
    /// Each learned agent of an env is a seat: every episode it becomes a new character of a class/role drawn from
    /// AnimusForge.ClassRoles (all 18 by default) -- a race the class allows, random gender, level (1-80, 55-80 for
    /// death knights), one of the role's specs with a random talent build that fills the spec's tree to its capstone
    /// first, the trainer spells of the level, and random level-appropriate gear including trinkets. A seat's
    /// layout is its class/role's: observation features and actions fixed per class/role (see ClassRoleLayout and
    /// SeatEncoder), padded to the largest layout's on the wire (see LayoutSpec). The learner shares one trunk between
    /// all layouts, with an input adapter and an action head per layout.
    ///
    /// The critic state is class-agnostic (BuildState): every seat's and enemy's essentials, the owner and the
    /// pull timing.
    class ClassRoleScenario final : public Scenario, public AnimusForge::ClassRole::LayoutConstants
    {
    public:
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

        enum PackInfoColumn : uint32
        {
            PACK_INFO_KILLS             = 0,
            PACK_INFO_INTERRUPTS        = 1,
            PACK_INFO_PACK_SIZE         = 2,    // creatures in the first pull
            PACK_INFO_LINKED            = 3,
            PACK_INFO_COUNT
        };

        enum GauntletInfoColumn : uint32
        {
            GAUNTLET_INFO_PULLS_CLEARED = 0,
            GAUNTLET_INFO_FOOD_USED     = 1,
            GAUNTLET_INFO_DRINK_USED    = 2,
            GAUNTLET_INFO_SUSTAIN_CASTS = 3,
            GAUNTLET_INFO_COUNT
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

        enum PartyInfoColumn : uint32
        {
            PARTY_INFO_TEAMMATES_DIED   = 0,
            PARTY_INFO_TEAMMATE_DAMAGE_TAKEN = 1,
            PARTY_INFO_TEAMMATE_HEALING = 2,    // effective healing the seat did on its teammates
            PARTY_INFO_THREAT_ON_TEAMMATES = 3, // enemy-decisions spent attacking non-tank teammates
            PARTY_INFO_SEAT             = 4,
            PARTY_INFO_COUNT
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
        using Layout = AnimusForge::ClassRole::Layout;

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
        [[nodiscard]] Layout const& PickLayout(Role role, uint8 maxMinLevel) const;
        [[nodiscard]] Player* SeatBot(EnvData const& data, uint32 seat) const;
        bool Rebuild(Env& env);
        /// Create, place and dress seat `seat`'s next character on its idle session (`newSession`).
        bool BuildSeat(Env& env, uint32 seat, Map*& map, uint8 level, Position const& start, uint8& newSession);
        void Configure(Player* bot, Seat& seat) const;
        void StartFight(Player* bot, Unit* dummy, Seat const& seat) const;
        void UpdateDummyHealth(Env const& env, Seat const& seat, Unit* dummy) const;
        /// What the seat's actions aim at: the dummy or opponent, the selected pack enemy (the nearest living one
        /// when the selection is dead), the enemy player. Null between gauntlet pulls.
        [[nodiscard]] Unit* CurrentTarget(Env const& env, uint32 seat);
        /// What the encoder needs to know about seat `seat` that only the env knows (see SeatView).
        [[nodiscard]] SeatView ViewSeat(Env const& env, uint32 seat, Player* bot, Unit* target) const;
        void ApplySeatAction(Env& env, uint32 seat, int32 action);
        void ObserveSeat(Env& env, uint32 seat, float* obs, uint8* mask);
        [[nodiscard]] float SeatReward(Env& env, uint32 seat);
        void SeatEpisodeInfo(Env const& env, uint32 seat, float* info) const;

        // Critic state (ClassRoleState.cpp).
        void BuildState(Env const& env, float* state) const;

        // Duel stage (ClassRoleDuel.cpp).
        void StartDuel(Player* bot, Seat& seat) const;
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
        /// The pull timing of the gauntlet block.
        void ViewPull(Env const& env, SeatView& view) const;
        [[nodiscard]] float PackReward(Env& env, uint32 seat, Player* bot);
        void PackEpisodeInfo(Env const& env, uint32 seat, float* info) const;
        void GauntletEpisodeInfo(Env const& env, uint32 seat, float* info) const;

        // Companion stage (ClassRoleCompanion.cpp).
        [[nodiscard]] Player* FindOwner(EnvData const& data) const;
        bool RebuildOwner(Env& env, Player* anchor, Map* map, uint8 level);
        void DestroyOwner(Env& env);
        void UpdateOwner(Env& env);
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
        [[nodiscard]] float PartyReward(Env& env, uint32 seat, Player* bot);
        void PartyEpisodeInfo(Env const& env, uint32 seat, float* info) const;

        // PvP stages (ClassRolePvp.cpp).
        /// The seat's enemy player: the scripted opponent (stage 7) or the other seat's bot (stage 8).
        [[nodiscard]] Player* FindOpponent(Env const& env, uint32 seat) const;
        bool StartPvp(Env& env, Map* map);
        bool RebuildOpponent(Env& env, Player* bot, Map* map);
        void DestroyOpponent(Env& env);
        void UpdatePvp(Env& env);
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
