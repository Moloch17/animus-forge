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

#ifndef MOD_ANIMUS_FORGE_CLASS_ROLE_ENCODER_SUPPORT_H
#define MOD_ANIMUS_FORGE_CLASS_ROLE_ENCODER_SUPPORT_H

#include "ActionCatalog.h"
#include "ObjectGuid.h"
#include "SeatView.h"
#include "Spell.h"
#include "Unit.h"

class Item;
class Player;
class SpellInfo;

/*
 * What the blocks share: reading cooldowns and auras, the core's cast checks run without casting, enemy slots, and
 * moving the bot the way a client would.
 */
namespace AnimusForge::ClassRole::Encoding
{
    constexpr uint32 IMMOBILE_STATES = UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING;
    constexpr uint32 STUN_STATES = UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING;
    constexpr uint32 CROWD_CONTROL_STATES = STUN_STATES | UNIT_STATE_ROOT;

    /// Remaining cooldown of a spell as a fraction of its full cooldown.
    [[nodiscard]] float CooldownFraction(Player const* bot, SpellInfo const* info);

    /// Remaining duration fraction of `caster`'s aura `spellId` on `unit` (1 for permanent auras); raises `stacks` to
    /// its stacks or charges / 5.
    [[nodiscard]] float AuraFraction(Unit const* unit, uint32 spellId, ObjectGuid caster, float& stacks);

    /// Features for a known/cooldown pair list: 1 and the cooldown fraction for each spell the bot knows.
    void WriteKnownCooldowns(Player const* bot, std::vector<ActionCatalog::Action> const& actions, float* out);

    /// The targets a client would send for `info` aimed at `target` (null: self-cast spells only).
    [[nodiscard]] SpellCastTargets TargetsFor(SpellInfo const* info, Player* bot, Unit* target);

    /// A cast in its cast time (channels excluded): the client refuses to start another spell or use an item
    /// meanwhile. The core only checks this for client casts, so actions check it here.
    [[nodiscard]] bool CastInProgress(Player const* bot);

    /// The core's own cast validation (Spell::CheckCast), without casting. `target` may be null (self-cast spells).
    [[nodiscard]] bool CanCast(Player* bot, SpellInfo const* info, Unit* target, Item* castItem = nullptr);

    /// Whether an ally heal can be cast on `ally` now, and casting it.
    [[nodiscard]] bool CanHeal(Player* bot, ActionCatalog::Action const& heal, Unit* ally);
    void Heal(Player* bot, ActionCatalog::Action const& heal, Unit* ally, SeatActionResult& result);

    /// Whether a spell action of the catalog could be cast at `target` now.
    [[nodiscard]] bool IsSpellActionAllowed(SeatView const& view, Unit* target, ActionCatalog::Action const& def);

    /// Cast a spell action at `target` as CMSG_CAST_SPELL would. Returns true if it started.
    bool ApplySpellAction(SeatView const& view, Unit* target, ActionCatalog::Action const& def,
        SeatActionResult& result);

    /// The on-use spell of an item (a trinket), or null.
    [[nodiscard]] SpellInfo const* TrinketSpell(Item const* item);

    /// The on-use spell of an item entry (food, drink, potions, stones), or null.
    [[nodiscard]] SpellInfo const* UseSpell(uint32 itemEntry);

    /// Whether the bot can use its `entry` item (a potion, healthstone, bandage or soulstone) on `target` now, and
    /// using it as the client does (true if one was used up).
    [[nodiscard]] bool CanUseItemOn(Player* bot, uint32 entry, Unit* target);
    bool UseItemOn(Player* bot, uint32 entry, Unit* target);

    /// The remaining cooldown of an item's on-use spell as a fraction; 0 without the item.
    [[nodiscard]] float ItemCooldownFraction(Player const* bot, uint32 entry);

    /// A revive (a resurrection spell on a dead ally, or a warlock's soulstone on a living one) usable on `ally` now,
    /// and using it.
    [[nodiscard]] bool CanRevive(SeatView const& view, ActionCatalog::Action const& revive, Player* ally);
    void Revive(SeatView const& view, ActionCatalog::Action const& revive, Player* ally, SeatActionResult& result);

    /// Revive features, two per revive: known (a spell the bot knows, or a soulstone in its bags) and cooldown.
    void WriteRevives(SeatView const& view, float* out);

    /// The bot's pet, or its first living controlled unit other than a totem.
    [[nodiscard]] Unit* FirstPet(Player* bot);

    /// A shapeshift the player could cancel from the client (druid forms, Shadowform, Ghost Wolf, Stealth).
    [[nodiscard]] SpellInfo const* CancellableForm(Player const* bot);

    [[nodiscard]] bool IsCrowdControlled(Unit const* unit);

    /// The enemy slot of `unit`, or -1.
    [[nodiscard]] int32 SlotOf(SeatView const& view, Unit const* unit);

    /// An enemy slot, other than `except`, whose living enemy attacks `victim`; -1 if none.
    [[nodiscard]] int32 SlotAttacking(SeatView const& view, Unit const* victim, uint32 except);

    /// Select enemy `slot` and keep swinging, at the new target.
    void SelectEnemy(SeatView& view, uint32 slot);

    /// Run to a point, replacing whatever movement the bot had.
    void MoveTo(Player* bot, uint32 pointId, float x, float y, float z);

    /// Send the bot's pets and guardians at `target`, as the pet bar's Attack does. True if any was ordered.
    bool PetAttack(Player* bot, Unit* target);

    /// Put the Call Pet global cooldown on the bot, as calling a beast does.
    void StartCallBeastCooldown(Player* bot);
}

#endif
