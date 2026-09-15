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

#include "Encounters.h"
#include "Creature.h"
#include "Env.h"
#include "Player.h"
#include "Random.h"
#include "TrainingDummy.h"
#include <algorithm>

namespace
{
    enum DummySpells : uint32
    {
        SPELL_BATTLE_STANCE     = 2457,
        SPELL_DEFENSIVE_STANCE  = 71,
    };

    constexpr float MELEE_DISTANCE = 2.0f;
    constexpr float RANGED_DISTANCE = 20.0f;
}

AnimusForge::ClassRole::DummyEncounter::DummyEncounter(ClassRoleScenario& scenario, uint32 envs)
    : Encounter(scenario), _curves(envs)
{
}

bool AnimusForge::ClassRole::DummyEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    SeatState& seat = _scenario.Data(env).Seats[0];
    Player* bot = _scenario.SeatBot(env, 0);
    SpecProfile const& spec = seat.L->Profile->Specs[seat.Spec];
    bool const melee = spec.Range == RangeBand::Melee;

    Creature* dummy = TrainingDummy::Spawn(bot, map, melee ? MELEE_DISTANCE : RANGED_DISTANCE);
    if (!dummy)
        return false;

    env.Targets = { dummy->GetGUID() };

    HealthCurve& curve = _curves[env.Index];
    curve.Start = frand(_scenario.Tuning().Dummy.StartHealthMin, 1.0f);
    curve.End = frand(0.0f, curve.Start);
    dummy->SetHealth(std::max<uint32>(1, uint32(float(dummy->GetMaxHealth()) * curve.Start)));

    bot->SetOrientation(bot->GetAngle(dummy));

    // A warrior has no stance until one is cast (a first login casts it), and nothing works without one.
    if (seat.L->Profile->Class == CLASS_WARRIOR)
        bot->CastSpell(bot, seat.L->PlayRole() == Role::Tank && bot->HasSpell(SPELL_DEFENSIVE_STANCE)
            ? SPELL_DEFENSIVE_STANCE : SPELL_BATTLE_STANCE, true);

    bot->Attack(dummy, melee);

    // Random swing phase so episodes do not all start on the same swing boundary.
    if (melee)
        bot->setAttackTimer(BASE_ATTACK, int32(urand(0, bot->GetAttackTime(BASE_ATTACK))));

    return true;
}

void AnimusForge::ClassRole::DummyEncounter::BeforeSeatAction(Env& env, uint32 /*seat*/, Unit* target)
{
    if (!target)
        return;

    HealthCurve const& curve = _curves[env.Index];
    float const progress = env.EpisodeLengthMs
        ? std::min(1.0f, float(env.EpisodeElapsedMs) / float(env.EpisodeLengthMs)) : 0.0f;
    float const fraction = curve.Start + (curve.End - curve.Start) * progress;

    target->SetHealth(std::max<uint32>(1, uint32(float(target->GetMaxHealth()) * fraction)));
}

void AnimusForge::ClassRole::DummyEncounter::Reward(Env& env, uint32 seatIndex, Player* /*bot*/,
    RewardLedger& ledger)
{
    ledger.Add(RewardTerm::DamageDealt, _scenario.Data(env).Seats[seatIndex].LastStepDamage);
}
