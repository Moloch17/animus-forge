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

void Addmod_animus_libScripts();
void AddSC_animus_forge();
void AddSC_animus_forge_commands();

// Called by the generated modules loader; the name is Add<module dir with - as _>Scripts.
void Addmod_animus_forgeScripts()
{
    // animus-lib's hooks, which feed the forge's env pool (registered once, whichever module asks first).
    Addmod_animus_libScripts();
    AddSC_animus_forge();
    AddSC_animus_forge_commands();
}
