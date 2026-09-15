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

#include "WarriorDummy20Scenario.h"
#include "BotAccounts.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Env.h"
#include "ForgeBotFactory.h"
#include "ForgeConfig.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellChecks.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TrainingDummy.h"
#include <algorithm>
#include <cstring>

namespace
{
    using namespace AnimusForge::SpellChecks;

    constexpr uint8 BOT_LEVEL = 20;

    /// Talent points at BOT_LEVEL: one per level from 10.
    constexpr uint32 BOT_TALENT_POINTS = BOT_LEVEL - 9;

    constexpr uint8 ARMS_TAB_PAGE = 0;
    constexpr uint32 CLASS_MASK_WARRIOR = 1 << (CLASS_WARRIOR - 1);

    enum WarriorDummy20Spells : uint32
    {
        SPELL_HEROIC_STRIKE_RANK_3      = 285,
        SPELL_CLEAVE_RANK_1             = 845,
        SPELL_REND_RANK_3               = 6547,
        SPELL_THUNDER_CLAP_RANK_2       = 8198,
        SPELL_BATTLE_SHOUT_RANK_2       = 5242,
        SPELL_BLOODRAGE                 = 2687,
        SPELL_BLOODRAGE_RAGE_AURA       = 29131,    // triggered by Bloodrage: rage over 10 s
        SPELL_OVERPOWER                 = 7384,
        SPELL_HAMSTRING                 = 1715,
        SPELL_MOCKING_BLOW              = 694,
    };

    /// Every spell the warrior trainers teach up to level 20 (trainer_spell, Requirement = warrior
    /// class, learn-spells resolved to the spells they teach). Heroic Strike rank 1 comes from the
    /// starting skills.
    constexpr std::array<uint32, 25> TRAINER_SPELLS =
    {
        6673,   // Battle Shout (Rank 1)            1
        100,    // Charge (Rank 1)                  4
        772,    // Rend (Rank 1)                    4
        3127,   // Parry                            6
        6343,   // Thunder Clap (Rank 1)            6
        34428,  // Victory Rush                     6
        284,    // Heroic Strike (Rank 2)           8
        1715,   // Hamstring                        8
        2687,   // Bloodrage                        10
        6546,   // Rend (Rank 2)                    10
        72,     // Shield Bash                      12
        5242,   // Battle Shout (Rank 2)            12
        7384,   // Overpower                        12
        1160,   // Demoralizing Shout (Rank 1)      14
        6572,   // Revenge (Rank 1)                 14
        285,    // Heroic Strike (Rank 3)           16
        694,    // Mocking Blow                     16
        2565,   // Shield Block                     16
        676,    // Disarm                           18
        8198,   // Thunder Clap (Rank 2)            18
        674,    // Dual Wield                       20
        845,    // Cleave (Rank 1)                  20
        6547,   // Rend (Rank 3)                    20
        12678,  // Stance Mastery                   20
        20230,  // Retaliation                      20
    };

    struct GearPiece
    {
        uint8 Slot;
        uint32 Item;
    };

    /// A coherent strength/stamina mail kit of level 16-20 greens and dungeon blues: not best in
    /// slot, but what a well-equipped level 20 warrior plausibly wears. No helm or trinkets.
    constexpr std::array<GearPiece, 12> GEAR =
    {{
        { EQUIPMENT_SLOT_NECK,      30419 },    // Brilliant Necklace
        { EQUIPMENT_SLOT_SHOULDERS, 3231 },     // Cutthroat Pauldrons
        { EQUIPMENT_SLOT_CHEST,     6627 },     // Mutant Scale Breastplate
        { EQUIPMENT_SLOT_WAIST,     6460 },     // Cobrahn's Grasp
        { EQUIPMENT_SLOT_LEGS,      14748 },    // Hulking Leggings
        { EQUIPMENT_SLOT_FEET,      14742 },    // Hulking Boots
        { EQUIPMENT_SLOT_WRISTS,    2868 },     // Patterned Bronze Bracers
        { EQUIPMENT_SLOT_HANDS,     12994 },    // Thorbia's Gauntlets
        { EQUIPMENT_SLOT_FINGER1,   1076 },     // Defias Renegade Ring
        { EQUIPMENT_SLOT_FINGER2,   12054 },    // Demon Band
        { EQUIPMENT_SLOT_BACK,      6340 },     // Fenrus' Hide
        { EQUIPMENT_SLOT_MAINHAND,  6641 },     // Haunting Blade (two-handed sword)
    }};

    /// Remaining cooldown of a spell as a fraction of its full cooldown.
    float SpellCooldownFraction(Player const* bot, uint32 spellId)
    {
        return CooldownFraction(bot, sSpellMgr->GetSpellInfo(spellId));
    }
}

AnimusForge::WarriorDummy20Scenario::WarriorDummy20Scenario(ForgeConfig const& config)
    : _spawnMapId(config.SpawnMapId), _spawnPoint(config.SpawnPosition),
    _hsRageThreshold(config.WarriorDummy20HsRageThreshold), _talentPoints(BOT_TALENT_POINTS)
{
    _actions.resize(ACTION_FIXED_COUNT);
    _actions[ACTION_NOOP] = { "noop" };
    _actions[ACTION_HEROIC_STRIKE] = { "heroic_strike", SPELL_HEROIC_STRIKE_RANK_3, true };
    _actions[ACTION_CLEAVE] = { "cleave", SPELL_CLEAVE_RANK_1, true };
    _actions[ACTION_CANCEL_QUEUED] = { "cancel_queued" };
    _actions[ACTION_REND] = { "rend", SPELL_REND_RANK_3 };
    _actions[ACTION_THUNDER_CLAP] = { "thunder_clap", SPELL_THUNDER_CLAP_RANK_2 };
    _actions[ACTION_BATTLE_SHOUT] = { "battle_shout", SPELL_BATTLE_SHOUT_RANK_2 };
    _actions[ACTION_BLOODRAGE] = { "bloodrage", SPELL_BLOODRAGE };
    _actions[ACTION_OVERPOWER] = { "overpower", SPELL_OVERPOWER };
    _actions[ACTION_HAMSTRING] = { "hamstring", SPELL_HAMSTRING };
    _actions[ACTION_MOCKING_BLOW] = { "mocking_blow", SPELL_MOCKING_BLOW };

    BuildTalentTable(_talentPoints);

    std::vector<uint8> ranks(_talents.size(), 0);
    EnumerateBuilds(0, _talentPoints, ranks);

    if (_builds.empty())
    {
        LOG_ERROR("module.animus", "warrior_dummy_20: no valid Arms build spends {} points; episodes run untalented",
            _talentPoints);
        _builds.emplace_back(_talents.size(), 0);
    }

    // Active abilities granted by reachable talents become actions, masked when not learned.
    for (size_t slot = 0; slot < _talents.size(); ++slot)
    {
        TalentEntry const* entry = sTalentStore.LookupEntry(_talents[slot].TalentId);
        SpellInfo const* rankOne = sSpellMgr->GetSpellInfo(_talents[slot].RankSpells[0]);
        if (!entry || !rankOne || !entry->addToSpellBook || rankOne->IsPassive())
            continue;

        SpellAction action;
        action.Name = "talent_" + std::to_string(_talents[slot].TalentId);
        action.SpellId = _talents[slot].RankSpells[0];
        action.NextSwing = rankOne->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE);
        action.TalentIndex = int32(slot);
        _actions.push_back(action);
    }

    _data.resize(config.Envs);
    for (EnvData& data : _data)
        data.Casts.assign(_actions.size(), 0);

    _spec.AgentsPerEnv = 1;
    _spec.ObsDim = OBS_FIXED_COUNT + uint32(_talents.size());
    _spec.StateDim = _spec.ObsDim;      // one agent: the critic sees exactly what the actor sees
    _spec.NumActions = uint32(_actions.size());
    _spec.EpisodeInfoDim = uint32(EpisodeInfoNames().size());

    LOG_INFO("module.animus", "warrior_dummy_20: level {}, {} talent points, {} reachable Arms talents, {} talent "
        "builds, {} talent actions", BOT_LEVEL, _talentPoints, _talents.size(), _builds.size(),
        _actions.size() - ACTION_FIXED_COUNT);
}

AnimusForge::ScenarioSpec AnimusForge::WarriorDummy20Scenario::Spec() const
{
    return _spec;
}

void AnimusForge::WarriorDummy20Scenario::BuildTalentTable(uint32 talentPoints)
{
    uint32 armsTab = 0;
    for (uint32 i = 0; i < sTalentTabStore.GetNumRows(); ++i)
        if (TalentTabEntry const* tab = sTalentTabStore.LookupEntry(i))
            if ((tab->ClassMask & CLASS_MASK_WARRIOR) && tab->tabpage == ARMS_TAB_PAGE)
                armsTab = tab->TalentTabID;

    // A point in row r needs 5r points already spent in the tree, so the last reachable row is the
    // one whose requirement still leaves a point to spend.
    uint32 const maxRow = talentPoints ? (talentPoints - 1) / MAX_TALENT_RANK : 0;

    std::vector<TalentEntry const*> entries;
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        if (TalentEntry const* entry = sTalentStore.LookupEntry(i))
            if (entry->TalentTab == armsTab && entry->Row <= maxRow)
                entries.push_back(entry);

    std::sort(entries.begin(), entries.end(), [](TalentEntry const* a, TalentEntry const* b)
    {
        return a->Row != b->Row ? a->Row < b->Row : a->Col < b->Col;
    });

    // Learning order: row by row, and a prerequisite before any talent that depends on it. A talent
    // whose prerequisite lies outside the reachable rows is never placed, so it is excluded.
    std::vector<TalentEntry const*> ordered;
    std::vector<bool> placed(entries.size(), false);
    for (bool progress = true; progress;)
    {
        progress = false;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (placed[i])
                continue;

            bool const ready = !entries[i]->DependsOn || std::any_of(ordered.begin(), ordered.end(),
                [&](TalentEntry const* e) { return e->TalentID == entries[i]->DependsOn; });

            if (ready)
            {
                ordered.push_back(entries[i]);
                placed[i] = true;
                progress = true;
            }
        }
    }

    for (TalentEntry const* entry : ordered)
    {
        TalentSlot slot;
        slot.TalentId = entry->TalentID;
        slot.Row = entry->Row;
        for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
        {
            slot.RankSpells[rank] = entry->RankID[rank];
            if (entry->RankID[rank])
                slot.MaxRank = rank + 1;
        }

        if (entry->DependsOn)
        {
            auto const dep = std::find_if(_talents.begin(), _talents.end(),
                [&](TalentSlot const& s) { return s.TalentId == entry->DependsOn; });
            slot.DependsOnSlot = int32(dep - _talents.begin());
            // The core accepts the prerequisite at rank index DependsOnRank or higher (Player::LearnTalent).
            slot.DependsOnRank = uint8(entry->DependsOnRank + 1);
        }

        _talents.push_back(slot);
    }
}

void AnimusForge::WarriorDummy20Scenario::EnumerateBuilds(size_t slot, uint32 remaining, std::vector<uint8>& ranks)
{
    if (slot == _talents.size())
    {
        if (!remaining && IsValidBuild(ranks))
            _builds.push_back(ranks);
        return;
    }

    for (uint8 rank = 0; rank <= std::min<uint32>(_talents[slot].MaxRank, remaining); ++rank)
    {
        ranks[slot] = rank;
        EnumerateBuilds(slot + 1, remaining - rank, ranks);
    }

    ranks[slot] = 0;
}

bool AnimusForge::WarriorDummy20Scenario::IsValidBuild(std::vector<uint8> const& ranks) const
{
    // Mirrors Player::LearnTalent: row r needs Row * MAX_TALENT_RANK points spent in the tree, and a
    // talent with a prerequisite needs it at DependsOnRank. Learned row by row, points in lower rows
    // are exactly what has been spent when a row's first point goes in.
    std::vector<uint32> rowPoints;
    for (size_t i = 0; i < _talents.size(); ++i)
    {
        if (rowPoints.size() <= _talents[i].Row)
            rowPoints.resize(_talents[i].Row + 1, 0);
        rowPoints[_talents[i].Row] += ranks[i];
    }

    for (size_t i = 0; i < _talents.size(); ++i)
    {
        if (!ranks[i])
            continue;

        uint32 below = 0;
        for (uint32 row = 0; row < _talents[i].Row; ++row)
            below += rowPoints[row];

        if (below < _talents[i].Row * MAX_TALENT_RANK)
            return false;

        if (_talents[i].DependsOnSlot >= 0 && ranks[_talents[i].DependsOnSlot] < _talents[i].DependsOnRank)
            return false;
    }

    return true;
}

void AnimusForge::WarriorDummy20Scenario::ApplyTalentBuild(Player* bot, uint32 build) const
{
    // No database access: resetTalents does not save, and never-saved talents are simply erased.
    bot->resetTalents(true);

    std::vector<uint8> const& ranks = _builds[build];
    for (size_t slot = 0; slot < _talents.size(); ++slot)
        if (ranks[slot])
            bot->LearnTalent(_talents[slot].TalentId, ranks[slot] - 1);

    if (bot->GetFreeTalentPoints())
        LOG_ERROR("module.animus", "Talent build {} left {} points unspent on {}; the build table and "
            "Player::LearnTalent disagree", build, bot->GetFreeTalentPoints(), bot->GetName());
}

bool AnimusForge::WarriorDummy20Scenario::EquipGear(Player* bot) const
{
    // Drop the starting outfit.
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

    for (GearPiece const& piece : GEAR)
    {
        uint16 dest = 0;
        InventoryResult const result = bot->CanEquipNewItem(piece.Slot, dest, piece.Item, false);
        if (result != EQUIP_ERR_OK)
        {
            LOG_ERROR("module.animus", "{} cannot equip item {} in slot {} (inventory result {})", bot->GetName(),
                piece.Item, piece.Slot, uint32(result));
            return false;
        }

        bot->EquipNewItem(dest, piece.Item, true);
    }

    return true;
}

void AnimusForge::WarriorDummy20Scenario::LearnKit(Player* bot) const
{
    for (uint32 spellId : TRAINER_SPELLS)
        if (!bot->HasSpell(spellId))
            bot->learnSpell(spellId);
}

bool AnimusForge::WarriorDummy20Scenario::Setup(Env& env)
{
    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Forgearms{}", env.Index);
    spec.Race = RACE_HUMAN;
    spec.Class = CLASS_WARRIOR;
    spec.Gender = GENDER_MALE;
    spec.Level = BOT_LEVEL;
    spec.AccountId = BotAccounts::Seat(env.Index, 0, 0);

    Player* bot = BotFactory::Create(spec);
    if (!bot)
        return false;

    // The talent build table assumes the level's standard point count; a talent rate config would
    // make every build under- or over-spend.
    if (bot->GetFreeTalentPoints() != _talentPoints)
    {
        LOG_ERROR("module.animus", "{} has {} talent points at level {}, the scenario expects {}", bot->GetName(),
            bot->GetFreeTalentPoints(), BOT_LEVEL, _talentPoints);
        return false;
    }

    LearnKit(bot);
    if (!EquipGear(bot))
        return false;

    bot->SetFullHealth();

    Map* map = BotFactory::PlaceInNewInstance(bot, _spawnMapId, _spawnPoint);
    if (!map)
        return false;

    env.MapId = map->GetId();
    env.InstanceId = map->GetInstanceId();
    env.Bots = { bot->GetGUID() };

    // A first login casts the class's start spells (playercreateinfo_cast_spell); Create does not.
    bot->CastSpell(bot, SPELL_BATTLE_STANCE, true);

    TrainingDummy::ClearSpawnArea(bot);

    Creature* dummy = TrainingDummy::Spawn(bot, map);
    if (!dummy)
        return false;

    env.Targets = { dummy->GetGUID() };

    EnvData& data = _data[env.Index];
    data.Home = _spawnPoint;
    data.DamageScale = std::max(1.0f, bot->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE));

    return true;
}

void AnimusForge::WarriorDummy20Scenario::Reset(Env& env)
{
    EnvData& data = _data[env.Index];
    data.LastRage = 0;
    data.LastStepDamage = 0.0f;
    data.LastStepRageDelta = 0.0f;
    data.Build = urand(0, uint32(_builds.size()) - 1);
    std::fill(data.Casts.begin(), data.Casts.end(), 0);

    Player* bot = env.FindBot(0);
    Creature* dummy = env.FindTarget(0);
    if (!bot || !dummy)
    {
        LOG_ERROR("module.animus", "Env {} lost its bot or dummy", env.Index);
        return;
    }

    if (!bot->IsAlive())
        bot->ResurrectPlayer(1.0f);

    bot->InterruptSpell(CURRENT_MELEE_SPELL);
    bot->InterruptNonMeleeSpells(false);
    bot->AttackStop();
    bot->CombatStop(true);
    dummy->CombatStop(true);

    // Clear everything the last episode applied: the bot's debuffs on the dummy, and the bot's own
    // temporary buffs. Passive auras (talents, racials, proficiencies) and the stance stay.
    ObjectGuid const botGuid = bot->GetGUID();
    dummy->RemoveAppliedAuras([botGuid](AuraApplication const* app)
    {
        return app->GetBase()->GetCasterGUID() == botGuid;
    });
    bot->RemoveAppliedAuras([](AuraApplication const* app)
    {
        Aura const* aura = app->GetBase();
        return !aura->IsPassive() && !aura->GetSpellInfo()->HasAura(SPELL_AURA_MOD_SHAPESHIFT);
    });

    ApplyTalentBuild(bot, data.Build);

    bot->RemoveAllSpellCooldown();
    if (SpellInfo const* gcdSpell = sSpellMgr->GetSpellInfo(SPELL_REND_RANK_3))
        bot->GetGlobalCooldownMgr().CancelGlobalCooldown(gcdSpell);
    bot->ClearAllReactives();
    bot->ClearComboPoints();

    bot->SetFullHealth();
    bot->SetPower(POWER_RAGE, 0);
    dummy->SetFullHealth();

    if (bot->GetExactDist(&data.Home) > 0.1f)
        bot->UpdatePosition(data.Home, true);

    bot->SetOrientation(bot->GetAngle(dummy));
    bot->Attack(dummy, true);

    // Random swing phase so episodes do not all start on the same swing boundary.
    bot->setAttackTimer(BASE_ATTACK, int32(urand(0, bot->GetAttackTime(BASE_ATTACK))));
}

SpellInfo const* AnimusForge::WarriorDummy20Scenario::ResolveSpell(Player const* bot, int32 action) const
{
    SpellAction const& spellAction = _actions[action];
    if (spellAction.TalentIndex < 0)
        return sSpellMgr->GetSpellInfo(spellAction.SpellId);

    TalentSlot const& talent = _talents[spellAction.TalentIndex];
    for (int32 rank = int32(talent.MaxRank) - 1; rank >= 0; --rank)
        if (talent.RankSpells[rank] && bot->HasSpell(talent.RankSpells[rank]))
            return sSpellMgr->GetSpellInfo(talent.RankSpells[rank]);

    return nullptr;
}

bool AnimusForge::WarriorDummy20Scenario::CanCast(Player* bot, SpellInfo const* info, Unit* target)
{
    if (!info || !bot->HasActiveSpell(info->Id))
        return false;

    // The core's own cast validation, without casting: cooldown, GCD, rage, stance, range, facing,
    // and reactive requirements such as Overpower's dodge window.
    SpellCastTargets targets;
    targets.SetUnitTarget(info->NeedsExplicitUnitTarget() ? target : bot);
    return CheckCast(bot, info, targets);
}

bool AnimusForge::WarriorDummy20Scenario::IsActionAllowed(Player* bot, Unit* target, int32 action) const
{
    switch (action)
    {
        case ACTION_NOOP:
            return true;
        case ACTION_CANCEL_QUEUED:
            return bot->GetCurrentSpell(CURRENT_MELEE_SPELL) != nullptr;
        default:
            break;
    }

    // One on-next-swing ability at a time.
    if (_actions[action].NextSwing && bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
        return false;

    return CanCast(bot, ResolveSpell(bot, action), target);
}

void AnimusForge::WarriorDummy20Scenario::ApplyActions(Env& env, int32 const* actions)
{
    Player* bot = env.FindBot(0);
    Creature* dummy = env.FindTarget(0);
    int32 const action = actions[0];
    if (!bot || !dummy || action <= ACTION_NOOP || action >= int32(_actions.size()))
        return;

    if (action == ACTION_CANCEL_QUEUED)
    {
        if (bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
            bot->InterruptSpell(CURRENT_MELEE_SPELL);
        return;
    }

    if (_actions[action].NextSwing && bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
        return;

    SpellInfo const* info = ResolveSpell(bot, action);
    if (!info || !bot->HasActiveSpell(info->Id))
        return;

    // Same path as CMSG_CAST_SPELL. prepare() runs the full cast validation again, so a masked action
    // from a misbehaving client simply fails. The spell owns and frees itself.
    SpellCastTargets targets;
    targets.SetUnitTarget(info->NeedsExplicitUnitTarget() ? static_cast<Unit*>(dummy) : bot);

    Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
    if (spell->prepare(&targets) == SPELL_CAST_OK)
        ++_data[env.Index].Casts[action];
}

void AnimusForge::WarriorDummy20Scenario::Observe(Env& env, float* obs, float* state, uint8* mask)
{
    std::fill(obs, obs + _spec.ObsDim, 0.0f);
    std::fill(mask, mask + _spec.NumActions, 0);
    mask[ACTION_NOOP] = 1;

    EnvData const& data = _data[env.Index];
    Player* bot = env.FindBot(0);
    Creature* dummy = env.FindTarget(0);

    if (bot && dummy)
    {
        ObjectGuid const botGuid = bot->GetGUID();
        float const maxRage = float(std::max<uint32>(1, bot->GetMaxPower(POWER_RAGE)));
        float const attackTime = float(std::max<uint32>(1, bot->GetAttackTime(BASE_ATTACK)));
        Spell const* queued = bot->GetCurrentSpell(CURRENT_MELEE_SPELL);
        uint32 const queuedId = queued ? queued->m_spellInfo->Id : 0;

        obs[OBS_RAGE] = float(bot->GetPower(POWER_RAGE)) / maxRage;
        obs[OBS_SWING_REMAINING] = float(std::max(0, bot->getAttackTimer(BASE_ATTACK))) / attackTime;
        obs[OBS_WEAPON_SPEED] = attackTime / 4000.0f;
        obs[OBS_HEROIC_STRIKE_QUEUED] = queuedId == SPELL_HEROIC_STRIKE_RANK_3 ? 1.0f : 0.0f;
        obs[OBS_CLEAVE_QUEUED] = queuedId == SPELL_CLEAVE_RANK_1 ? 1.0f : 0.0f;
        obs[OBS_IN_MELEE_FRONT] = bot->IsWithinMeleeRange(dummy) && bot->HasInArc(2 * float(M_PI) / 3, dummy)
            ? 1.0f : 0.0f;

        if (SpellInfo const* gcdSpell = sSpellMgr->GetSpellInfo(SPELL_REND_RANK_3))
            obs[OBS_GCD_REMAINING] = std::min(1.0f, float(bot->GetGlobalCooldownMgr().GetGlobalCooldown(gcdSpell))
                / GCD_MS);

        obs[OBS_THUNDER_CLAP_COOLDOWN] = SpellCooldownFraction(bot, SPELL_THUNDER_CLAP_RANK_2);
        obs[OBS_BLOODRAGE_COOLDOWN] = SpellCooldownFraction(bot, SPELL_BLOODRAGE);
        obs[OBS_OVERPOWER_COOLDOWN] = SpellCooldownFraction(bot, SPELL_OVERPOWER);
        obs[OBS_MOCKING_BLOW_COOLDOWN] = SpellCooldownFraction(bot, SPELL_MOCKING_BLOW);

        // A dodge gives the warrior a combo point on the target for the Overpower window
        // (Unit::StartReactiveTimer(REACTIVE_OVERPOWER)); it is cleared when the window closes.
        obs[OBS_OVERPOWER_WINDOW] = bot->GetComboPoints(dummy) ? 1.0f : 0.0f;

        obs[OBS_REND_REMAINING] = AuraFraction(dummy, SPELL_REND_RANK_3, botGuid);
        obs[OBS_THUNDER_CLAP_REMAINING] = AuraFraction(dummy, SPELL_THUNDER_CLAP_RANK_2, botGuid);
        obs[OBS_HAMSTRING_REMAINING] = AuraFraction(dummy, SPELL_HAMSTRING, botGuid);
        obs[OBS_BATTLE_SHOUT_REMAINING] = AuraFraction(bot, SPELL_BATTLE_SHOUT_RANK_2, botGuid);
        obs[OBS_BLOODRAGE_REMAINING] = AuraFraction(bot, SPELL_BLOODRAGE_RAGE_AURA, botGuid);
        obs[OBS_LAST_STEP_DAMAGE] = data.LastStepDamage;
        obs[OBS_LAST_STEP_RAGE_DELTA] = data.LastStepRageDelta;

        for (int32 action = ACTION_NOOP + 1; action < int32(_actions.size()); ++action)
            mask[action] = IsActionAllowed(bot, dummy, action) ? 1 : 0;
    }

    std::vector<uint8> const& ranks = _builds[data.Build];
    for (size_t slot = 0; slot < _talents.size(); ++slot)
        obs[OBS_FIXED_COUNT + slot] = float(ranks[slot]) / float(std::max<uint8>(1, _talents[slot].MaxRank));

    std::memcpy(state, obs, _spec.ObsDim * sizeof(float));
}

void AnimusForge::WarriorDummy20Scenario::Reward(Env& env, float* reward)
{
    EnvData& data = _data[env.Index];

    float const scaled = float(env.StepStats[0].Damage) / data.DamageScale;
    reward[0] = scaled;
    data.LastStepDamage = scaled;

    if (Player* bot = env.FindBot(0))
    {
        uint32 const rage = bot->GetPower(POWER_RAGE);
        float const maxRage = float(std::max<uint32>(1, bot->GetMaxPower(POWER_RAGE)));
        data.LastStepRageDelta = (float(rage) - float(data.LastRage)) / maxRage;
        data.LastRage = rage;
    }
}

void AnimusForge::WarriorDummy20Scenario::EpisodeInfo(Env const& env, float* info) const
{
    AgentStats const& stats = env.EpisodeStats[0];
    EnvData const& data = _data[env.Index];
    float const seconds = std::max(0.001f, float(env.EpisodeElapsedMs) / 1000.0f);

    info[INFO_DAMAGE] = float(stats.Damage);
    info[INFO_DPS] = float(stats.Damage) / seconds;
    info[INFO_WHITE_HITS] = float(stats.WhiteHits);
    info[INFO_SPECIAL_HITS] = float(stats.SpecialHits);
    info[INFO_WHITE_DAMAGE] = float(stats.WhiteDamage);
    info[INFO_SPECIAL_DAMAGE] = float(stats.SpecialDamage);
    info[INFO_TALENT_BUILD] = float(data.Build);

    uint32 column = INFO_CASTS_FIRST;
    for (int32 action = ACTION_NOOP + 1; action < int32(_actions.size()); ++action)
        if (action != ACTION_CANCEL_QUEUED)
            info[column++] = float(data.Casts[action]);
}

std::vector<std::string> AnimusForge::WarriorDummy20Scenario::EpisodeInfoNames() const
{
    std::vector<std::string> names =
        { "damage", "dps", "white_hits", "special_hits", "white_damage", "special_damage", "talent_build" };

    for (int32 action = ACTION_NOOP + 1; action < int32(_actions.size()); ++action)
        if (action != ACTION_CANCEL_QUEUED)
            names.push_back("casts_" + _actions[action].Name);

    return names;
}

bool AnimusForge::WarriorDummy20Scenario::ScriptedAction(std::string const& policy, float const* obs,
    uint8 const* mask, uint16 /*layout*/, int32& action) const
{
    float const rage = obs[OBS_RAGE] * 100.0f;  // max rage is 100 (stored as 1000 tenths)

    if (policy == "white_only")
    {
        action = mask[ACTION_CANCEL_QUEUED] ? ACTION_CANCEL_QUEUED : ACTION_NOOP;
        return true;
    }

    if (policy == "hs_at_threshold")
    {
        action = mask[ACTION_HEROIC_STRIKE] && rage >= float(_hsRageThreshold) ? ACTION_HEROIC_STRIKE : ACTION_NOOP;
        return true;
    }

    if (policy == "rotation")
    {
        // A conventional levelling priority: keep Battle Shout and Rend up, use Bloodrage and
        // Overpower on cooldown, Thunder Clap and Mocking Blow for damage, and dump spare rage into
        // Heroic Strike.
        if (mask[ACTION_BATTLE_SHOUT] && obs[OBS_BATTLE_SHOUT_REMAINING] < 0.1f)
            action = ACTION_BATTLE_SHOUT;
        else if (mask[ACTION_BLOODRAGE])
            action = ACTION_BLOODRAGE;
        else if (mask[ACTION_OVERPOWER])
            action = ACTION_OVERPOWER;
        else if (mask[ACTION_REND] && obs[OBS_REND_REMAINING] <= 0.0f)
            action = ACTION_REND;
        else if (mask[ACTION_THUNDER_CLAP])
            action = ACTION_THUNDER_CLAP;
        else if (mask[ACTION_MOCKING_BLOW])
            action = ACTION_MOCKING_BLOW;
        else if (mask[ACTION_HEROIC_STRIKE] && rage >= float(_hsRageThreshold))
            action = ACTION_HEROIC_STRIKE;
        else
            action = ACTION_NOOP;
        return true;
    }

    return false;
}

void AnimusForge::WarriorDummy20Scenario::Teardown(Env& env)
{
    if (Creature* dummy = env.FindTarget(0))
        dummy->DespawnOrUnsummon();

    if (Player* bot = env.FindBot(0))
        BotFactory::Destroy(bot);

    env.Bots.clear();
    env.Targets.clear();
}
