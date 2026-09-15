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

#include "CoreBlock.h"
#include "EncoderSupport.h"
#include "Item.h"
#include "JsonWriter.h"
#include "Layout.h"
#include "Player.h"
#include "SpellInfo.h"
#include <algorithm>

namespace
{
    using namespace AnimusForge::ClassRole;

    constexpr std::array<ShapeshiftForm, 13> TRACKED_FORMS =
    {
        FORM_NONE, FORM_CAT, FORM_TREE, FORM_BEAR, FORM_DIREBEAR, FORM_MOONKIN, FORM_SHADOW, FORM_STEALTH,
        FORM_BATTLESTANCE, FORM_DEFENSIVESTANCE, FORM_BERSERKERSTANCE, FORM_METAMORPHOSIS, FORM_GHOSTWOLF
    };

    constexpr float GCD_MS = 1500.0f;
    constexpr float RUNE_COOLDOWN_MS = 10000.0f;
    constexpr float TALENT_POINTS_AT_MAX_LEVEL = 71.0f;

    bool IsActionAllowed(SeatView const& view, uint32 action)
    {
        ActionCatalog::Action const& def = view.L->Catalog().Actions()[action];
        Player* bot = view.Bot;

        switch (def.Type)
        {
            case ActionCatalog::Kind::Noop:
                return true;
            case ActionCatalog::Kind::Soulstone:
                return false;       // a revive (companion and party blocks), never a core action
            case ActionCatalog::Kind::CancelQueued:
                return bot->GetCurrentSpell(CURRENT_MELEE_SPELL) != nullptr;
            case ActionCatalog::Kind::Trinket:
            {
                Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, def.EquipmentSlot);
                SpellInfo const* info = Encoding::TrinketSpell(item);
                return info && !Encoding::CastInProgress(bot) && !bot->HasSpellCooldown(info->Id)
                    && Encoding::CanCast(bot, info, view.Target, item);
            }
            case ActionCatalog::Kind::Spell:
                break;
        }

        return Encoding::IsSpellActionAllowed(view, view.Target, def);
    }
}

AnimusForge::ClassRole::BlockSize AnimusForge::ClassRole::CoreBlock::Size(Layout const& layout) const
{
    uint32 const actions = uint32(layout.Catalog().Actions().size());
    uint32 const talents = uint32(layout.Assets->Talents->Talents().size());
    return { OBS_GLOBAL_COUNT + actions * ACTION_FEATURES + talents + TalentBuilder::TREE_COUNT, actions };
}

uint32 AnimusForge::ClassRole::CoreBlock::TalentObsFirst(Layout const& layout)
{
    return layout.Slice(BlockId::Core).ObsFirst + OBS_GLOBAL_COUNT
        + uint32(layout.Catalog().Actions().size()) * ACTION_FEATURES;
}

uint32 AnimusForge::ClassRole::CoreBlock::TreeObsFirst(Layout const& layout)
{
    return TalentObsFirst(layout) + uint32(layout.Assets->Talents->Talents().size());
}

void AnimusForge::ClassRole::CoreBlock::DescribeManifest(Layout const& layout, JsonWriter& json) const
{
    json.Key("action_features").Value(ACTION_FEATURES);

    json.Key("catalog").Array(layout.Catalog().Actions(), [](JsonWriter& out, ActionCatalog::Action const& action)
    {
        out.BeginObject();
        switch (action.Type)
        {
            case ActionCatalog::Kind::Noop:
                out.Key("kind").Value("noop");
                break;
            case ActionCatalog::Kind::CancelQueued:
                out.Key("kind").Value("cancel_queued");
                break;
            case ActionCatalog::Kind::Trinket:
                out.Key("kind").Value("trinket").Key("slot").Value(action.EquipmentSlot);
                break;
            case ActionCatalog::Kind::Soulstone:
                out.Key("kind").Value("soulstone");
                break;
            case ActionCatalog::Kind::Spell:
                out.Key("kind").Value("spell").Key("first_rank").Value(action.FirstRank)
                    .Key("next_swing").Value(action.NextSwing);
                break;
        }
        out.EndObject();
    });

    json.Key("talents").Array(layout.Assets->Talents->Talents(),
        [](JsonWriter& out, TalentBuilder::Talent const& talent)
    {
        out.BeginArray().Value(talent.TalentId).Value(talent.MaxRank).EndArray();
    });
}

void AnimusForge::ClassRole::CoreBlock::ObserveCharacter(SeatView const& view, float* obs)
{
    Layout const& layout = *view.L;
    float* core = obs + layout.Slice(BlockId::Core).ObsFirst;

    core[OBS_LEVEL] = float(view.Level) / float(DEFAULT_MAX_LEVEL);
    WriteOneHot(PLAYABLE_RACES, view.Race, core + OBS_RACE_FIRST);
    core[OBS_SPEC_FIRST + std::min<uint32>(view.Spec, MAX_SPECS - 1)] = 1.0f;

    if (!view.Build)
        return;

    std::vector<TalentBuilder::Talent> const& talents = layout.Assets->Talents->Talents();
    float* talentObs = obs + TalentObsFirst(layout);
    for (uint32 i = 0; i < talents.size() && i < view.Build->Ranks.size(); ++i)
        talentObs[i] = float(view.Build->Ranks[i]) / float(std::max<uint8>(1, talents[i].MaxRank));

    float* treeObs = obs + TreeObsFirst(layout);
    for (uint32 tree = 0; tree < TalentBuilder::TREE_COUNT; ++tree)
        treeObs[tree] = float(view.Build->TreePoints[tree]) / TALENT_POINTS_AT_MAX_LEVEL;
}

void AnimusForge::ClassRole::CoreBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;
    Unit* target = view.Target;
    ObjectGuid const botGuid = bot->GetGUID();
    uint8 const level = bot->GetLevel();

    obs[OBS_HEALTH] = bot->GetHealthPct() / 100.0f;
    if (uint32 const maxMana = bot->GetMaxPower(POWER_MANA))
        obs[OBS_MANA] = float(bot->GetPower(POWER_MANA)) / float(maxMana);
    obs[OBS_RAGE] = float(bot->GetPower(POWER_RAGE)) / 1000.0f;
    if (uint32 const maxEnergy = bot->GetMaxPower(POWER_ENERGY))
        obs[OBS_ENERGY] = float(bot->GetPower(POWER_ENERGY)) / float(maxEnergy);
    obs[OBS_RUNIC_POWER] = float(bot->GetPower(POWER_RUNIC_POWER)) / 1000.0f;

    if (bot->getClass() == CLASS_DEATH_KNIGHT)
        for (uint8 rune = 0; rune < MAX_RUNES; ++rune)
            obs[OBS_RUNE_FIRST + rune] = 1.0f - std::min(1.0f, float(bot->GetRuneCooldown(rune)) / RUNE_COOLDOWN_MS);

    if (target)
        obs[OBS_COMBO_POINTS] = float(bot->GetComboPoints(target)) / 5.0f;

    ShapeshiftForm const form = bot->GetShapeshiftForm();
    for (uint32 i = 0; i < TRACKED_FORMS.size(); ++i)
        obs[OBS_FORM_FIRST + i] = TRACKED_FORMS[i] == form ? 1.0f : 0.0f;

    obs[OBS_CASTING] = bot->IsNonMeleeSpellCast(false, false, true) ? 1.0f : 0.0f;
    obs[OBS_QUEUED_NEXT_SWING] = bot->GetCurrentSpell(CURRENT_MELEE_SPELL) ? 1.0f : 0.0f;

    for (auto const& [index, attack] : { std::pair{ OBS_MAIN_HAND_SWING, BASE_ATTACK },
        std::pair{ OBS_OFF_HAND_SWING, OFF_ATTACK }, std::pair{ OBS_RANGED_SWING, RANGED_ATTACK } })
    {
        if (uint32 const attackTime = bot->GetAttackTime(attack))
            obs[index] = std::clamp(float(bot->getAttackTimer(attack)) / float(attackTime), 0.0f, 1.0f);
    }

    obs[OBS_MAIN_HAND_SPEED] = float(bot->GetAttackTime(BASE_ATTACK)) / 4000.0f;
    if (target)
    {
        obs[OBS_TARGET_HEALTH] = target->GetHealthPct() / 100.0f;
        obs[OBS_TARGET_DISTANCE] = std::min(1.0f, bot->GetDistance(target) / 40.0f);
        obs[OBS_IN_MELEE_FRONT] = bot->IsWithinMeleeRange(target) && bot->HasInArc(2 * float(M_PI) / 3, target)
            ? 1.0f : 0.0f;
    }

    obs[OBS_ATTACK_POWER] = bot->GetTotalAttackPowerValue(BASE_ATTACK) / (100.0f + 50.0f * level);
    obs[OBS_SPELL_POWER] = float(bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC)) / (50.0f + 30.0f * level);
    obs[OBS_MELEE_CRIT] = bot->GetFloatValue(PLAYER_CRIT_PERCENTAGE) / 100.0f;

    float spellCrit = 0.0f;
    for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
        spellCrit = std::max(spellCrit, bot->GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + school));
    obs[OBS_SPELL_CRIT] = spellCrit / 100.0f;

    obs[OBS_MELEE_HASTE] = bot->GetRatingBonusValue(CR_HASTE_MELEE) / 100.0f;
    obs[OBS_SPELL_HASTE] = bot->GetRatingBonusValue(CR_HASTE_SPELL) / 100.0f;
    obs[OBS_MELEE_HIT] = bot->GetRatingBonusValue(CR_HIT_MELEE) / 100.0f;
    obs[OBS_SPELL_HIT] = bot->GetRatingBonusValue(CR_HIT_SPELL) / 100.0f;
    obs[OBS_EXPERTISE] = float(bot->GetUInt32Value(PLAYER_EXPERTISE)) / 30.0f;
    obs[OBS_ARMOR_PENETRATION] = bot->GetRatingBonusValue(CR_ARMOR_PENETRATION) / 100.0f;
    obs[OBS_LAST_STEP_DAMAGE] = view.LastStepDamage;
    obs[OBS_LAST_STEP_POWER_DELTA] = view.LastStepPowerDelta;

    std::vector<ActionCatalog::Action> const& actions = view.L->Catalog().Actions();
    for (uint32 action = 0; action < actions.size(); ++action)
    {
        SpellInfo const* info = nullptr;
        if (actions[action].Type == ActionCatalog::Kind::Spell)
            info = ActionCatalog::KnownRank(bot, actions[action].FirstRank);
        else if (actions[action].Type == ActionCatalog::Kind::Trinket)
            info = Encoding::TrinketSpell(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, actions[action].EquipmentSlot));

        if (info)
        {
            float* features = obs + OBS_GLOBAL_COUNT + action * ACTION_FEATURES;
            float stacks = 0.0f;
            features[0] = 1.0f;
            features[1] = Encoding::CooldownFraction(bot, info);
            features[2] = target ? Encoding::AuraFraction(target, info->Id, botGuid, stacks) : 0.0f;
            features[3] = Encoding::AuraFraction(bot, info->Id, botGuid, stacks);
            features[4] = stacks;

            if (!obs[OBS_GCD] && info->StartRecoveryTime)
                obs[OBS_GCD] = std::min(1.0f, float(bot->GetGlobalCooldownMgr().GetGlobalCooldown(info)) / GCD_MS);
        }

        if (action > 0)
            mask[action] = IsActionAllowed(view, action) ? 1 : 0;
    }
}

void AnimusForge::ClassRole::CoreBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    std::vector<ActionCatalog::Action> const& catalog = view.L->Catalog().Actions();
    if (local == 0 || local >= catalog.size())
        return;

    Player* bot = view.Bot;
    ActionCatalog::Action const& def = catalog[local];
    switch (def.Type)
    {
        case ActionCatalog::Kind::Noop:
        case ActionCatalog::Kind::Soulstone:
            return;
        case ActionCatalog::Kind::CancelQueued:
            if (bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
                bot->InterruptSpell(CURRENT_MELEE_SPELL);
            return;
        case ActionCatalog::Kind::Trinket:
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, def.EquipmentSlot);
            SpellInfo const* info = Encoding::TrinketSpell(item);
            if (!info || bot->HasSpellCooldown(info->Id))
                return;

            bot->CastItemUseSpell(item, Encoding::TargetsFor(info, bot, view.Target), 1, 0);
            if (bot->HasSpellCooldown(info->Id))
                ++result.TrinketUses;
            return;
        }
        case ActionCatalog::Kind::Spell:
            break;
    }

    Encoding::ApplySpellAction(view, view.Target, def, result);
}
