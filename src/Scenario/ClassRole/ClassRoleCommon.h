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
 * The class/role building blocks -- profiles, class kits, talent builds, gear, the action catalog, layouts, the seat
 * encoder and supplies -- in namespace AnimusForge::ClassRole, made available to the scenario code here.
 */
#include "ClassRoleAssets.h"
#include "SeatEncoder.h"
#include "Supplies.h"

namespace AnimusForge
{
    using AnimusForge::ClassRole::ActionCatalog;
    using AnimusForge::ClassRole::BattleSupplies;
    using AnimusForge::ClassRole::CallHunterBeast;
    using AnimusForge::ClassRole::ClassKit;
    using AnimusForge::ClassRole::ClassRoleAssets;
    using AnimusForge::ClassRole::ClassRoleProfile;
    using AnimusForge::ClassRole::ClassRoleProfiles;
    using AnimusForge::ClassRole::ConsumablePool;
    using AnimusForge::ClassRole::GearBuilder;
    using AnimusForge::ClassRole::RangeBand;
    using AnimusForge::ClassRole::Role;
    using AnimusForge::ClassRole::RoleName;
    using AnimusForge::ClassRole::SeatActionResult;
    using AnimusForge::ClassRole::SeatEncoder;
    using AnimusForge::ClassRole::SeatView;
    using AnimusForge::ClassRole::SpecProfile;
    using AnimusForge::ClassRole::StablePool;
    using AnimusForge::ClassRole::StatProfile;
    using AnimusForge::ClassRole::StockBattleSupplies;
    using AnimusForge::ClassRole::StockConsumables;
    using AnimusForge::ClassRole::TalentBuilder;
    using AnimusForge::ClassRole::WeaponLayout;
}

#endif
