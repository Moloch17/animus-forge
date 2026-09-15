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

/*
 * The centralized critic state of ClassRoleScenario: a class-agnostic picture of the whole env -- every seat, every
 * enemy slot (the PvP stages' enemy player is slot 0 in stage 6; in stage 7 each seat is the other's enemy), the
 * owner, and the pull timing. The critic sees it together with each agent's own observation (see the learner's
 * LayoutCritic), so values can account for teammates and enemies the agent's observation only partly shows.
 */

#include "ClassRoleScenario.h"
#include "Creature.h"
#include "Env.h"
#include "Player.h"
#include <algorithm>

namespace
{
    constexpr std::array<uint8, 10> STATE_CLASSES =
    {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_DEATH_KNIGHT, CLASS_SHAMAN,
        CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };

    constexpr float POSITION_SCALE = 40.0f;
    constexpr float NEXT_PULL_SCALE_MS = 20000.0f;

    float Relative(float coordinate, float origin)
    {
        return std::clamp((coordinate - origin) / POSITION_SCALE, -2.0f, 2.0f);
    }

    float OtherPower(Unit const* unit)
    {
        Powers const power = unit->getPowerType();
        if (power == POWER_MANA)
            return 0.0f;

        uint32 const maxPower = unit->GetMaxPower(power);
        return maxPower ? float(unit->GetPower(power)) / float(maxPower) : 0.0f;
    }
}

void AnimusForge::ClassRoleScenario::BuildState(Env const& env, float* state) const
{
    std::fill(state, state + _spec.StateDim, 0.0f);

    EnvData const& data = _data[env.Index];
    float const originX = _arenaPosition.GetPositionX();
    float const originY = _arenaPosition.GetPositionY();

    state[STATE_EPISODE_TIME] = env.EpisodeLengthMs
        ? std::min(1.0f, float(env.EpisodeElapsedMs) / float(env.EpisodeLengthMs)) : 0.0f;
    if (HasPack() && IsPve())
    {
        bool const pullActive = !env.Targets.empty();
        state[STATE_PULL_ACTIVE] = pullActive ? 1.0f : 0.0f;
        state[STATE_PULLS_CLEARED] = std::min(1.0f, float(data.PullsCleared) / 10.0f);
        state[STATE_NEXT_PULL] = pullActive ? 0.0f
            : std::clamp((float(data.NextPullMs) - float(env.EpisodeElapsedMs)) / NEXT_PULL_SCALE_MS, 0.0f, 1.0f);
        state[STATE_ELITE_PULL] = pullActive && data.EliteOrHigherPull ? 1.0f : 0.0f;
        state[STATE_LINKED_PULL] = pullActive && data.PackLinked ? 1.0f : 0.0f;
    }

    Player* owner = HasCompanion() ? FindOwner(data) : nullptr;
    if (owner && owner->IsInWorld())
    {
        state[STATE_OWNER_PRESENT] = 1.0f;
        state[STATE_OWNER_ALIVE] = owner->IsAlive() ? 1.0f : 0.0f;
        state[STATE_OWNER_HEALTH] = owner->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = owner->GetMaxPower(POWER_MANA))
            state[STATE_OWNER_MANA] = float(owner->GetPower(POWER_MANA)) / float(maxMana);
        state[STATE_OWNER_X] = Relative(owner->GetPositionX(), originX);
        state[STATE_OWNER_Y] = Relative(owner->GetPositionY(), originY);
        state[STATE_OWNER_IN_COMBAT] = owner->IsInCombat() ? 1.0f : 0.0f;
    }

    std::array<Player*, MAX_SEATS> bots{};
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        Player* bot = env.FindBot(seat);
        Seat const& slot = data.Seats[seat];
        bots[seat] = bot;
        if (!bot || !slot.L)
            continue;

        float* features = state + STATE_GLOBAL_COUNT + seat * STATE_SEAT_FEATURES;
        features[STATE_SEAT_PRESENT] = 1.0f;
        features[STATE_SEAT_ALIVE] = bot->IsAlive() ? 1.0f : 0.0f;
        features[STATE_SEAT_HEALTH] = bot->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = bot->GetMaxPower(POWER_MANA))
            features[STATE_SEAT_MANA] = float(bot->GetPower(POWER_MANA)) / float(maxMana);
        features[STATE_SEAT_OTHER_POWER] = OtherPower(bot);
        features[STATE_SEAT_LEVEL] = float(slot.Level) / float(DEFAULT_MAX_LEVEL);
        features[STATE_SEAT_ROLE_FIRST + uint32(slot.L->PlayRole())] = 1.0f;
        for (uint32 i = 0; i < STATE_CLASSES.size(); ++i)
            features[STATE_SEAT_CLASS_FIRST + i] = STATE_CLASSES[i] == slot.L->Profile->Class ? 1.0f : 0.0f;
        features[STATE_SEAT_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
        features[STATE_SEAT_CASTING] = bot->IsNonMeleeSpellCast(false, false, true) ? 1.0f : 0.0f;
        features[STATE_SEAT_X] = Relative(bot->GetPositionX(), originX);
        features[STATE_SEAT_Y] = Relative(bot->GetPositionY(), originY);
    }

    // The enemies: the env's targets (creatures, or stage 6's enemy player); in stage 7 each seat's opponent is the
    // other seat, already in the seat block.
    float const leadLevel = float(data.Seats[0].Level);
    for (uint32 slot = 0; slot < env.Targets.size() && slot < PACK_SLOTS; ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy)
            continue;

        float* features = state + STATE_GLOBAL_COUNT + MAX_SEATS * STATE_SEAT_FEATURES + slot * STATE_ENEMY_FEATURES;
        Unit const* victim = enemy->GetVictim();

        features[STATE_ENEMY_PRESENT] = 1.0f;
        features[STATE_ENEMY_ALIVE] = enemy->IsAlive() ? 1.0f : 0.0f;
        features[STATE_ENEMY_HEALTH] = enemy->GetHealthPct() / 100.0f;
        features[STATE_ENEMY_X] = Relative(enemy->GetPositionX(), originX);
        features[STATE_ENEMY_Y] = Relative(enemy->GetPositionY(), originY);
        features[STATE_ENEMY_CASTING] = enemy->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
        features[STATE_ENEMY_ELITE] = enemy->ToCreature() && enemy->ToCreature()->isElite() ? 1.0f : 0.0f;
        features[STATE_ENEMY_LEVEL_DIFF] = (float(enemy->GetLevel()) - leadLevel) / 5.0f;
        features[STATE_ENEMY_IN_COMBAT] = enemy->IsInCombat() ? 1.0f : 0.0f;
        features[STATE_ENEMY_ON_OWNER] = victim && victim == owner ? 1.0f : 0.0f;
        for (uint32 seat = 0; seat < _seatCount; ++seat)
            if (victim && victim == bots[seat])
                features[STATE_ENEMY_ON_SEAT_FIRST + seat] = 1.0f;
    }
}
