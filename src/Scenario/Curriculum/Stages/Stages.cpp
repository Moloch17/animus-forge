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

/*
 * The curriculum, a tree: every stage extends one earlier stage (and seeds from it), keeping the base's
 * blocks it needs and adding its own.
 *
 *   duel ─┬─ pack ─ gauntlet ─ companion ─ party      (PvE)
 *         └─ pvp ─ arena                               (PvP)
 *
 * Scenario names carry the stage's number (stage1_duel ... stage7_arena), model names only its suffix (_duel). The
 * duel is the first stage: nothing seeds it.
 *
 * A stage's episodes are its arenas (see ArenaDefinition): each episode draws one by weight, so a stage can mix PvE
 * and PvP situations over the union of their blocks. Stages 1-7 have one arena each.
 *
 * Adding a stage is one entry here (plus new blocks or encounters only if it needs new features) and a learner
 * config, configs/<name>.yaml.
 */

#include "StageDefinition.h"
#include "Log.h"
#include <algorithm>

namespace
{
    using namespace AnimusForge::Curriculum;

    std::vector<StageDefinition> Definitions()
    {
        using enum BlockId;

        std::vector<StageDefinition> stages;

        stages.push_back({
            .Name = "stage1_duel",
            .Suffix = "_duel",
            .Extends = "",
            .Summary = "a same-level creature out of aggro range: close in and kill it fast, taking little damage",
            .Blocks = { Core, Duel },
            .Arenas = { { .Name = "duel", .Against = Opposition::Creature } },
        });

        stages.push_back({
            .Name = "stage2_pack",
            .Suffix = "_pack",
            .Extends = "stage1_duel",
            .Summary = "a pack of 2-4, casters included, usually linked: targets, interrupts, crowd control",
            .Blocks = { Core, Duel, Pack },
            .Arenas = { { .Name = "pack", .Against = Opposition::Pulls, .Schedule = PullSchedule::SinglePack } },
        });

        stages.push_back({
            .Name = "stage3_gauntlet",
            .Suffix = "_gauntlet",
            .Extends = "stage2_pack",
            .Summary = "pull after pull with short breaks: heals, food and drink",
            .Blocks = { Core, Duel, Pack, Gauntlet },
            .Arenas = { { .Name = "gauntlet", .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet } },
        });

        stages.push_back({
            .Name = "stage4_companion",
            .Suffix = "_companion",
            .Extends = "stage3_gauntlet",
            .Summary = "the gauntlet beside a scripted owner: follow, assist, guard and heal it",
            .Blocks = { Core, Duel, Pack, Gauntlet, Companion },
            .Arenas = { { .Name = "companion", .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                .Owner = true } },
        });

        stages.push_back({
            .Name = "stage5_party",
            .Suffix = "_party",
            .Extends = "stage4_companion",
            .Summary = "four learned seats and the scripted owner against elite-heavy pulls",
            .Blocks = { Core, Duel, Pack, Gauntlet, Companion, Party },
            .Arenas = { { .Name = "party", .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::Gauntlet, .Owner = true, .PartyGroup = true } },
        });

        // The PvP branch: off the duel, without the PvE blocks it would never fill.
        stages.push_back({
            .Name = "stage6_pvp",
            .Suffix = "_pvp",
            .Extends = "stage1_duel",
            .Summary = "one-on-one against a scripted enemy player",
            .Blocks = { Core, Duel, Pvp },
            .Arenas = { { .Name = "pvp_scripted", .Against = Opposition::ScriptedPlayer, .Pvp = true } },
        });

        stages.push_back({
            .Name = "stage7_arena",
            .Suffix = "_arena",
            .Extends = "stage6_pvp",
            .Summary = "self-play one-on-one: two learned seats of any classes",
            .Blocks = { Core, Duel, Pvp },
            .Arenas = { { .Name = "arena_1v1", .Seats = SeatPlan::Mirror, .Against = Opposition::MirrorSeat,
                .Pvp = true } },
        });

        // A pilot of arena mixing, not part of the curriculum: the duel and the scripted enemy player in one stage.
        // Trained only when named (forge start mix_duel_pvp).
        stages.push_back({
            .Name = "mix_duel_pvp",
            .Suffix = "_mix",
            .Extends = "stage6_pvp",
            .Summary = "pilot arena mix: half the episodes a creature duel, half a scripted enemy player",
            .Blocks = { Core, Duel, Pvp },
            .Arenas = {
                { .Name = "duel", .Against = Opposition::Creature },
                { .Name = "pvp_scripted", .Against = Opposition::ScriptedPlayer, .Pvp = true },
            },
            .InDefaultQueue = false,
        });

        return stages;
    }

    /// Why `arena` cannot be played with `stage`'s blocks, or empty.
    std::string ArenaProblem(StageDefinition const& stage, ArenaDefinition const& arena)
    {
        bool const pulls = arena.Against == Opposition::Pulls;
        bool const player = arena.Against == Opposition::ScriptedPlayer || arena.Against == Opposition::MirrorSeat;

        if (pulls != (arena.Schedule != PullSchedule::None))
            return "a pull schedule goes with pulls, and only with pulls";
        if (pulls && !stage.Has(BlockId::Pack))
            return "pulls need the pack block";
        if (arena.Schedule == PullSchedule::Gauntlet && !stage.Has(BlockId::Gauntlet))
            return "the gauntlet schedule needs the gauntlet block";
        if (arena.Owner && (!pulls || !stage.Has(BlockId::Companion)))
            return "an owner needs pulls and the companion block";
        if (arena.PartyGroup && (!arena.Owner || arena.Seats != SeatPlan::Party || !stage.Has(BlockId::Party)))
            return "a party group needs an owner, party seats and the party block";
        if ((arena.Seats == SeatPlan::Mirror) != (arena.Against == Opposition::MirrorSeat))
            return "mirror seats go with fighting the mirror seat, and only with it";
        if (player && !stage.Has(BlockId::Pvp))
            return "fighting a player needs the pvp block";
        if (player != arena.Pvp)
            return "an arena is pvp exactly when it fights a player";

        return {};
    }

    /// Why `stage` cannot be used, or empty. `valid` holds the stages accepted so far.
    std::string Problem(StageDefinition const& stage, std::vector<StageDefinition> const& valid)
    {
        if (stage.Blocks.empty() || stage.Blocks.front() != BlockId::Core)
            return "its blocks must start with core";

        for (std::size_t i = 0; i < stage.Blocks.size(); ++i)
            if (std::find(stage.Blocks.begin() + i + 1, stage.Blocks.end(), stage.Blocks[i]) != stage.Blocks.end())
                return "a block is listed twice";

        // The base only has to exist: seeding maps the base's blocks to this stage's by name (stage.json spans), so a
        // stage may drop base blocks it does not need and several stages may share a base.
        if (!stage.Extends.empty()
            && std::none_of(valid.begin(), valid.end(),
                [&stage](StageDefinition const& other) { return other.Name == stage.Extends; }))
            return "it extends " + stage.Extends + ", which is not an earlier valid stage";

        if (!stage.Has(BlockId::Duel))
            return "every stage fights something that fights back, which needs the duel block";

        if (stage.Arenas.empty() || stage.Arenas.size() > MAX_ARENAS)
            return "it needs 1 to " + std::to_string(MAX_ARENAS) + " arenas";

        for (std::size_t i = 0; i < stage.Arenas.size(); ++i)
        {
            ArenaDefinition const& arena = stage.Arenas[i];
            if (arena.Name.empty())
                return "an arena has no name";

            for (std::size_t j = i + 1; j < stage.Arenas.size(); ++j)
                if (stage.Arenas[j].Name == arena.Name)
                    return "arena " + arena.Name + " is listed twice";

            if (std::string const problem = ArenaProblem(stage, arena); !problem.empty())
                return "arena " + arena.Name + ": " + problem;
        }

        return {};
    }
}

uint32 AnimusForge::Curriculum::ArenaDefinition::SeatCount() const
{
    switch (Seats)
    {
        case SeatPlan::Party:  return MAX_SEATS;
        case SeatPlan::Mirror: return 2;
        case SeatPlan::Solo:   break;
    }

    return 1;
}

bool AnimusForge::Curriculum::StageDefinition::Has(BlockId block) const
{
    return std::find(Blocks.begin(), Blocks.end(), block) != Blocks.end();
}

uint32 AnimusForge::Curriculum::StageDefinition::SeatCount() const
{
    uint32 seats = 1;
    for (ArenaDefinition const& arena : Arenas)
        seats = std::max(seats, arena.SeatCount());

    return seats;
}

std::vector<AnimusForge::Curriculum::StageDefinition> const& AnimusForge::Curriculum::CurriculumStages()
{
    static std::vector<StageDefinition> const stages = []()
    {
        std::vector<StageDefinition> valid;
        for (StageDefinition& stage : Definitions())
        {
            if (std::string const problem = Problem(stage, valid); !problem.empty())
            {
                LOG_ERROR("module.animus", "Stage {} is left out: {}", stage.Name, problem);
                continue;
            }

            valid.push_back(std::move(stage));
        }

        return valid;
    }();

    return stages;
}

AnimusForge::Curriculum::StageDefinition const* AnimusForge::Curriculum::FindStage(std::string_view name)
{
    for (StageDefinition const& stage : CurriculumStages())
        if (stage.Name == name)
            return &stage;

    return nullptr;
}
