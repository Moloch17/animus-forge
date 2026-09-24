/*
 * This file is part of the Animus project, based on AzerothCore.
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

#ifndef ANIMUS_LIB_CORE_HOOKS_H
#define ANIMUS_LIB_CORE_HOOKS_H

#include "Define.h"

class Group;
class WorldSession;

/*
 * What only the forge core can do, behind function pointers the module fills in at load (AddSC_animus_forge). The
 * curriculum layer calls these seams wherever it needs the forge core's help and never a forge-only API directly,
 * which keeps the layer's sources the same ones mod-animus carries for a stock core.
 */
namespace Animus::CoreHooks
{
    struct Seams
    {
        /// A bot's session: no account or character rows exist, so logout, play time and instance binds write nothing.
        void (*MarkSimSession)(WorldSession* session) = nullptr;

        /// A group of bots that is never written to the character database.
        void (*MarkSimGroup)(Group* group) = nullptr;

        /// Restart the world thread's random numbers from `seed` (0: back to entropy).
        void (*SeedRandom)(uint32 seed) = nullptr;
    };

    /// Set the seams: once, at load, before any bot is created. Every call below assumes they are set.
    void Install(Seams const& seams);

    void MarkSimSession(WorldSession* session);
    void MarkSimGroup(Group* group);
    void SeedRandom(uint32 seed);
}

#endif
