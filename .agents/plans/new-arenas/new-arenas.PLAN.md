# new-arenas: PvE and PvP in one branching curriculum, up to raids and battlegrounds

Branch: `new-arenas` (from `master` at f33267c). Status: the renames below and **P1 (arenas in stages)** are done,
with the pilot stage `mix_duel_pvp` (duel + scripted enemy player) to try it; P2 onward is still plan.

## Naming (done on this branch)

| Before | Now | Why |
|---|---|---|
| `src/Scenario/ClassRole/`, `AnimusForge::ClassRole` | `src/Scenario/Curriculum/`, `AnimusForge::Curriculum` | it is the whole curriculum; class/role is only the layout axis |
| `ClassRoleScenario`, `ClassRoleState.h` | `StageScenario`, `StageState.h` | one scenario plays one stage (and, with this plan, its arenas) |
| `ClassRoleTuning`, `ClassRoleStages()` | `CurriculumTuning`, `CurriculumStages()` | |
| `AnimusForge.ClassRole.<Group>.<Name>` | `AnimusForge.Curriculum.<Group>.<Name>` | this plan's `AnimusForge.Curriculum.Arena.*` keys fit next to them |
| `warrior_dummy_20`, `WarriorDummy20Scenario` | `bench_arms_warrior_20`, `Bench::ArmsWarriorBenchScenario` | a fixed benchmark and mechanics check, not a stage |
| `AnimusForge.WarriorDummy20.HsRageThreshold` | `AnimusForge.Bench.ArmsWarrior20.HsRageThreshold` | |
| `TrainingDummy::ClearSpawnArea` | `SpawnArea::Clear` | used by every scenario, nothing to do with dummies |
| `TrainingDummy::Spawn` | `Bench::TrainingDummy::Spawn` | only the bench uses it |

Kept: `ClassRoleProfile`, `ClassRoleAssets`, `AnimusForge.ClassRoles`, the manifest `class_role` key and layout names
(`warrior_dps`), so exported models and manifests are unchanged. mod-animus gets the matching rename on its own
`new-arenas` branch.

## 1. Goal

1. Use the branching curriculum to train PvE and PvP **at the same time**, into **one trunk and one model per
   class/role**. That model is what mod-animus needs: a companion that clears a dungeon and defends its owner from a
   gank, without swapping models.
2. Fold the seven existing stages into that structure instead of throwing them away.
3. Lay out the training that gets from a 5-player party and a 1v1 arena to **10/25-player raids** and **10v10-40v40
   PvP**.

## 2. Recommendation in one paragraph

Put **arenas** inside stages. An arena is one situation an episode can be: today's duel, pack, gauntlet, companion,
party, scripted PvP, or 1v1 self-play. A stage is then a weighted **mix of arenas** over the **union of their blocks**,
and each episode draws its arena. Add **merge nodes**: a stage may extend several stages, seeding each block from the
parent that trained it and **distilling** (kickstarting) each arena from that parent's checkpoint. The curriculum
stays a tree up to stage 5 and stage 7. Then `stage8_crossroads` merges both branches and adds cross arenas that need
PvE and PvP features in the same episode (a companion whose owner is ganked mid-pull). After the merge, the tree
branches again into a raid line and a mass-PvP line. Everything trains on **one machine, one stage at a time**: the
two lines are interleaved in queue order (a raid stage, then a PvP stage, ...), each keeping a rehearsal share of the
other line's most recently trained arenas so their trunks stay compatible. A final merge produces the model that plays
everything.

## 3. What exists today (the parts this plan depends on)

| Piece | Where | Relevant fact |
|---|---|---|
| Stage tree | `src/Scenario/Curriculum/Stages/Stages.cpp` | `duel ─┬─ pack ─ gauntlet ─ companion ─ party` / `└─ pvp ─ arena`; one `Extends` per stage |
| Stage shape | `Stages/StageDefinition.h` | `Seats`, `Against`, `Schedule`, `Owner`, `PartyGroup` are **per stage** |
| Blocks | `Layout/Block.h`, `Blocks/*` | Stateless. They write zeros when their `SeatView` part is null (`PvpBlock` returns without an opponent, `CompanionBlock` reads `view.Owner`) |
| Fixed sizes | `Block.h` | `MAX_SEATS=4`, `PARTY_MEMBERS=3`, `PACK_SLOTS=4`, `STABLE_SLOTS=4` |
| Encounters | `Encounters/Encounters.h` | Per-env state, created from the stage. `PullsEncounter` and `OwnerEncounter` read `_scenario.Stage()` (`PullsEncounter.cpp:97,145,488,556`, `OwnerEncounter.cpp:252`) |
| Stage-wide PvP switches | `StageScenario.cpp:630,719,790` | Resilience gear and no self-resurrection come from `_stage.Has(BlockId::Pvp)` |
| Seat plan | `StageScenario.cpp:503-558` | Party draws 1-4 active seats; **empty seats already work** (layout 0, no-op only, reward 0) |
| Episode length | `ForgeConfig.cpp:74`, `EnvPool.cpp:34,47` | One global `EpisodeSeconds`, but `Env::EpisodeLengthMs` is per env |
| Critic state | `StageScenario.cpp:170,920-986` | `STATE_GLOBAL + MAX_SEATS*seat + PACK_SLOTS*enemy`, fixed slots |
| Eval seeds | `EnvPool.cpp:285-300` | Seed index *i* goes to **whichever env resets next**, so anything drawn in `Rebuild` after the reseed is seed-determined |
| Seeding | `python/animus/bootstrap.py`, `config.py:117` | One checkpoint: trunk copied; kept blocks remapped by `stage.json` spans; sizes must match per block |
| Network | `python/animus/mappo/networks.py` | Per-layout adapter/head around a shared trunk; the critic sums a state encoder with the layout adapter |
| Stage gates | `python/animus/stage.py`, `gates.py` | Convergence, target, restarts; baseline `fight`; stage 7 has no baseline (self-play) |
| Scripted players | `Encounters/ScriptedPlayer.*` | Owner (tank/heal/dps party logic) and PvP opponent; simple and tunable |
| Creature pool | `Encounters/Opponents.cpp:60-140` | Default-AI and cast-only SmartAI; **no C++-scripted creatures, no bosses** |
| Runs | `var/animus-forge/runs/` | **Empty**: nothing trained yet, so there are no checkpoints to stay compatible with |

The main weakness of the current tree for the product: `_party` and `_arena` models come from **two diverged trunks
with disjoint blocks**. `stage5_party` has no `pvp` block and `stage7_arena` has no pack/gauntlet/companion/party
blocks. Neither model can do the other's job, and mod-animus would have to pick a model per situation.

## 4. Training both on one machine

"At the same time" means **in the same model**, not simultaneously. Arena mixes put PvE and PvP episodes into one
stage and one trunk; the curriculum itself is one `AnimusForge.Queue` on one sim, one scenario at a time. That already
works today:

- A stage only needs its base (or, for a merge, its parents) trained first. Branches of the tree can go in any order.
- An empty `AnimusForge.Queue` trains every stage in definition order, and definitions list every base before the
  stages that extend it (§6 numbers the stages in the recommended order).
- `forge start` with `Queue.SkipFinished` carries on where training stopped; `forge pause`/`resume` survive a long
  queue across sessions.

Running stages on several sims at once would only save wall-clock time; nothing in this design depends on it.

Not needed: two `EnvPool`s and two learners in one sim. It would double the lock-step plumbing in `AnimusForge.cpp`
and still yield two trunks. Arena mixes get one trunk with less code.

## 5. Design

### 5.1 Arenas inside stages (C++)

```cpp
/// One situation an episode of a stage can be. A stage with one arena is today's stage.
struct ArenaDefinition
{
    std::string Name;               // "party", "arena_1v1", "ambush" (episode info, eval tables, tuning keys)
    uint32 Weight = 1;              // share of episodes; AnimusForge.Curriculum.Arena.<stage>.<name>.Weight overrides
    SeatPlan Seats = SeatPlan::Solo;
    Opposition Against = Opposition::Creature;
    PullSchedule Schedule = PullSchedule::None;
    bool Owner = false;
    bool PartyGroup = false;
    bool Pvp = false;               // resilience gear, PvP flags, no self-resurrection
    uint32 EpisodeSeconds = 0;      // 0 = AnimusForge.EpisodeSeconds
};

struct StageDefinition
{
    std::string Name, Suffix, Summary;
    std::vector<std::string> Extends;   // first = primary parent (trunk); more = merge
    std::vector<BlockId> Blocks;        // union of what every arena needs, in layout order
    std::vector<ArenaDefinition> Arenas;
    // SeatCount() = the largest arena's
};
```

- **Validation** (`Problem`): every arena passes today's rules against the stage's blocks. Arena names are unique.
  Every name in `Extends` is an earlier valid stage.
- **Existing stages** get a single arena whose fields are their current `Seats`/`Against`/... Stages 1-7 behave
  exactly as now and keep their scenario names.
- **Per-episode arena draw**: `EnvState::Arena`, drawn at the top of `StageScenario::Rebuild`. `EnvPool` has
  already reseeded by then, so evaluation seed *i* always gets the same arena whatever the env count.
- **Stage reads become arena reads**: `_stage.Seats/Against/Schedule/Owner/PartyGroup/Has(Pvp)` →
  `ArenaOf(env).X`, at the lines listed in §3. `Encounter` gets
  `[[nodiscard]] bool Uses(Env const&) const` from the arena. Every hook returns early when unused, and a new
  `virtual void Deactivate(Env&)` removes the owner bot, scripted opponent or sim group when an env switches to an
  arena that does not use them. It runs from `BeforeRebuild`, before the seats are replaced, as the party already
  requires.
- **Encounters are created for the union** of the stage's arenas. Reward order stays
  `creature, pulls, owner, party, opponent`.
- **Seats**: `AgentsPerEnv` = the largest arena's seat count. Smaller arenas leave seats empty through the existing
  `ActiveSeats` path. Mirror and party spawn placement move into the arena.
- **Episode length**: `Rebuild` sets `env.EpisodeLengthMs` from the arena, so gauntlet-type arenas get minutes and
  duels 60 s in one run.
- **Episode info**: a new `arena` column (index). Encounter columns are the union and read 0 when unused, like
  pulls columns in PvP today.
- **stage.json** (format 2): `arenas: [{name, weight, seats, episode_seconds, pvp}]`, and `extends` as a list plus
  `seed_parents` (below). `seed_chain` stays: the primary parent's chain.

### 5.2 A `context` block (obs only, deployable)

Blocks must stay fillable by mod-animus on a live server, so the **actor never sees an arena id**. It sees the
situation instead, which mod-animus can compute:

- living allies (owner, group members) and group size;
- hostile players and hostile creatures near the bot, counts and nearest distance;
- whether the bot is PvP-flagged, on a battleground/arena map, or in an instance;
- whether self-resurrection is allowed, and whether the fight is against players.

It is a new block, so it seeds at zero and does not break the core/duel sizes. The **critic** additionally gets an
arena one-hot (`STATE_ARENA_FIRST`, 16 slots). The critic is never exported, and a fresh critic state encoder is
already the seeding rule.

### 5.3 A `hostiles` block: enemy players in enemy slots

Mixed episodes have creatures and players in the same 4 `PACK_SLOTS` (the scripted opponent already goes into
`env.Targets`). Per slot, add what the pack block lacks: is-player, class one-hot, role guess, casting a heal,
crowd-controlled, stealthed. This is a new block rather than a resized `pack`, so pack keeps its seeded weights.
`pvp` keeps describing the focused enemy player.

### 5.4 Merge nodes: multi-parent seeding (learner)

`stage.json.seed_parents: [{stage: "stage5_party", blocks: [...]}, {stage: "stage7_arena", blocks: [...]}]`,
primary first.

- **Trunk**: from the primary parent. Pick the PvE parent: it has trained more stages and has the larger block set.
- **Blocks**: each block comes from the first parent that has it. `core`/`duel` come from the primary; `pvp` comes
  from `stage7_arena`. New blocks (`context`, `hostiles`) start at zero.
- `bootstrap.seed_trainer` generalises to a list of `(checkpoint, stage, blocks)`. `_seed_trunk` runs only for the
  first. `_seed_adapter_blocks` and `_seed_head_blocks` stop zeroing between parents: zero once, then fill per parent.
- The secondary parent's `pvp` columns were trained against a different trunk. Both trunks descend from the duel, so
  they are a warm start, not a correct one. Distillation fixes that.

### 5.5 Distillation per arena (kickstarting)

```yaml
distill:
  teachers:                     # arena -> checkpoint whose policy already plays it
    party:        "{runs_dir}/stage5_party/best.pt"
    companion:    "{runs_dir}/stage4_companion/best.pt"
    gauntlet:     "{runs_dir}/stage3_gauntlet/best.pt"
    pvp_scripted: "{runs_dir}/stage6_pvp/best.pt"
    arena_1v1:    "{runs_dir}/stage7_arena/best.pt"
  coef: 1.0                     # KL(teacher || student) weight added to the policy loss
  half_life_env_steps: 30000000 # decays towards 0, so PPO takes over
  min_coef: 0.0
```

- **Per-step arena id**: add an arena field per env to `STEP` and the arena names to `SPEC` (protocol version bump,
  `Protocol.h` and `protocol.py`). Then the buffer knows each row's arena.
- **Teacher inputs**: the teacher's observation is the student's row gathered through the block span map (a teacher's
  blocks ⊆ student's). Its logits scatter back to student action indices. The KL is over actions both allow, with
  the student's mask renormalised on that set. Rows of arenas without a teacher (new cross arenas) get no KL term.
- Teachers are frozen actors on the rollout device, run only in the update, batched per layout like `LayoutActor`.
- Cost: one extra forward per teacher per minibatch. That is small next to the sim.
- Because KL decays, a merge cannot keep a teacher's mistakes; gates (§5.7) still require beating the baseline per
  arena.

### 5.6 Rewards and values across arenas

- Return scales differ a lot (a 60 s duel vs a 10 min gauntlet with owner penalties). Add
  `AnimusForge.Curriculum.Arena.<stage>.<name>.RewardScale` (default 1) and report `reward_per_decision` per arena in
  eval tables. Tune scales so per-arena mean |return| lands within about 3x of each other.
- Shared `ValueNorm` first, with the critic reading the arena one-hot. If value loss is dominated by one arena, add
  **per-arena value heads**: `LayoutCritic.head` becomes `heads[arena]` indexed by the STEP arena id, with a
  `ValueNorm` per arena.
- `gamma: 0.999` for mixed stages (the gauntlet/pvp value). PPO advantage normalisation stays per minibatch.

### 5.7 Evaluation and gates per arena

- Eval summaries grouped by arena, as level bands are today (`eval_<arena>/*`, tables in `eval.jsonl`).
- Target gates per arena:
  ```yaml
  target:
    min_over_baseline: 0.1
    arenas:
      party:     {min_over_baseline: 0.1, metrics: {wipes: {max: 0.3}}}
      companion: {min_over_baseline: 0.1}
      arena_1v1: {metrics: {won: {min: 0.55}}}   # vs the baseline seat, see below
  ```
- **Mirror arenas in evaluation**: seat 1 plays the baseline (`MODE` gains a per-seat "scripted" flag) so `won`
  measures learner-vs-`fight`, not learner-vs-itself. This also gives stage 7 a real score, which it lacks today
  (`stage7_arena.yaml` disables convergence).
- `min_arena_episodes`, like `min_layout_episodes`, keeps a small arena from being judged on noise.

## 6. The new curriculum

```
stage1_duel ─┬─ stage2_pack ─ stage3_gauntlet ─ stage4_companion ─ stage5_party ─┐
             └─ stage6_pvp ─ stage7_arena ───────────────────────────────────────┤
                                                                                 ▼
                                            stage8_crossroads  (merge + cross arenas)
                                              ├─ stage9_dungeon ─ stage11_raid10 ─ stage13_raid25            (PvE line)
                                              └─ stage10_skirmish ─ stage12_battleground ─ stage14_warfront  (PvP line)
                                                                                 ▼
                                            stage15_world  (merge of raid25 + warfront: the model mod-animus plays)
```

**One machine, one queue.** Stage numbers are the training order, and definitions follow it, so an empty
`AnimusForge.Queue` trains 1, 2, ..., 15 on one sim, and every base comes before the stages that extend it. After
stage 8 the lines are **interleaved**: dungeon, skirmish, raid10, battleground, raid25, warfront. Each line's rehearsal
teachers are then the other line's latest `best.pt`, at a similar level, so the trunks drift less and the final merge
stays short. Line by line (`AnimusForge.Queue = "stage9_dungeon, stage11_raid10, stage13_raid25, stage10_skirmish,
stage12_battleground, stage14_warfront, stage15_world"`) also works, with staler rehearsal.

Every stage from 8 on has the union blocks, so **each line keeps a rehearsal share (15-20%) of the other line's most
recently trained arenas**. It is cheap, it stops forgetting, and it keeps the two trunks compatible. The rehearsal
teachers in `distill.teachers` point at the other line's latest finished stage: `stage9_dungeon` rehearses PvP arenas
from `stage8_crossroads`, `stage11_raid10` rehearses skirmish arenas from `stage10_skirmish`, and so on.

### 6.1 Folding the existing stages in

| Existing stage | Keeps its own run | Becomes arena | Where it lives afterwards |
|---|---|---|---|
| `stage1_duel` | yes (root) | `duel` | 5% rehearsal in stage 8 (the bot playing alone) |
| `stage2_pack` | yes | `pack` | absorbed by `gauntlet` (a single-pull gauntlet); 0-5% |
| `stage3_gauntlet` | yes | `gauntlet` | stage 8 rehearsal 10%: solo sustain, food/drink |
| `stage4_companion` | yes | `companion` | stage 8 core arena, 20%: mod-animus's main use |
| `stage5_party` | yes | `party` | stage 8 20%; stage 9 builds on it |
| `stage6_pvp` | yes | `pvp_scripted` | stage 8 10% and eval anchor for every PvP stage |
| `stage7_arena` | yes | `arena_1v1` | stage 8 15%; stage 10 builds on it |

Stages 1-7 stay as they are: single-arena stages under the new definition, repeatable, with gates. Nothing is trained
yet, so any tuning changes they need can land before the first pilot run.

### 6.2 `stage8_crossroads`: the merge

Blocks: `core, duel, pack, gauntlet, companion, party, pvp, context, hostiles`. Extends
`stage5_party, stage7_arena`. `AgentsPerEnv = 4`.

| Arena | Weight | What it is | Why |
|---|---|---|---|
| `companion` | 20 | stage 4 | the product |
| `party` | 20 | stage 5 | group PvE |
| `arena_1v1` | 15 | stage 7 | PvP skill |
| `pvp_scripted` | 10 | stage 6 | anchor, stable signal |
| `gauntlet` | 10 | stage 3 | solo sustain |
| `duel` | 5 | stage 1 | fundamentals |
| **`ambush`** (new) | 10 | companion + owner mid-gauntlet; 1-2 scripted enemy players attack the owner at a random pull | forces pack and pvp features into one decision: drop the pull, peel for the owner, target the healer |
| **`escort_duel`** (new) | 5 | companion + owner vs 1 scripted enemy player (world PvP) | companion block with a player enemy |
| **`duo_arena`** (new) | 5 | 2v2 self-play, companion-shaped: each side is 1 learned seat + 1 scripted owner | owner protection under PvP pressure |

New code: `ambush`/`escort_duel` need `OpponentEncounter` to run beside `PullsEncounter` and `OwnerEncounter` (target
the owner, spawn at a pull), and the opponent in a `Targets` slot next to creatures. `duo_arena` needs `SeatPlan::Teams`
(§8.1).

Gates: every arena at or above its teacher's eval score minus 5%, and the new arenas above `fight`.

## 7. The PvE line: what training gets to raids

### 7.1 Movement and targeting the current blocks cannot express

Raids fail on positioning and friendly targeting, not rotations. New blocks, all situational and deployable:

- **`hazards`** (obs + actions): the nearest 4 ground hazards (`DynamicObject`s and area-aura/trigger creatures
  damaging the bot's faction). Features: relative position, radius, time left, inside or not. Actions: step out of
  hazard *k*, the 8 compass steps relative to the current target (5 yd), stack on the tank, spread from the nearest
  ally.
- **`boss`**: the current boss's cast (spell school, interruptible, time left, targets me), the bot's threat as a
  fraction of the tank's, facing (am I in front), and marks (the raid target icon on each enemy slot). Marks are
  also the channel for leader intent (§9).
- **`raidframes`**: up to 25 compact ally slots sorted by group then role (present, alive, health, in range, role,
  has my HoT/shield, dispellable debuff type, taking avoidable damage). Plus **factorised friendly targeting**:
  "select ally slot *k*", then heals and dispels cast on the selected ally. This is the same select-then-act pattern
  the pack block uses for enemy slots. It avoids `heals × members` action heads (a 25-player raid would need ~150
  heal actions).
- **`party`** stays for the 5-player group view; `raidframes` supersedes it at raid scale.

### 7.2 Stage 9 `stage9_dungeon` (5 players): boss drills, then real bosses

A `BossEncounter` with composable, randomised **mechanics**. Each is built from real spell data and sim-owned trigger
creatures, so the observation goes through the generic blocks above, never through boss-specific one-hots:

| Drill arena | Mechanic | Teaches | Measured by |
|---|---|---|---|
| `tank_and_spank` | high-health elite, enrage timer tuned to party DPS | DPS check, healer mana, threat | kill before enrage |
| `void_zones` | ground AoE under random players every 6-12 s | move out, then back to work | avoidable damage taken |
| `frontal` | cone cleave and tail swipe | position behind/side, tank faces away | cleave hits on non-tanks |
| `big_cast` | interruptible nuke every 15 s | interrupt rotation | interrupts, damage taken |
| `adds` | add waves | switch, AoE, off-tank pickup | adds alive time, healer hit |
| `stacking_debuff` | tank debuff stacks (2 tanks) | taunt swap | stack peak, tank deaths |
| `spread_stack` | chain/proximity damage vs shared split damage | spread or stack on cue | chain jumps / soak count |
| `dispel` | magic/curse/poison/disease on allies | dispel priority | debuff uptime |

Episodes draw 1-3 mechanics, boss health scales with the party's level damage scale, and pulls of real dungeon
trash precede the boss. After drills pass their gates, add **real 5-player bosses**: env maps become the real dungeon
maps (envs are already instance maps; `SpawnPoint` becomes per arena). Spawn the party at the boss's room. Use an
**allowlist** of self-contained bosses (Utgarde Keep, Nexus, Azjol-Nerub, Ahn'kahet, Drak'Tharon, Gundrak, Halls of
Stone/Lightning, Utgarde Pinnacle, Forge of Souls, Pit of Saron, plus classic/TBC ones per level band). Skip
event/vehicle bosses (Violet Hold, Oculus, Culling of Stratholme, Halls of Reflection, Trial of the Champion).

Rewards: shared boss kill and shared wipe penalty, individual role terms as in stage 5, and a heavy
**avoidable-damage** term. That term tags damage by the mechanic's spell ids, like "damage taken from avoidable
abilities" in combat logs.

Levels: a boss arena draws from its content's level band. Gear adds dungeon-tier bands per expansion to
`GearBuilder` `ITEM_LEVEL_ANCHORS`.

### 7.3 Stages 11 and 13 `stage11_raid10`, `stage13_raid25`

- `SeatPlan::Raid{10|25}`: all seats learned. Role makeup 2/2-3/rest at 10, 2-3/5-6/rest at 25. Some episodes have
  scripted fillers so policies learn with imperfect allies (and with the mod-animus player).
- **Drills scaled up** (two tanks, split healing assignments, raid-wide spread), then **Naxxramas as the raid
  curriculum**. Its bosses are nearly one mechanic each: Patchwerk (healing and hateful strike),
  Grobbulus (spread), Heigan (dance), Loatheb (healing windows), Four Horsemen (tank rotation), Sapphiron (line of
  sight), Thaddius (polarity), Instructor Razuvious (the mind-control tanking may need to be skipped). Then Obsidian
  Sanctum, then selected Ulduar/ICC bosses. Level-60/70 raids optional for band coverage; raid arenas pin levels to 60, 70 or 80.
- Gear: raid item level bands (Naxx 200/213, Ulduar 219/226, ToC 232-258, ICC 251-277) as new anchors.
- Credit assignment at 25 agents: shared terms are scaled per seat (`1/√n`) and individual role terms dominate
  shaping. The critic moves to a set encoder (§8.2). Wipe = episode ends (no recovery loop as in the party stage).

## 8. The PvP line: what training gets to large battles

### 8.1 Stage 10 `stage10_skirmish`: team arenas with a league

- `SeatPlan::Teams{n}`: 2n learned seats, two factions, generalising `Mirror`. Arenas `2v2`, `3v3`, `5v5`, plus
  uneven `2v3`, `1v2` (survive and trade), and a gank-defense arena (owner + companion vs 2-3).
- **Real arena maps** as spawn points: Nagrand (559), Blade's Edge (562), Ruins of Lordaeron (572) and Dalaran Sewers
  (617) have line-of-sight pillars. Ring of Valor (618) has moving pillars and comes later. **Spike first**: these are
  battleground maps, and creating them as plain instance maps for bots may need core work. Fallback: open terrain with
  spawned LoS game objects.
- **Opponent league (learner-only)**: the learner already chooses every agent's action, so the opposing team can be
  a **frozen snapshot** without sim changes. A snapshot is saved every N updates. Opponents are sampled 50% latest
  self, 30% past snapshots (prioritised by how often they beat the latest), 20% scripted anchors. Frozen seats are
  excluded from the buffer. `league.json` keeps an Elo table. This prevents strategy cycling, the known failure mode
  of pure self-play.
- New PvP observations in `hostiles`/`pvp`: diminishing-returns state per CC category on self and focus target, PvP
  trinket ready, enemy healer LoS/range, enemy defensive cooldown active. Actions: focus target on slot *k*,
  PvP trinket, LoS step (move to break LoS from slot *k*), peel on ally slot *k*.
- Rewards: team win (shared), kill participation, damage/healing as now, CC uptime on the kill target, a DR-waste
  penalty, and a death penalty weighted by team size.
- Eval: `won` vs `fight` teams on seeds, and Elo vs a fixed reference pool (the stage 7 best and scripted anchors).

### 8.2 Stages 12 and 14 `stage12_battleground`, `stage14_warfront`

- **Objective encounter first, real battlegrounds later.** A sim-owned `ObjectiveEncounter` on open terrain: capture
  points (hold 10 yd uncontested for 8 s; Arathi-style nodes), a flag carry (pick up, run, return), and respawn
  waves every 30 s at a graveyard. Deaths do not end the episode; episodes run 8-15 min. Then real Warsong Gulch
  (10v10) and Arathi Basin (15v15) maps, after the arena-map spike tells us what `BattlegroundMap` needs.
- Scale: 10v10 → 15v15 with fixed-K entity slots (8 allies, 8 hostiles, sorted by relevance: my attackers, my
  target, healers, lowest health, marked). 40v40 (Alterac Valley) is not a flat-MAPPO target: 80 learned agents per
  env would need a few envs of 80 bots with minute-long credit assignment.
  Plan it as **hierarchical**: a commander (scripted first, learned later) writes an objective/intent vector
  (§9) and the per-agent policy executes it.
- **Critic as a set encoder**: the fixed `MAX_SEATS*seat + PACK_SLOTS*enemy` state cannot hold 30 players. The critic
  becomes an entity table (allies, hostiles, objectives with presence flags) encoded with DeepSets or attention. The
  critic is never exported, so the actor keeps the plain-MLP `.amdl` format mod-animus loads.
- Rewards: objective ticks (shared), capture/defend participation near the objective, kills, healing, and time spent
  in useful zones. The gamma for battlegrounds is 0.9995.

## 9. Cross-cutting: architecture changes for scale

| Change | Needed by | Notes |
|---|---|---|
| `MAX_SEATS` 4 → config (up to 40) | raid10+, skirmish 5v5+ | `BotAccounts` ranges, `BotSlot` GUID pairs, `EnvState::Seats` arrays → vectors |
| `SeatView` generic `Allies`/`Hostiles` lists | stage 8+ | `Owner`, `Teammates`, `Opponent` become views over them; old blocks unchanged |
| Fixed-K sorted entity blocks | raids, battlegrounds | keeps the actor an MLP; sorting rules are the manifest's contract with mod-animus |
| **Intent block** (marks, objective vector, "focus", "stack/spread") | raid25, battlegrounds, mod-animus | the same features a mod-animus player fills by marking targets or giving orders; commanders train against it |
| Per-arena `SpawnPoint`, `EpisodeSeconds`, level band | stage 8+ | `ArenaDefinition` |
| Protocol: arena id per env, per-seat frozen/scripted flags, entity-table state | §5.5, §5.7, §8 | bump `Protocol.h` / `protocol.py` together |
| Per-arena value heads | if value loss is unbalanced | learner only |
| `DecisionTicks` per arena (200 ms for 20+ players) | battlegrounds | throughput; decisions stay lock-step |
| Throughput | raids | 25 bots × 16 envs = 400 players; profile `MapUpdate.Threads` and per-decision Python round-trip before stage 11 |

**mod-animus impact** (separate repo, built separately; models copied by hand): every stage from 8 on produces
manifests with `context`/`hostiles` blocks, and later with `hazards`/`boss`/`raidframes`/`intent`. mod-animus must
fill each block exactly as the manifest says. Plan a manifest-format bump (4) and implement the fillers there, block
by block, as each stage lands. No code moves between the modules.

## 10. Phases, in order

Each phase ends with pytest green (in the `ac-dev-server` venv; the host has no torch) and a syntax-only compile
(`g++ -fsyntax-only -std=gnu++20` in the dev-server image). The user runs pilots and end-to-end checks.

| # | Phase | Code | Done when |
|---|---|---|---|
| P0 | Pilot the tree as is | none; stages 1-7 in queue order on one sim | stages 1-7 pass gates; target numbers tuned from `eval.jsonl` |
| P1 | Arenas in stages | `ArenaDefinition`; stage reads → arena reads; `Encounter::Uses/Deactivate`; per-env episode length; `arena` episode info; stage.json format 2 | stages 1-7 byte-identical `stage.json` layouts; a test stage mixing `duel`+`pvp_scripted` runs with seeded evals stable across env counts |
| P2 | Per-arena eval and gates | `animus/evaluation.py` grouping, `gates.py` `arenas:`, `MODE` per-seat scripted flag | stage 7 reports `won` vs `fight`; per-arena gates tested |
| P3 | Merge seeding and distillation | `Extends` list, `seed_parents`; `bootstrap.py` multi-parent; protocol arena id; `distill:` in trainer | tests: block provenance per parent; KL is 0 when student == teacher; action/obs remap round-trips |
| P4 | `context` + `hostiles` blocks, cross arenas | new blocks; `OpponentEncounter` beside pulls and owner; `ambush`, `escort_duel` | `stage8_crossroads` passes per-arena gates |
| P5 | Raid and PvP lines, interleaved (stages 9-14 in queue order) | each stage's code lands before it trains. Stage 9: `hazards`, `boss`, `BossEncounter` + drills, boss allowlist. Stage 10: `SeatPlan::Teams`, league in learner, arena-map spike. Stage 11: `raidframes`, factorised ally targeting, raid seat plans, gear bands. Stage 12: `ObjectiveEncounter`, set-encoder critic. Stages 13-14: scale-ups | stage 9 drills → real 5-player bosses → Naxx 10 → 25; skirmish Elo climbs vs reference pool; 10v10 objective win rate vs scripted |
| P6 | Final merge | `stage15_world` with distillation from 13 and 14 | every arena within 5% of its teacher; mod-animus manifest v4 fillers done |

P1 and P2 are the enablers and change no behaviour of stages 1-7. P3 and P4 give the first single PvE+PvP model.
P5 is long. The branching payoff is not parallelism: one queue on one machine trains every model, each stage stays
repeatable on its own, and each seeds only from what has already been trained.

## 11. Risks and open questions

- **Arena and battleground maps for bots**: `BattlegroundMap` needs a `Battleground`; the sim creates instance maps
  per env. Spike before stage 10; open-terrain fallback is in §8.1.
- **One machine**: total wall-clock time is the sum of the stages. `forge pause`/`forge resume` and
  `Queue.SkipFinished` let the queue run across restarts. Distillation loads up to ~5 frozen teacher actors; they are
  small MLPs (a few hundred MB of GPU/CPU memory at most), so they fit beside the learner.
- **Real boss scripts in a reset-in-place sim**: `InstanceScript` boss states, doors and respawn timers persist across
  episodes. Resetting needs `SetBossState(NOT_STARTED)`, a respawn, and possibly door game objects. Spike on one
  boss (Utgarde Keep's Prince Keleseth) before the allowlist grows.
- **Negative transfer in the merged trunk**: PvP opens with burst while PvE opens with threat. The `context` block
  plus distillation should separate them. Watch per-arena scores against teachers during the KL decay; if one
  arena sinks, raise its weight or slow the half-life.
- **Empty-seat compute**: 1v1 episodes in a 4-seat (or 25-seat) pool waste agent slots. It is acceptable at 4 seats;
  at raid scale keep small arenas out of raid stages except a 5% rehearsal share.
- **Value scale imbalance** across arenas: see §5.6. Measure first, add heads if needed.
- **Scripted-player quality** caps what PvE-with-owner and PvP-anchor arenas can teach; raising their skill is
  tuning (`ScriptedPlayers.*`), and league self-play takes over for PvP.
- **Open question for the user**: should raids and battlegrounds pin level 80 only, or cover 60 and 70 content too?
  Pinning 80 cuts content work in half. Covering all three matches how the curriculum samples levels today.
