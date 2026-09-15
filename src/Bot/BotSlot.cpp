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

#include "BotSlot.h"
#include "Player.h"
#include "Position.h"
#include "WorldSession.h"

AnimusForge::BotSlot::BotSlot() = default;
AnimusForge::BotSlot::~BotSlot() = default;
AnimusForge::BotSlot::BotSlot(BotSlot&&) noexcept = default;
AnimusForge::BotSlot& AnimusForge::BotSlot::operator=(BotSlot&&) noexcept = default;

Player* AnimusForge::BotSlot::Active() const
{
    WorldSession* session = _sessions[_active].get();
    return session ? session->GetPlayer() : nullptr;
}

void AnimusForge::BotSlot::Begin()
{
    _previous = Active();
    _created = false;
}

uint8 AnimusForge::BotSlot::NextSession() const
{
    return _previous ? uint8(1 - _active) : _active;
}

Player* AnimusForge::BotSlot::CreateNext(BotFactory::BotSpec spec, Map*& map, uint32 mapId, Position const& start)
{
    uint8 const session = NextSession();
    spec.GuidLow = _guids[session];

    Player* bot = BotFactory::Create(spec, _sessions[session].get());
    if (!bot)
        return nullptr;

    // Create made a session if the slot had none; the slot owns it either way.
    if (_sessions[session].get() != bot->GetSession())
        _sessions[session].reset(bot->GetSession());
    _guids[session] = bot->GetGUID().GetCounter();

    bool const placed = map ? BotFactory::PlaceInMap(bot, map, start)
        : (map = BotFactory::PlaceInNewInstance(bot, mapId, start)) != nullptr;
    if (!placed)
    {
        // DestroyUnplaced deletes the session too.
        [[maybe_unused]] WorldSession* released = _sessions[session].release();
        BotFactory::DestroyUnplaced(bot);
        return nullptr;
    }

    _created = true;
    return bot;
}

void AnimusForge::BotSlot::Promote()
{
    uint8 const next = NextSession();

    // The session stays with the slot for the next rebuild.
    if (_previous)
        BotFactory::Destroy(_previous, true);

    if (_created)
        _active = next;

    _previous = nullptr;
    _created = false;
}

void AnimusForge::BotSlot::Destroy()
{
    if (Player* bot = Active())
    {
        // Destroy deletes the session.
        [[maybe_unused]] WorldSession* released = _sessions[_active].release();
        BotFactory::Destroy(bot);
    }

    _previous = nullptr;
    _created = false;
}
