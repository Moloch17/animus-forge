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

#include "PackBlock.h"
#include "Creature.h"
#include "EncoderSupport.h"
#include "JsonWriter.h"
#include "Layout.h"
#include "Player.h"
#include <cmath>

namespace
{
    using namespace AnimusForge::ClassRole;

    bool IsSlotAllowed(SeatView const& view, uint32 slot)
    {
        if (slot >= view.EnemyCount || slot == view.TargetSlot || !view.Bot->IsAlive())
            return false;

        Unit const* enemy = view.Enemies[slot];
        return enemy && enemy->IsAlive();
    }
}

AnimusForge::ClassRole::BlockSize AnimusForge::ClassRole::PackBlock::Size(Layout const& layout) const
{
    uint32 const tactical = uint32(layout.Catalog().Tactical().size());
    return { OBS_GLOBAL_COUNT + PACK_SLOTS * SLOT_FEATURES + tactical * 2, PACK_SLOTS + tactical };
}

void AnimusForge::ClassRole::PackBlock::DescribeManifest(Layout const& layout, JsonWriter& json) const
{
    json.Key("slots").Value(PACK_SLOTS).Key("slot_features").Value(uint32(SLOT_FEATURES)).Key("tactical");
    WriteSpellList(json, layout.Catalog().Tactical());
}

void AnimusForge::ClassRole::PackBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;

    uint32 alive = 0;
    uint32 inCombat = 0;
    for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
    {
        Unit* enemy = view.Enemies[slot];
        if (!enemy)
            continue;

        float* features = obs + OBS_GLOBAL_COUNT + slot * SLOT_FEATURES;
        float const bearing = bot->GetRelativeAngle(enemy);
        Unit const* victim = enemy->GetVictim();

        features[SLOT_PRESENT] = 1.0f;
        features[SLOT_ALIVE] = enemy->IsAlive() ? 1.0f : 0.0f;
        features[SLOT_HEALTH] = enemy->GetHealthPct() / 100.0f;
        features[SLOT_DISTANCE] = std::min(1.0f, bot->GetDistance(enemy) / 60.0f);
        features[SLOT_BEARING_SIN] = std::sin(bearing);
        features[SLOT_BEARING_COS] = std::cos(bearing);
        features[SLOT_BEHIND] = enemy->isInBack(bot) ? 1.0f : 0.0f;
        features[SLOT_ATTACKS_BOT] = victim == bot ? 1.0f : 0.0f;
        features[SLOT_ATTACKS_PET] = victim && victim != bot && victim->GetOwnerGUID() == bot->GetGUID() ? 1.0f : 0.0f;
        features[SLOT_CASTING] = enemy->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
        features[SLOT_IN_COMBAT] = enemy->IsInCombat() ? 1.0f : 0.0f;
        features[SLOT_CROWD_CONTROLLED] = Encoding::IsCrowdControlled(enemy) ? 1.0f : 0.0f;
        features[SLOT_CURRENT_TARGET] = slot == view.TargetSlot ? 1.0f : 0.0f;
        features[SLOT_ELITE] = enemy->ToCreature() && enemy->ToCreature()->isElite() ? 1.0f : 0.0f;
        features[SLOT_LEVEL_DIFFERENCE] = (float(enemy->GetLevel()) - float(bot->GetLevel())) / 5.0f;

        alive += enemy->IsAlive() ? 1 : 0;
        inCombat += enemy->IsAlive() && enemy->IsInCombat() ? 1 : 0;
    }

    obs[OBS_ALIVE] = float(alive) / float(PACK_SLOTS);
    obs[OBS_IN_COMBAT] = float(inCombat) / float(PACK_SLOTS);

    std::vector<ActionCatalog::Action> const& tactical = view.L->Catalog().Tactical();
    Encoding::WriteKnownCooldowns(bot, tactical, obs + OBS_GLOBAL_COUNT + PACK_SLOTS * SLOT_FEATURES);
    if (!mask)
        return;

    for (uint32 slot = 0; slot < PACK_SLOTS; ++slot)
        mask[slot] = IsSlotAllowed(view, slot) ? 1 : 0;

    for (uint32 i = 0; i < tactical.size(); ++i)
        mask[ACTION_TACTICAL_FIRST + i] = view.Target && Encoding::IsSpellActionAllowed(view, view.Target, tactical[i])
            ? 1 : 0;
}

void AnimusForge::ClassRole::PackBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (local >= ACTION_TACTICAL_FIRST)
    {
        std::vector<ActionCatalog::Action> const& tactical = view.L->Catalog().Tactical();
        uint32 const index = local - ACTION_TACTICAL_FIRST;
        if (view.Target && index < tactical.size())
            Encoding::ApplySpellAction(view, view.Target, tactical[index], result);
        return;
    }

    if (IsSlotAllowed(view, local))
        Encoding::SelectEnemy(view, local);
}
