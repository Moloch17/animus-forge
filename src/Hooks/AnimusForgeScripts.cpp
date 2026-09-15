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

#include "AnimusForge.h"
#include "CoreHooks.h"
#include "Group.h"
#include "RandomSeed.h"
#include "WorldScript.h"
#include "WorldSession.h"

/*
 * The forge's world hooks. The combat hooks every env pool needs (damage, heals, casts, summon levels) are
 * animus-lib's (Hooks/AnimusLibScripts.cpp), fed while the forge's pool is registered.
 */
namespace
{
    class AnimusForgeWorldScript : public WorldScript
    {
    public:
        AnimusForgeWorldScript() : WorldScript("AnimusForgeWorldScript",
            { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) { }

        void OnStartup() override { sAnimusForge->OnStartup(); }
        void OnUpdate(uint32 diff) override { sAnimusForge->OnUpdate(diff); }
        void OnShutdown() override { sAnimusForge->OnShutdown(); }
    };
}

void AddSC_animus_forge()
{
    // What only the forge core can do, for the library's bots, groups and evaluation seeds.
    Animus::CoreHooks::Seams seams;
    seams.MarkSimSession = [](WorldSession* session) { session->SetSimSession(true); };
    seams.MarkSimGroup = [](Group* group) { group->SetSimGroup(true); };
    seams.SeedRandom = [](uint32 seed) { rand_seed(seed); };
    Animus::CoreHooks::Install(seams);

    new AnimusForgeWorldScript();
}
