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

#ifndef MOD_ANIMUS_FORGE_CLASS_ROLE_ENCOUNTERS_H
#define MOD_ANIMUS_FORGE_CLASS_ROLE_ENCOUNTERS_H

#include "BotSlot.h"
#include "ClassRoleScenario.h"
#include "Encounter.h"
#include "ObjectGuid.h"
#include "RewardLedger.h"
#include "ScriptedPlayer.h"
#include "StageDefinition.h"
#include <array>
#include <vector>

class Group;

/*
 * The encounters a StageDefinition can ask for (see Encounter). Each keeps its state per env, sized at construction.
 */
namespace AnimusForge::ClassRole
{
    /// Stage 1: a training dummy in range that takes no damage. Its health follows a random curve over the episode, so
    /// execute-range abilities are sometimes usable. Reward: damage dealt / the level's damage scale.
    class DummyEncounter final : public Encounter
    {
    public:
        DummyEncounter(ClassRoleScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override { return { RewardTerm::DamageDealt }; }
        bool Build(Env& env, Map* map, uint8 level) override;
        void BeforeSeatAction(Env& env, uint32 seat, Unit* target) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;

    private:
        struct HealthCurve
        {
            float Start = 1.0f;         // dummy health fraction at episode start
            float End = 0.0f;           // ... and at the episode time limit
        };

        std::vector<HealthCurve> _curves;
    };

    /// A same-level creature spawned out of aggro range, which fights back. Reward: CombatReward::OneOnOne.
    class CreatureEncounter final : public Encounter
    {
    public:
        using Encounter::Encounter;

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
    };

    /// Packs of creatures (casters included, often linked): one pack, or the gauntlet's pull after pull with breaks
    /// between. Pulls are shared by every seat: whether one was cleared is decided once per decision, before the
    /// seats' rewards, and a cleared gauntlet pull is removed after them. With an owner, pulls spawn around it, and
    /// after a pull everyone who died stands up again (a pull that killed everyone is cleared away).
    class PullsEncounter final : public Encounter
    {
    public:
        PullsEncounter(ClassRoleScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void UpdateEnemies(Env& env) override;
        void Update(Env& env) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void BeforeRewards(Env& env) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void AfterRewards(Env& env) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;

    private:
        struct SeatPull
        {
            uint32 Interrupts = 0;
            ObjectGuid PendingInterrupt;        // a casting enemy the seat just cast an interrupt at
            uint64 PullDamageTaken = 0;
            uint32 FoodItem = 0;
            uint32 DrinkItem = 0;
            uint32 FoodUsed = 0;
            uint32 DrinkUsed = 0;
            uint32 SustainCasts = 0;
        };

        struct EnvPulls
        {
            bool Linked = false;
            uint32 PackSize = 0;                // creatures in the episode's first pull
            uint32 PullKills = 0;               // dead enemies of the current pull
            uint32 Kills = 0;
            uint32 PullStartMs = 0;
            bool PullCleared = false;           // decided once per decision, before the seats' rewards
            uint32 NewKills = 0;                // ... and the kills since the last decision
            uint32 PullsCleared = 0;
            uint32 QuietSinceMs = 0;            // episode time the last pull ended
            uint32 NextPullMs = 0;              // spawn the next pull at this episode time
            bool EliteOrHigher = false;
            uint32 Wipes = 0;                   // owner stages: pulls that killed everyone and were cleared away
            std::array<SeatPull, MAX_SEATS> Seats;
        };

        [[nodiscard]] bool Gauntlet() const { return _scenario.Stage().Schedule == PullSchedule::Gauntlet; }
        bool SpawnPull(Env& env, Map* map);
        /// The field is empty: schedule the next pull and restart the seats' target selection.
        void EndPull(Env& env, EnvPulls& pulls);
        void Recover(Env& env);

        std::vector<EnvPulls> _envs;
    };

    /// A scripted player of a random class and role near the seats' level, whom the seats fight for (companion and
    /// party stages). It is the env's ally 0.
    class OwnerEncounter final : public Encounter
    {
    public:
        OwnerEncounter(ClassRoleScenario& scenario, uint32 envs);

        [[nodiscard]] Player* Find(Env const& env) const;
        [[nodiscard]] Role RoleOf(Env const& env) const;
        [[nodiscard]] ScriptedPlayer::State& StateOf(Env const& env);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        void OnRecovered(Env& env, int32 who) override;
        void Teardown(Env& env) override;

    private:
        struct SeatOwner
        {
            uint64 Healing = 0;                 // effective healing the seat did on the owner
            uint64 ThreatOnBot = 0;             // enemy-decisions spent attacking the seat
            bool DeathSeen = false;             // the seat has paid for the owner's current death
        };

        struct EnvOwner
        {
            BotSlot Bot;
            uint8 Class = 0;
            Role PlayRole = Role::Dps;
            ScriptedPlayer::State Script;
            bool Died = false;
            uint32 Deaths = 0;
            bool DeathCounted = false;
            uint64 DamageTaken = 0;
            uint64 ThreatOnOwner = 0;
            std::array<SeatOwner, MAX_SEATS> Seats;
        };

        std::vector<EnvOwner> _envs;
    };

    /// The owner and the seats form a real core group every episode (a sim group: it lives only in memory), so party
    /// spells, auras and group heals work as in play. Each seat sees its three teammates and is rewarded for them.
    class PartyEncounter final : public Encounter
    {
    public:
        PartyEncounter(ClassRoleScenario& scenario, uint32 envs);

        /// The party's living tank seat, or null.
        [[nodiscard]] Player* Tank(Env const& env) const;

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeRebuild(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void OnRecovered(Env& env, int32 who) override;
        void Teardown(Env& env) override;

    private:
        struct SeatParty
        {
            uint64 TeammateDamageTaken = 0;
            uint64 TeammateHealing = 0;
            uint64 ThreatOnTeammates = 0;
            uint32 TeammatesDied = 0;
            std::array<bool, MAX_SEATS> TeammateDeathSeen{};
        };

        struct EnvParty
        {
            Group* PartyGroup = nullptr;
            std::array<SeatParty, MAX_SEATS> Seats;
        };

        /// The seat index of teammate slot `slot` (0..PARTY_MEMBERS-1) of `seat`: the other seats in order.
        [[nodiscard]] static uint32 TeammateSeat(uint32 seat, uint32 slot) { return slot < seat ? slot : slot + 1; }
        void Disband(Env& env);

        std::vector<EnvParty> _envs;
    };

    /// An enemy player: one played by a script (Opposition::ScriptedPlayer), or the other seat (self-play,
    /// Opposition::MirrorSeat). Reward: CombatReward::OneOnOne against it.
    class OpponentEncounter final : public Encounter
    {
    public:
        OpponentEncounter(ClassRoleScenario& scenario, uint32 envs, bool mirror);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Teardown(Env& env) override;

    private:
        struct EnvOpponent
        {
            BotSlot Bot;
            uint8 Class = 0;
            Role PlayRole = Role::Dps;
            ScriptedPlayer::State Script;
        };

        [[nodiscard]] Player* Find(Env const& env, uint32 seat) const;
        bool RebuildScripted(Env& env, Player* bot, Map* map);

        bool _mirror;
        std::vector<EnvOpponent> _envs;
    };
}

#endif
