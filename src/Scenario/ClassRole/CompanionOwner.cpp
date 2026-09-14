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
    using AnimusForge::Role;
    using AnimusForge::CompanionOwner::State;

    constexpr std::array<uint8, 10> PLAYABLE_RACES =
    {
        RACE_HUMAN, RACE_ORC, RACE_DWARF, RACE_NIGHTELF, RACE_UNDEAD_PLAYER, RACE_TAUREN, RACE_GNOME, RACE_TROLL,
        RACE_BLOODELF, RACE_DRAENEI
    };

    enum OwnerSpells : uint32
    {
        SPELL_BATTLE_STANCE     = 2457,
        SPELL_DEFENSIVE_STANCE  = 71,
    };

    constexpr uint32 OWNER_MOVE_POINT_ID = 2;
    constexpr uint32 CHASE_REPATH_MS = 1000;
    constexpr uint32 SPELL_MIN_MS = 2000;
    constexpr uint32 SPELL_MAX_MS = 4000;
    constexpr uint32 HEAL_MIN_MS = 1500;
    constexpr uint32 HEAL_MAX_MS = 2500;
    constexpr uint32 WANDER_MIN_MS = 6000;
    constexpr uint32 WANDER_MAX_MS = 12000;
    constexpr uint32 REGEN_INTERVAL_MS = 1000;
    constexpr float REGEN_FRACTION = 0.04f;     // of max health and mana per second, out of combat
    constexpr float WANDER_MIN_DISTANCE = 8.0f;
    constexpr float WANDER_MAX_DISTANCE = 20.0f;
    constexpr float WANDER_LEASH = 30.0f;       // never wander further than this from home
    constexpr float HEAL_BELOW = 0.85f;         // healers heal party members under this health fraction
    constexpr float HEALER_RANGE = 30.0f;       // healers stay this close to the tank
    constexpr float HEAL_RANGE = 40.0f;
    constexpr float TAUNT_RANGE = 25.0f;
    constexpr float RANGED_MIN = 20.0f;         // PvP: a ranged spec backs off inside this ...
    constexpr float RANGED_MAX = 30.0f;         // ... and closes in beyond this
    constexpr float SELF_HEAL_BELOW = 0.6f;
    constexpr uint8 DEATH_KNIGHT_START_LEVEL = 55;

    bool IsHeal(SpellInfo const* info)
    {
        if (!info || info->IsPassive() || !info->IsPositive() || !info->NeedsExplicitUnitTarget())
            return false;

        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_HEAL
                || (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL))
                return true;

        return false;
    }

    bool IsTaunt(SpellInfo const* info)
    {
        if (!info || info->IsPassive() || !info->NeedsExplicitUnitTarget())
            return false;

        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_ATTACK_ME
                || (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_MOD_TAUNT))
                return true;

        return false;
    }

    bool TryCast(Player* caster, uint32 spellId, Unit* target)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || caster->HasSpellCooldown(info->Id) || caster->GetGlobalCooldownMgr().HasGlobalCooldown(info)
            || caster->IsNonMeleeSpellCast(false))
            return false;

        SpellCastTargets targets;
        targets.SetUnitTarget(target);
        Spell* spell = new Spell(caster, info, TRIGGERED_NONE);
        return spell->prepare(&targets) == SPELL_CAST_OK;
    }

    void MoveNear(Player* player, Unit* target, float distance, uint32 nowMs, State& state)
    {
        if (nowMs < state.NextMoveMs)
            return;

        state.NextMoveMs = nowMs + CHASE_REPATH_MS;

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        target->GetNearPoint(player, x, y, z, player->GetCombatReach(), distance, target->GetAngle(player));
        player->GetMotionMaster()->MovePoint(OWNER_MOVE_POINT_ID, x, y, z);
    }

    /// Between pulls: recover out of combat and wander near home.
    void Idle(Player* player, uint32 nowMs, Position const& home, State& state)
    {
        if (player->GetVictim())
            player->AttackStop();

        if (!player->IsInCombat() && nowMs >= state.NextRegenMs)
        {
            state.NextRegenMs = nowMs + REGEN_INTERVAL_MS;
            player->SetHealth(std::min(player->GetMaxHealth(),
                player->GetHealth() + uint32(float(player->GetMaxHealth()) * REGEN_FRACTION)));
            if (uint32 const maxMana = player->GetMaxPower(POWER_MANA))
                player->SetPower(POWER_MANA, std::min(maxMana, player->GetPower(POWER_MANA)
                    + uint32(float(maxMana) * REGEN_FRACTION)));
        }

        if (nowMs < state.NextMoveMs)
            return;

        state.NextMoveMs = nowMs + urand(WANDER_MIN_MS, WANDER_MAX_MS);

        float const angle = frand(0.0f, 2.0f * float(M_PI));
        float const distance = frand(WANDER_MIN_DISTANCE, WANDER_MAX_DISTANCE);
        Position destination(player->GetPositionX() + distance * std::cos(angle),
            player->GetPositionY() + distance * std::sin(angle), player->GetPositionZ());

        // Stay near home: head back when the step would leave the leash.
        if (destination.GetExactDist2d(&home) > WANDER_LEASH)
            destination.Relocate(home.GetPositionX() + frand(-5.0f, 5.0f), home.GetPositionY() + frand(-5.0f, 5.0f),
                home.GetPositionZ());

        player->UpdateAllowedPositionZ(destination.m_positionX, destination.m_positionY, destination.m_positionZ);
        player->GetMotionMaster()->MovePoint(OWNER_MOVE_POINT_ID, destination);
    }

    /// Melee the target, casting one of the player's damage spells now and then.
    void Fight(Player* player, Unit* target, uint32 nowMs, State& state)
    {
        if (!player->IsWithinMeleeRange(target))
        {
            MoveNear(player, target, 0.5f, nowMs, state);
            return;
        }

        if (player->GetVictim() != target)
        {
            player->GetMotionMaster()->Clear();
            player->SetFacingToObject(target);
            player->Attack(target, true);
        }

        if (nowMs >= state.NextSpellMs && !state.Spells.empty())
        {
            state.NextSpellMs = nowMs + urand(SPELL_MIN_MS, SPELL_MAX_MS);
            TryCast(player, state.Spells[urand(0, uint32(state.Spells.size()) - 1)], target);
        }
    }

    /// The enemy to fight: one already on the player, else the nearest.
    Unit* DefaultTarget(Player* player, std::vector<Unit*> const& enemies)
    {
        Unit* target = nullptr;
        for (Unit* enemy : enemies)
        {
            if (!enemy->IsAlive())
                continue;

            bool const onPlayer = enemy->GetVictim() == player;
            bool const targetOnPlayer = target && target->GetVictim() == player;
            if (!target || (onPlayer && !targetOnPlayer)
                || (onPlayer == targetOnPlayer && player->GetDistance(enemy) < player->GetDistance(target)))
                target = enemy;
        }

        return target;
    }
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
        std::pair<uint8, Role> const key{ profile.Class, profile.PlayRole };
        if (_byClassRole.contains(key))
            continue;

        Template& entry = _byClassRole[key];
        entry.Profile = &profile;
        for (uint8 race : PLAYABLE_RACES)
            if (sObjectMgr->GetPlayerInfo(race, profile.Class))
                entry.Races.push_back(race);

        entry.Kit = std::make_unique<ClassKit>(profile.Class);
        entry.Talents = std::make_unique<TalentBuilder>(profile.Class);
        entry.Gear = std::make_unique<GearBuilder>(profile, *entry.Kit);
    }

    LOG_INFO("module.animus", "Scripted players: {} class/role templates", _byClassRole.size());
}

AnimusForge::CompanionOwner::Template const* AnimusForge::CompanionOwner::Templates::ForClassRole(uint8 playerClass,
    Role role) const
{
    auto const itr = _byClassRole.find({ playerClass, role });
    return itr != _byClassRole.end() ? &itr->second : nullptr;
}

std::vector<uint8> AnimusForge::CompanionOwner::Templates::ClassesForRole(uint8 level, Role role) const
{
    std::vector<uint8> classes;
    for (auto const& [key, entry] : _byClassRole)
        if (key.second == role && !entry.Races.empty()
            && (key.first != CLASS_DEATH_KNIGHT || level >= DEATH_KNIGHT_START_LEVEL))
            classes.push_back(key.first);

    return classes;
}

void AnimusForge::CompanionOwner::Configure(Player* player, Template const& player_template, State& state)
{
    SpecProfile const& spec =
        player_template.Profile->Specs[urand(0, uint32(player_template.Profile->Specs.size()) - 1)];

    GearBuilder::LearnProficiencies(player);
    player_template.Talents->Apply(player, player_template.Talents->Random(spec.TabPage, player->GetFreeTalentPoints()));
    player_template.Kit->Learn(player);
    player_template.Gear->Equip(player, spec);

    player->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    player->UpdateAllStats();
    player->SetFullHealth();
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    player->SetPower(POWER_ENERGY, player->GetMaxPower(POWER_ENERGY));

    state = State();
    state.PlayRole = player_template.Profile->PlayRole;
    state.Ranged = spec.Range == RangeBand::Ranged;

    if (player->getClass() == CLASS_WARRIOR)
        player->CastSpell(player, state.PlayRole == Role::Tank && player->HasSpell(SPELL_DEFENSIVE_STANCE)
            ? SPELL_DEFENSIVE_STANCE : SPELL_BATTLE_STANCE, true);

    // Its repertoire, highest ranks only: harmful single-target combat spells, heals and taunts.
    for (auto const& [spellId, spell] : player->GetSpellMap())
    {
        if (spell->State == PLAYERSPELL_REMOVED || !spell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (IsTaunt(info))
            state.Taunts.push_back(spellId);
        else if (IsHeal(info))
            state.Heals.push_back(spellId);
        else if (ActionCatalog::IsCombatSpell(info) && !info->IsPositive() && info->NeedsExplicitUnitTarget()
            && !info->IsAutoRepeatRangedSpell())
            state.Spells.push_back(spellId);
    }
}

void AnimusForge::CompanionOwner::Update(Player* player, std::vector<Unit*> const& enemies, uint32 nowMs,
    Position const& home, State& state, Unit* preferred)
{
    if (!player || !player->IsAlive())
        return;

    Unit* target = preferred && preferred->IsAlive() ? preferred : DefaultTarget(player, enemies);
    if (!target)
    {
        Idle(player, nowMs, home, state);
        return;
    }

    if (nowMs < state.EngageMs)
        return;

    Fight(player, target, nowMs, state);
}

void AnimusForge::CompanionOwner::UpdateMember(Player* member, std::vector<Player*> const& party, Player* tank,
    std::vector<Unit*> const& enemies, uint32 nowMs, Position const& home, State& state)
{
    if (!member || !member->IsAlive())
        return;

    bool const pullUp = std::any_of(enemies.begin(), enemies.end(), [](Unit* enemy) { return enemy->IsAlive(); });

    switch (state.PlayRole)
    {
        case Role::Tank:
        {
            if (!pullUp)
            {
                Idle(member, nowMs, home, state);
                return;
            }

            if (nowMs < state.EngageMs)
                return;

            // Whatever is hitting someone else comes first; taunt it off them.
            Unit* loose = nullptr;
            for (Unit* enemy : enemies)
                if (enemy->IsAlive() && enemy->GetVictim() && enemy->GetVictim() != member
                    && (!loose || member->GetDistance(enemy) < member->GetDistance(loose)))
                    loose = enemy;

            if (loose && member->IsWithinDist(loose, TAUNT_RANGE))
                for (uint32 taunt : state.Taunts)
                    if (TryCast(member, taunt, loose))
                        break;

            Fight(member, loose ? loose : DefaultTarget(member, enemies), nowMs, state);
            return;
        }
        case Role::Heal:
        {
            // The most hurt party member in range, below the threshold.
            Player* patient = nullptr;
            for (Player* ally : party)
                if (ally && ally->IsAlive() && ally->GetHealthPct() < HEAL_BELOW * 100.0f
                    && member->IsWithinDist(ally, HEAL_RANGE)
                    && (!patient || ally->GetHealthPct() < patient->GetHealthPct()))
                    patient = ally;

            if (patient && nowMs >= state.NextHealMs && !state.Heals.empty())
            {
                state.NextHealMs = nowMs + urand(HEAL_MIN_MS, HEAL_MAX_MS);
                member->GetMotionMaster()->Clear();
                member->StopMoving();
                TryCast(member, state.Heals[urand(0, uint32(state.Heals.size()) - 1)], patient);
                return;
            }

            if (!pullUp)
            {
                Idle(member, nowMs, home, state);
                return;
            }

            if (member->IsNonMeleeSpellCast(false))
                return;

            // Stay in reach of the tank, out of the melee.
            Player* anchor = tank && tank != member && tank->IsAlive() ? tank : nullptr;
            if (anchor && !member->IsWithinDist(anchor, HEALER_RANGE))
            {
                MoveNear(member, anchor, HEALER_RANGE * 0.5f, nowMs, state);
                return;
            }

            // Nobody to heal: help with a damage spell on the tank's target.
            Unit* target = anchor && anchor->GetVictim() ? anchor->GetVictim() : DefaultTarget(member, enemies);
            if (target && nowMs >= state.EngageMs && nowMs >= state.NextSpellMs && !state.Spells.empty())
            {
                state.NextSpellMs = nowMs + urand(SPELL_MIN_MS, SPELL_MAX_MS);
                member->SetFacingToObject(target);
                TryCast(member, state.Spells[urand(0, uint32(state.Spells.size()) - 1)], target);
            }
            return;
        }
        case Role::Dps:
            break;
    }

    Unit* tankTarget = tank && tank != member && tank->IsAlive() ? tank->GetVictim() : nullptr;
    Update(member, enemies, nowMs, home, state, tankTarget);
}

void AnimusForge::CompanionOwner::UpdateOpponent(Player* player, Player* enemy, uint32 nowMs, State& state)
{
    if (!player || !player->IsAlive() || !enemy || !enemy->IsAlive())
        return;

    if (player->IsNonMeleeSpellCast(false))
        return;

    if (state.PlayRole == Role::Heal && player->GetHealthPct() < SELF_HEAL_BELOW * 100.0f && !state.Heals.empty()
        && nowMs >= state.NextHealMs)
    {
        state.NextHealMs = nowMs + urand(HEAL_MIN_MS, HEAL_MAX_MS);
        player->GetMotionMaster()->Clear();
        player->StopMoving();
        TryCast(player, state.Heals[urand(0, uint32(state.Heals.size()) - 1)], player);
        return;
    }

    if (nowMs < state.EngageMs)
        return;

    if (!state.Ranged)
    {
        Fight(player, enemy, nowMs, state);
        return;
    }

    float const distance = player->GetDistance(enemy);
    if (distance > RANGED_MAX || distance < RANGED_MIN * 0.5f)
    {
        MoveNear(player, enemy, (RANGED_MIN + RANGED_MAX) * 0.5f, nowMs, state);
        return;
    }

    if (!player->movespline->Finalized())
        return;

    player->SetFacingToObject(enemy);
    if (nowMs >= state.NextSpellMs && !state.Spells.empty())
    {
        state.NextSpellMs = nowMs + urand(SPELL_MIN_MS, SPELL_MAX_MS);
        TryCast(player, state.Spells[urand(0, uint32(state.Spells.size()) - 1)], enemy);
    }
}
