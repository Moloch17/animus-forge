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

#ifndef MOD_ANIMUS_FORGE_CLASS_ROLE_COMMON_H
#define MOD_ANIMUS_FORGE_CLASS_ROLE_COMMON_H

/*
 * The class/role building blocks -- profiles, class kits, talent builds, gear, the action catalog -- live in
 * mod-animus (src/ClassRole), which plays the trained models on a stock server: training and play build characters
 * and actions from the same code. mod-animus is built alongside this module.
 */
#include "ClassRoleAssets.h"

namespace AnimusForge
{
    using Animus::ClassRole::ActionCatalog;
    using Animus::ClassRole::ClassKit;
    using Animus::ClassRole::ClassRoleAssets;
    using Animus::ClassRole::ClassRoleProfile;
    using Animus::ClassRole::ClassRoleProfiles;
    using Animus::ClassRole::GearBuilder;
    using Animus::ClassRole::RangeBand;
    using Animus::ClassRole::Role;
    using Animus::ClassRole::RoleName;
    using Animus::ClassRole::SpecProfile;
    using Animus::ClassRole::StatProfile;
    using Animus::ClassRole::TalentBuilder;
    using Animus::ClassRole::WeaponLayout;
}

#endif
