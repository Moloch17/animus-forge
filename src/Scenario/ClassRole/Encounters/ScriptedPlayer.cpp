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

#include "ScriptedPlayer.h"
#include "Creature.h"
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
    using namespace AnimusForge::ClassRole;
    using ScriptedPlayer::State;
    using ScriptedPlayer::Tuning;

    enum ScriptedPlayerSpells : uint32
    {
        SPELL_BATTLE_STANCE     = 2457,
        SPELL_DEFENSIVE_STANCE  = 71,
    };

    constexpr uint32 MOVE_POINT_ID = 2;
    constexpr uint32 CHASE_REPATH_MS = 1000;
    constexpr uint32 REGEN_INTERVAL_MS = 1000;
    constexpr float WANDER_MIN_DISTANCE = 8.0f;
    constexpr float WANDER_MAX_DISTANCE = 20.0f;
    constexpr float WANDER_LEASH = 30.0f;       // never wander further than this from home
    constexpr float HEAL_RANGE = 40.0f;

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

    /// A random spell of `spells` at `target`, if the spell timer allows; restarts the timer.
    void CastSometimes(Player* caster, std::vector<uint32> const& spells, Unit* target, uint32 nowMs, uint32& nextMs,
        uint32 minMs, uint32 maxMs)
    {
        if (nowMs < nextMs || spells.empty())
            return;

        nextMs = nowMs + urand(minMs, maxMs);
        TryCast(caster, spells[urand(0, uint32(spells.size()) - 1)], target);
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
        player->GetMotionMaster()->MovePoint(MOVE_POINT_ID, x, y, z);
    }

    /// Between pulls: recover out of combat and wander near home.
    void Idle(Player* player, uint32 nowMs, Position const& home, State& state, Tuning const& tuning)
    {
        if (player->GetVictim())
            player->AttackStop();

        if (!player->IsInCombat() && nowMs >= state.NextRegenMs)
        {
            state.NextRegenMs = nowMs + REGEN_INTERVAL_MS;
            player->SetHealth(std::min(player->GetMaxHealth(),
                player->GetHealth() + uint32(float(player->GetMaxHealth()) * tuning.RegenFraction)));
            if (uint32 const maxMana = player->GetMaxPower(POWER_MANA))
                player->SetPower(POWER_MANA, std::min(maxMana, player->GetPower(POWER_MANA)
                    + uint32(float(maxMana) * tuning.RegenFraction)));
        }

        if (nowMs < state.NextMoveMs)
            return;

        state.NextMoveMs = nowMs + urand(tuning.WanderMinMs, tuning.WanderMaxMs);

        float const angle = frand(0.0f, 2.0f * float(M_PI));
        float const distance = frand(WANDER_MIN_DISTANCE, WANDER_MAX_DISTANCE);
        Position destination(player->GetPositionX() + distance * std::cos(angle),
            player->GetPositionY() + distance * std::sin(angle), player->GetPositionZ());

        // Stay near home: head back when the step would leave the leash.
        if (destination.GetExactDist2d(&home) > WANDER_LEASH)
            destination.Relocate(home.GetPositionX() + frand(-5.0f, 5.0f), home.GetPositionY() + frand(-5.0f, 5.0f),
                home.GetPositionZ());

        player->UpdateAllowedPositionZ(destination.m_positionX, destination.m_positionY, destination.m_positionZ);
        player->GetMotionMaster()->MovePoint(MOVE_POINT_ID, destination);
    }

    /// Melee the target, casting one of the player's damage spells now and then.
    void Fight(Player* player, Unit* target, uint32 nowMs, State& state, Tuning const& tuning)
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

        CastSometimes(player, state.Spells, target, nowMs, state.NextSpellMs, tuning.SpellMinMs, tuning.SpellMaxMs);
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

    void UpdateTank(Player* member, std::vector<Unit*> const& enemies, uint32 nowMs, Position const& home,
        State& state, Tuning const& tuning)
    {
        bool const pullUp = std::any_of(enemies.begin(), enemies.end(), [](Unit* enemy) { return enemy->IsAlive(); });
        if (!pullUp)
        {
            Idle(member, nowMs, home, state, tuning);
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

        if (loose && member->IsWithinDist(loose, tuning.TauntRange))
            for (uint32 taunt : state.Taunts)
                if (TryCast(member, taunt, loose))
                    break;

        Fight(member, loose ? loose : DefaultTarget(member, enemies), nowMs, state, tuning);
    }

    void UpdateHealer(Player* member, std::vector<Player*> const& party, Player* tank,
        std::vector<Unit*> const& enemies, uint32 nowMs, Position const& home, State& state, Tuning const& tuning)
    {
        // The most hurt party member in range, below the threshold.
        Player* patient = nullptr;
        for (Player* ally : party)
            if (ally && ally->IsAlive() && ally->GetHealthPct() < tuning.HealBelow * 100.0f
                && member->IsWithinDist(ally, HEAL_RANGE)
                && (!patient || ally->GetHealthPct() < patient->GetHealthPct()))
                patient = ally;

        if (patient && nowMs >= state.NextHealMs && !state.Heals.empty())
        {
            member->GetMotionMaster()->Clear();
            member->StopMoving();
            CastSometimes(member, state.Heals, patient, nowMs, state.NextHealMs, tuning.HealMinMs, tuning.HealMaxMs);
            return;
        }

        bool const pullUp = std::any_of(enemies.begin(), enemies.end(), [](Unit* enemy) { return enemy->IsAlive(); });
        if (!pullUp)
        {
            Idle(member, nowMs, home, state, tuning);
            return;
        }

        if (member->IsNonMeleeSpellCast(false))
            return;

        // Stay in reach of the tank, out of the melee.
        Player* anchor = tank && tank != member && tank->IsAlive() ? tank : nullptr;
        if (anchor && !member->IsWithinDist(anchor, tuning.HealerRange))
        {
            MoveNear(member, anchor, tuning.HealerRange * 0.5f, nowMs, state);
            return;
        }

        // Nobody to heal: help with a damage spell on the tank's target.
        Unit* target = anchor && anchor->GetVictim() ? anchor->GetVictim() : DefaultTarget(member, enemies);
        if (target && nowMs >= state.EngageMs && nowMs >= state.NextSpellMs && !state.Spells.empty())
        {
            member->SetFacingToObject(target);
            CastSometimes(member, state.Spells, target, nowMs, state.NextSpellMs, tuning.SpellMinMs,
                tuning.SpellMaxMs);
        }
    }
}

void AnimusForge::ClassRole::ScriptedPlayer::Configure(Player* player, ClassRoleAssets const& assets, State& state)
{
    SpecProfile const& spec = assets.Profile->Specs[urand(0, uint32(assets.Profile->Specs.size()) - 1)];

    GearBuilder::LearnProficiencies(player);
    assets.Talents->Apply(player, assets.Talents->Random(spec.TabPage, player->GetFreeTalentPoints()));
    assets.Kit->Learn(player);
    assets.Gear->Equip(player, spec);

    player->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    player->UpdateAllStats();
    player->SetFullHealth();
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    player->SetPower(POWER_ENERGY, player->GetMaxPower(POWER_ENERGY));

    state = State();
    state.PlayRole = assets.Profile->PlayRole;
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

void AnimusForge::ClassRole::ScriptedPlayer::UpdateMember(Player* member, std::vector<Player*> const& party,
    Player* tank, std::vector<Unit*> const& enemies, uint32 nowMs, Position const& home, State& state,
    Tuning const& tuning)
{
    if (!member || !member->IsAlive())
        return;

    switch (state.PlayRole)
    {
        case Role::Tank:
            UpdateTank(member, enemies, nowMs, home, state, tuning);
            return;
        case Role::Heal:
            UpdateHealer(member, party, tank, enemies, nowMs, home, state, tuning);
            return;
        case Role::Dps:
            break;
    }

    // A damage dealer fights the tank's target once the tank has one.
    Unit* tankTarget = tank && tank != member && tank->IsAlive() ? tank->GetVictim() : nullptr;
    Unit* target = tankTarget && tankTarget->IsAlive() ? tankTarget : DefaultTarget(member, enemies);
    if (!target)
    {
        Idle(member, nowMs, home, state, tuning);
        return;
    }

    if (nowMs < state.EngageMs)
        return;

    Fight(member, target, nowMs, state, tuning);
}

void AnimusForge::ClassRole::ScriptedPlayer::UpdateOpponent(Player* player, Player* enemy, uint32 nowMs, State& state,
    Tuning const& tuning)
{
    if (!player || !player->IsAlive() || !enemy || !enemy->IsAlive())
        return;

    if (player->IsNonMeleeSpellCast(false))
        return;

    if (state.PlayRole == Role::Heal && player->GetHealthPct() < tuning.SelfHealBelow * 100.0f
        && !state.Heals.empty() && nowMs >= state.NextHealMs)
    {
        player->GetMotionMaster()->Clear();
        player->StopMoving();
        CastSometimes(player, state.Heals, player, nowMs, state.NextHealMs, tuning.HealMinMs, tuning.HealMaxMs);
        return;
    }

    if (nowMs < state.EngageMs)
        return;

    if (!state.Ranged)
    {
        Fight(player, enemy, nowMs, state, tuning);
        return;
    }

    float const distance = player->GetDistance(enemy);
    if (distance > tuning.RangedMax || distance < tuning.RangedMin * 0.5f)
    {
        MoveNear(player, enemy, (tuning.RangedMin + tuning.RangedMax) * 0.5f, nowMs, state);
        return;
    }

    if (!player->movespline->Finalized())
        return;

    player->SetFacingToObject(enemy);
    CastSometimes(player, state.Spells, enemy, nowMs, state.NextSpellMs, tuning.SpellMinMs, tuning.SpellMaxMs);
}
