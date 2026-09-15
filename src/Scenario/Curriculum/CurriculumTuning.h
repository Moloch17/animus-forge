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

#ifndef MOD_ANIMUS_FORGE_CURRICULUM_CURRICULUM_TUNING_H
#define MOD_ANIMUS_FORGE_CURRICULUM_CURRICULUM_TUNING_H

#include "Define.h"
#include <boost/json/fwd.hpp>
#include <string>

/*
 * Every value that shapes what the curriculum trains on and what it is paid for: the characters, parties,
 * pulls and scripted players it meets, and the reward weights. Each is the config key AnimusForge.Curriculum.<key>
 * (mod_animus_forge.conf.dist documents them); the effective values are recorded in each stage's stage.json and so in
 * every run directory. Visit lists them once, for loading and for writing.
 *
 * Per-decision reward terms are tuned per 50 ms decision and scaled with AnimusForge.DecisionMs.
 */
namespace AnimusForge::Curriculum
{
    struct CurriculumTuning
    {
        /// The seat characters.
        struct CharacterTuning
        {
            uint32 HighLevelFirst = 61;         // levels at or above this are "high"
            int32 HighLevelChance = 50;         // percent of characters drawn from the high levels
        } Characters;

        /// Which party seats have a character, and their roles.
        struct PartyTuning
        {
            int32 SizeWeight1 = 20;             // relative chance of 1, 2, 3 or 4 seats with a character
            int32 SizeWeight2 = 20;
            int32 SizeWeight3 = 20;
            int32 SizeWeight4 = 40;
            int32 ClassicChance = 50;           // percent: tank, healer, damage dealers instead of drawn roles
            int32 RoleTankChance = 25;          // drawn roles: percent tanks, healers, the rest damage dealers
            int32 RoleHealerChance = 25;
            // Rewards added to the owner's, per teammate.
            float TeammateDamageTakenDps = 0.5f;        // damage dealers: a non-tank teammate's damage taken
            float TeammateDamageTakenProtector = 1.0f;  // tanks and healers
            float TeammateHealing = 2.0f;               // healers: effective healing, fraction of its health
            float TankLoseTeammate = 0.02f;             // tanks: per enemy on a non-tank teammate, per decision
            float TeammateDeath = 3.0f;
        } Party;

        /// One-on-one fights against a creature (the duel) or a player (PvP).
        struct DuelTuning
        {
            float DamageDealt = 2.0f;           // fraction of the opponent's health: a kill is worth this in damage
            float DamageTaken = 1.0f;           // fraction of the bot's health
            float Approach = 0.5f;              // shaping toward the spec's range, per 40 yd closed
            float StealthOpener = 0.5f;
            float StepCost = 0.0002f;           // per decision
            float Kill = 2.0f;
            float FastKill = 3.0f;              // times the fraction of the episode still left
            float HealthKept = 2.0f;            // times the fraction of the bot's health not lost
            float Death = 3.0f;
            float MeleeRange = 3.5f;            // the range the approach shaping aims for, melee specs
            float RangedRange = 25.0f;          // ... ranged specs
        } Duel;

        /// Cast-time spells, from the duel stage on.
        struct CastingTuning
        {
            float TimeWasted = 0.03f;           // per second spent on a cast that did not finish
            float TimeCompleted = 0.03f;        // per second of cast time of a cast that finished, in combat
        } Casting;

        /// Packs and the gauntlet's pull after pull.
        struct PullTuning
        {
            int32 LinkedChance = 70;            // percent of pulls whose members aggro together
            int32 EliteChance = 15;             // gauntlet: a single elite instead of a pack
            int32 HigherLevelChance = 25;       // gauntlet: a pack 1-3 levels above the bot
            int32 PartyEliteChance = 50;        // party: per pack member
            uint32 NextPullMinMs = 8000;        // gauntlet: the break between pulls
            uint32 NextPullMaxMs = 20000;
            uint32 OwnerEngageMinMs = 1500;     // with an owner: when it walks over to a new pull
            uint32 OwnerEngageMaxMs = 5000;
            uint32 PartyOwnerEngageMinMs = 4000;    // ... in a party, after the tank has had time to pull
            uint32 PartyOwnerEngageMaxMs = 7000;
            uint32 OwnerPullsMinMs = 500;       // ... when the owner starts the pull itself
            uint32 OwnerPullsMaxMs = 1500;
            int32 OwnerPullsChance = 30;        // percent of pulls a damage dealer or healer owner starts (tanks: all)
            float RecoverFraction = 0.5f;       // owner stages: health and mana the dead stand up with after a pull
            // Rewards.
            float DamageDealt = 2.0f;           // fraction of the pull's total health
            float DamageTaken = 1.0f;           // pack: fraction of the bot's health
            float GauntletDamageTaken = 1.5f;   // gauntlet on: surviving many pulls matters more than any one
            float Approach = 0.5f;
            float StealthOpener = 0.5f;
            float Interrupt = 0.3f;
            float Kill = 0.5f;
            float StepCost = 0.0002f;           // per decision
            float Clear = 2.0f;
            float FastClear = 3.0f;             // pack: times the fraction of the episode still left
            float FastPull = 2.0f;              // gauntlet: times 1 - pull time / 60 s
            float HealthKept = 2.0f;            // times the fraction of the bot's health not lost this pull
            float PackDeath = 3.0f;
            float GauntletDeath = 5.0f;
            float OwnerClearScale = 2.0f;       // owner stages: kills and clears count this many times
        } Pulls;

        /// The scripted owner of the companion and party stages.
        struct OwnerTuning
        {
            int32 LevelSpread = 2;              // its level: the bot's plus or minus this
            int32 TankChance = 25;              // percent tanks, healers, the rest damage dealers
            int32 HealerChance = 25;
            // Rewards added to the pulls'.
            float DamageTakenDps = 1.0f;        // damage dealers: the owner's damage taken, fraction of its health
            float DamageTakenProtector = 2.0f;  // tanks and healers exist to prevent it
            float TankOwnerDamageShare = 0.25f; // a tank owner is hit by design: its damage taken counts this much
            float Healing = 2.0f;               // healers: effective healing, fraction of the owner's health
            float TankDamageRefund = 0.5f;      // tanks: soften the pulls' damage taken
            float TankHold = 0.002f;            // tanks: per enemy on the tank, per decision
            float TankLose = 0.02f;             // tanks: per enemy on the owner, per decision
            float PulledThreat = 0.004f;        // damage dealers and healers: per enemy on the bot, per decision
            float SoloFight = 0.01f;            // per decision in combat while the owner is not
            float FollowFar = 0.002f;           // per decision out of combat beyond FollowFarDistance
            float FollowNear = 0.0005f;         // per decision out of combat within FollowNearDistance
            float FollowFarDistance = 25.0f;
            float FollowNearDistance = 12.0f;
            float Death = 6.0f;
        } Owner;

        /// Resurrecting: a seat's own Soulstone or Reincarnation, and revives on allies (companion and party stages).
        struct ResurrectionTuning
        {
            uint32 GraceMs = 20000;             // the dead wait this long for a resurrection they can get before solo
                                                // stages end and owner stages stand them up (the next pull waits too)
            float ReviveAlly = 1.5f;            // a dead ally the seat resurrected stood up
        } Resurrection;

        /// The scripted enemy player of the PvP stage.
        struct OpponentTuning
        {
            int32 LevelSpread = 1;
            uint32 EngageMaxMs = 3000;          // it starts fighting up to this long into the episode
            int32 HealerChance = 20;
            int32 TankChance = 20;
        } Opponent;

        /// Scripted enemy players ambushing the owner (arenas with ambushers). Their class, role and level follow
        /// Opponent.* chances and spread.
        struct AmbushTuning
        {
            uint32 MinMs = 20000;               // beside pulls: they arrive this far into the episode ...
            uint32 MaxMs = 120000;              // ... at the latest
            uint32 EngageMaxMs = 3000;          // they start fighting up to this long after arriving
            float Kill = 3.0f;                  // every seat, per ambusher killed
        } Ambush;

        /// How the scripted players (owner, PvP opponent, ambushers) play.
        struct ScriptedPlayerTuning
        {
            uint32 SpellMinMs = 2000;           // time between damage spells
            uint32 SpellMaxMs = 4000;
            uint32 HealMinMs = 1500;            // time between heals
            uint32 HealMaxMs = 2500;
            uint32 WanderMinMs = 6000;          // between pulls: time between wander steps
            uint32 WanderMaxMs = 12000;
            float RegenFraction = 0.04f;        // of max health and mana per second, out of combat
            float HealBelow = 0.85f;            // healers heal party members under this health fraction
            float SelfHealBelow = 0.6f;         // PvP healers heal themselves under this
            float HealerRange = 30.0f;          // healers stay this close to the tank
            float TauntRange = 25.0f;
            float RangedMin = 20.0f;            // PvP: a ranged spec backs off inside half this ...
            float RangedMax = 30.0f;            // ... and closes in beyond this
        } ScriptedPlayers;

        /// Calls f(key, value) for every value, key relative to AnimusForge.Curriculum.
        template <typename T, typename F>
        static void Visit(T& tuning, F&& f)
        {
            f("Characters.HighLevelFirst", tuning.Characters.HighLevelFirst);
            f("Characters.HighLevelChance", tuning.Characters.HighLevelChance);

            f("Party.SizeWeight1", tuning.Party.SizeWeight1);
            f("Party.SizeWeight2", tuning.Party.SizeWeight2);
            f("Party.SizeWeight3", tuning.Party.SizeWeight3);
            f("Party.SizeWeight4", tuning.Party.SizeWeight4);
            f("Party.ClassicChance", tuning.Party.ClassicChance);
            f("Party.RoleTankChance", tuning.Party.RoleTankChance);
            f("Party.RoleHealerChance", tuning.Party.RoleHealerChance);
            f("Party.TeammateDamageTakenDps", tuning.Party.TeammateDamageTakenDps);
            f("Party.TeammateDamageTakenProtector", tuning.Party.TeammateDamageTakenProtector);
            f("Party.TeammateHealing", tuning.Party.TeammateHealing);
            f("Party.TankLoseTeammate", tuning.Party.TankLoseTeammate);
            f("Party.TeammateDeath", tuning.Party.TeammateDeath);

            f("Duel.DamageDealt", tuning.Duel.DamageDealt);
            f("Duel.DamageTaken", tuning.Duel.DamageTaken);
            f("Duel.Approach", tuning.Duel.Approach);
            f("Duel.StealthOpener", tuning.Duel.StealthOpener);
            f("Duel.StepCost", tuning.Duel.StepCost);
            f("Duel.Kill", tuning.Duel.Kill);
            f("Duel.FastKill", tuning.Duel.FastKill);
            f("Duel.HealthKept", tuning.Duel.HealthKept);
            f("Duel.Death", tuning.Duel.Death);
            f("Duel.MeleeRange", tuning.Duel.MeleeRange);
            f("Duel.RangedRange", tuning.Duel.RangedRange);

            f("Casting.TimeWasted", tuning.Casting.TimeWasted);
            f("Casting.TimeCompleted", tuning.Casting.TimeCompleted);

            f("Pulls.LinkedChance", tuning.Pulls.LinkedChance);
            f("Pulls.EliteChance", tuning.Pulls.EliteChance);
            f("Pulls.HigherLevelChance", tuning.Pulls.HigherLevelChance);
            f("Pulls.PartyEliteChance", tuning.Pulls.PartyEliteChance);
            f("Pulls.NextPullMinMs", tuning.Pulls.NextPullMinMs);
            f("Pulls.NextPullMaxMs", tuning.Pulls.NextPullMaxMs);
            f("Pulls.OwnerEngageMinMs", tuning.Pulls.OwnerEngageMinMs);
            f("Pulls.OwnerEngageMaxMs", tuning.Pulls.OwnerEngageMaxMs);
            f("Pulls.PartyOwnerEngageMinMs", tuning.Pulls.PartyOwnerEngageMinMs);
            f("Pulls.PartyOwnerEngageMaxMs", tuning.Pulls.PartyOwnerEngageMaxMs);
            f("Pulls.OwnerPullsMinMs", tuning.Pulls.OwnerPullsMinMs);
            f("Pulls.OwnerPullsMaxMs", tuning.Pulls.OwnerPullsMaxMs);
            f("Pulls.OwnerPullsChance", tuning.Pulls.OwnerPullsChance);
            f("Pulls.RecoverFraction", tuning.Pulls.RecoverFraction);
            f("Pulls.DamageDealt", tuning.Pulls.DamageDealt);
            f("Pulls.DamageTaken", tuning.Pulls.DamageTaken);
            f("Pulls.GauntletDamageTaken", tuning.Pulls.GauntletDamageTaken);
            f("Pulls.Approach", tuning.Pulls.Approach);
            f("Pulls.StealthOpener", tuning.Pulls.StealthOpener);
            f("Pulls.Interrupt", tuning.Pulls.Interrupt);
            f("Pulls.Kill", tuning.Pulls.Kill);
            f("Pulls.StepCost", tuning.Pulls.StepCost);
            f("Pulls.Clear", tuning.Pulls.Clear);
            f("Pulls.FastClear", tuning.Pulls.FastClear);
            f("Pulls.FastPull", tuning.Pulls.FastPull);
            f("Pulls.HealthKept", tuning.Pulls.HealthKept);
            f("Pulls.PackDeath", tuning.Pulls.PackDeath);
            f("Pulls.GauntletDeath", tuning.Pulls.GauntletDeath);
            f("Pulls.OwnerClearScale", tuning.Pulls.OwnerClearScale);

            f("Owner.LevelSpread", tuning.Owner.LevelSpread);
            f("Owner.TankChance", tuning.Owner.TankChance);
            f("Owner.HealerChance", tuning.Owner.HealerChance);
            f("Owner.DamageTakenDps", tuning.Owner.DamageTakenDps);
            f("Owner.DamageTakenProtector", tuning.Owner.DamageTakenProtector);
            f("Owner.TankOwnerDamageShare", tuning.Owner.TankOwnerDamageShare);
            f("Owner.Healing", tuning.Owner.Healing);
            f("Owner.TankDamageRefund", tuning.Owner.TankDamageRefund);
            f("Owner.TankHold", tuning.Owner.TankHold);
            f("Owner.TankLose", tuning.Owner.TankLose);
            f("Owner.PulledThreat", tuning.Owner.PulledThreat);
            f("Owner.SoloFight", tuning.Owner.SoloFight);
            f("Owner.FollowFar", tuning.Owner.FollowFar);
            f("Owner.FollowNear", tuning.Owner.FollowNear);
            f("Owner.FollowFarDistance", tuning.Owner.FollowFarDistance);
            f("Owner.FollowNearDistance", tuning.Owner.FollowNearDistance);
            f("Owner.Death", tuning.Owner.Death);

            f("Resurrection.GraceMs", tuning.Resurrection.GraceMs);
            f("Resurrection.ReviveAlly", tuning.Resurrection.ReviveAlly);

            f("Opponent.LevelSpread", tuning.Opponent.LevelSpread);
            f("Opponent.EngageMaxMs", tuning.Opponent.EngageMaxMs);
            f("Opponent.HealerChance", tuning.Opponent.HealerChance);
            f("Opponent.TankChance", tuning.Opponent.TankChance);

            f("Ambush.MinMs", tuning.Ambush.MinMs);
            f("Ambush.MaxMs", tuning.Ambush.MaxMs);
            f("Ambush.EngageMaxMs", tuning.Ambush.EngageMaxMs);
            f("Ambush.Kill", tuning.Ambush.Kill);

            f("ScriptedPlayers.SpellMinMs", tuning.ScriptedPlayers.SpellMinMs);
            f("ScriptedPlayers.SpellMaxMs", tuning.ScriptedPlayers.SpellMaxMs);
            f("ScriptedPlayers.HealMinMs", tuning.ScriptedPlayers.HealMinMs);
            f("ScriptedPlayers.HealMaxMs", tuning.ScriptedPlayers.HealMaxMs);
            f("ScriptedPlayers.WanderMinMs", tuning.ScriptedPlayers.WanderMinMs);
            f("ScriptedPlayers.WanderMaxMs", tuning.ScriptedPlayers.WanderMaxMs);
            f("ScriptedPlayers.RegenFraction", tuning.ScriptedPlayers.RegenFraction);
            f("ScriptedPlayers.HealBelow", tuning.ScriptedPlayers.HealBelow);
            f("ScriptedPlayers.SelfHealBelow", tuning.ScriptedPlayers.SelfHealBelow);
            f("ScriptedPlayers.HealerRange", tuning.ScriptedPlayers.HealerRange);
            f("ScriptedPlayers.TauntRange", tuning.ScriptedPlayers.TauntRange);
            f("ScriptedPlayers.RangedMin", tuning.ScriptedPlayers.RangedMin);
            f("ScriptedPlayers.RangedMax", tuning.ScriptedPlayers.RangedMax);
        }

        /// The values from the config, each defaulting to the value above; min/max pairs are ordered.
        [[nodiscard]] static CurriculumTuning Load();

        /// Every value as a JSON object, keys as in the config.
        [[nodiscard]] boost::json::object Json() const;
    };
}

#endif
