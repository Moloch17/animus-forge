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

#include "CompanionOwner.h"
#include "ActionCatalog.h"
#include "Creature.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr std::array<uint8, 10> PLAYABLE_RACES =
    {
        RACE_HUMAN, RACE_ORC, RACE_DWARF, RACE_NIGHTELF, RACE_UNDEAD_PLAYER, RACE_TAUREN, RACE_GNOME, RACE_TROLL,
        RACE_BLOODELF, RACE_DRAENEI
    };

    enum OwnerSpells : uint32
    {
        SPELL_BATTLE_STANCE     = 2457,
    };

    constexpr uint32 OWNER_MOVE_POINT_ID = 2;
    constexpr uint32 CHASE_REPATH_MS = 1000;
    constexpr uint32 SPELL_MIN_MS = 2000;
    constexpr uint32 SPELL_MAX_MS = 4000;
    constexpr uint32 WANDER_MIN_MS = 6000;
    constexpr uint32 WANDER_MAX_MS = 12000;
    constexpr uint32 REGEN_INTERVAL_MS = 1000;
    constexpr float REGEN_FRACTION = 0.04f;     // of max health and mana per second, out of combat
    constexpr float WANDER_MIN_DISTANCE = 8.0f;
    constexpr float WANDER_MAX_DISTANCE = 20.0f;
    constexpr float WANDER_LEASH = 30.0f;       // never wander further than this from home
    constexpr uint8 DEATH_KNIGHT_START_LEVEL = 55;
}

AnimusForge::CompanionOwner::Templates const& AnimusForge::CompanionOwner::Templates::Instance()
{
    static Templates const templates;
    return templates;
}

AnimusForge::CompanionOwner::Templates::Templates()
{
    for (ClassRoleProfile const& profile : ClassRoleProfiles())
    {
        if (profile.PlayRole != Role::Dps || _byClass.contains(profile.Class))
            continue;

        Template& entry = _byClass[profile.Class];
        entry.Profile = &profile;
        for (uint8 race : PLAYABLE_RACES)
            if (sObjectMgr->GetPlayerInfo(race, profile.Class))
                entry.Races.push_back(race);

        entry.Kit = std::make_unique<ClassKit>(profile.Class);
        entry.Talents = std::make_unique<TalentBuilder>(profile.Class);
        entry.Gear = std::make_unique<GearBuilder>(profile, *entry.Kit);
    }

    LOG_INFO("module.animus", "Companion owners: {} class templates", _byClass.size());
}

AnimusForge::CompanionOwner::Template const* AnimusForge::CompanionOwner::Templates::ForClass(uint8 playerClass) const
{
    auto const itr = _byClass.find(playerClass);
    return itr != _byClass.end() ? &itr->second : nullptr;
}

std::vector<uint8> AnimusForge::CompanionOwner::Templates::ClassesForLevel(uint8 level) const
{
    std::vector<uint8> classes;
    for (auto const& [playerClass, entry] : _byClass)
        if (!entry.Races.empty() && (playerClass != CLASS_DEATH_KNIGHT || level >= DEATH_KNIGHT_START_LEVEL))
            classes.push_back(playerClass);

    return classes;
}

void AnimusForge::CompanionOwner::Configure(Player* owner, Template const& owner_template, State& state)
{
    SpecProfile const& spec = owner_template.Profile->Specs[urand(0, uint32(owner_template.Profile->Specs.size()) - 1)];

    GearBuilder::LearnProficiencies(owner);
    owner_template.Talents->Apply(owner, owner_template.Talents->Random(spec.TabPage, owner->GetFreeTalentPoints()));
    owner_template.Kit->Learn(owner);
    owner_template.Gear->Equip(owner, spec);

    owner->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    owner->UpdateAllStats();
    owner->SetFullHealth();
    owner->SetPower(POWER_MANA, owner->GetMaxPower(POWER_MANA));
    owner->SetPower(POWER_ENERGY, owner->GetMaxPower(POWER_ENERGY));

    if (owner->getClass() == CLASS_WARRIOR)
        owner->CastSpell(owner, SPELL_BATTLE_STANCE, true);

    // Its repertoire: every harmful single-target combat spell it knows, highest ranks only.
    state = State();
    for (auto const& [spellId, spell] : owner->GetSpellMap())
    {
        if (spell->State == PLAYERSPELL_REMOVED || !spell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (ActionCatalog::IsCombatSpell(info) && !info->IsPositive() && info->NeedsExplicitUnitTarget()
            && !info->IsAutoRepeatRangedSpell())
            state.Spells.push_back(spellId);
    }
}

void AnimusForge::CompanionOwner::Update(Player* owner, std::vector<Creature*> const& enemies, uint32 nowMs,
    Position const& home, State& state)
{
    if (!owner || !owner->IsAlive())
        return;

    // The enemy to fight: one already on the owner, else the nearest.
    Creature* target = nullptr;
    for (Creature* enemy : enemies)
    {
        if (!enemy->IsAlive())
            continue;

        bool const onOwner = enemy->GetVictim() == owner;
        bool const targetOnOwner = target && target->GetVictim() == owner;
        if (!target || (onOwner && !targetOnOwner)
            || (onOwner == targetOnOwner && owner->GetDistance(enemy) < owner->GetDistance(target)))
            target = enemy;
    }

    // Between pulls: recover and wander.
    if (!target)
    {
        if (owner->GetVictim())
            owner->AttackStop();

        if (!owner->IsInCombat() && nowMs >= state.NextRegenMs)
        {
            state.NextRegenMs = nowMs + REGEN_INTERVAL_MS;
            owner->SetHealth(std::min(owner->GetMaxHealth(),
                owner->GetHealth() + uint32(float(owner->GetMaxHealth()) * REGEN_FRACTION)));
            if (uint32 const maxMana = owner->GetMaxPower(POWER_MANA))
                owner->SetPower(POWER_MANA, std::min(maxMana, owner->GetPower(POWER_MANA)
                    + uint32(float(maxMana) * REGEN_FRACTION)));
        }

        if (nowMs >= state.NextMoveMs)
        {
            state.NextMoveMs = nowMs + urand(WANDER_MIN_MS, WANDER_MAX_MS);

            float const angle = frand(0.0f, 2.0f * float(M_PI));
            float const distance = frand(WANDER_MIN_DISTANCE, WANDER_MAX_DISTANCE);
            Position destination(owner->GetPositionX() + distance * std::cos(angle),
                owner->GetPositionY() + distance * std::sin(angle), owner->GetPositionZ());

            // Stay near home: head back when the step would leave the leash.
            if (destination.GetExactDist2d(&home) > WANDER_LEASH)
                destination.Relocate(home.GetPositionX() + frand(-5.0f, 5.0f), home.GetPositionY() + frand(-5.0f, 5.0f),
                    home.GetPositionZ());

            owner->UpdateAllowedPositionZ(destination.m_positionX, destination.m_positionY, destination.m_positionZ);
            owner->GetMotionMaster()->MovePoint(OWNER_MOVE_POINT_ID, destination);
        }

        return;
    }

    if (nowMs < state.EngageMs)
        return;

    if (!owner->IsWithinMeleeRange(target))
    {
        if (nowMs >= state.NextMoveMs)
        {
            state.NextMoveMs = nowMs + CHASE_REPATH_MS;

            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            target->GetNearPoint(owner, x, y, z, owner->GetCombatReach(), 0.5f, target->GetAngle(owner));
            owner->GetMotionMaster()->MovePoint(OWNER_MOVE_POINT_ID, x, y, z);
        }

        return;
    }

    if (owner->GetVictim() != target)
    {
        owner->GetMotionMaster()->Clear();
        owner->SetFacingToObject(target);
        owner->Attack(target, true);
    }

    // Now and then one of its spells, if it can cast it right now.
    if (nowMs >= state.NextSpellMs && !state.Spells.empty() && !owner->IsNonMeleeSpellCast(false))
    {
        state.NextSpellMs = nowMs + urand(SPELL_MIN_MS, SPELL_MAX_MS);

        SpellInfo const* info = sSpellMgr->GetSpellInfo(state.Spells[urand(0, uint32(state.Spells.size()) - 1)]);
        if (info && !owner->HasSpellCooldown(info->Id) && !owner->GetGlobalCooldownMgr().HasGlobalCooldown(info))
        {
            SpellCastTargets targets;
            targets.SetUnitTarget(target);
            Spell* spell = new Spell(owner, info, TRIGGERED_NONE);
            spell->prepare(&targets);
        }
    }
}
