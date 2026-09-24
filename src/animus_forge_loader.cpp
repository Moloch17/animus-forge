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

void AddSC_animus_lib();
void AddSC_animus_forge();
void AddSC_animus_forge_commands();

// Called by the generated modules loader; the name is Add<module dir with - as _>Scripts.
void Addmod_animus_forgeScripts()
{
    // The curriculum's core hooks, which feed the env pool. These had a loader and a module directory of their own
    // while animus-lib was a library mod-animus also registered; there is one module now, so they are registered
    // here, once, like everything else.
    AddSC_animus_lib();
    AddSC_animus_forge();
    AddSC_animus_forge_commands();
}
