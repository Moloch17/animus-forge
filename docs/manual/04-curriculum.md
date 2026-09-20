# 4. The curriculum

The curriculum is the set of scenarios the policies train on. It lives in animus-lib under
`src/Scenario/Curriculum/`. Twenty-four scenarios are defined; **twenty-three are the default queue**, in the
order they are trained, and one (`mix_duel_pvp`) is a pilot trained only by name.

Every stage trains the same eighteen class/role policies over a shared trunk, so what one class learns about
moving, threat or interrupts helps the others. A stage names the one it `Extends`, and its networks are seeded
from that stage's best checkpoint block by block: blocks it keeps carry over, blocks it drops are left behind,
blocks it adds start from nothing. That makes the curriculum a tree, not a line -- but the numbers now sort
into the training order, so `forge start` with no arguments walks the whole thing from stage 1 to stage 23 and
never reaches a stage before the stage it seeds from.

```
stage1_duel
├─ stage2_pack
│  └─ stage3_hazards
│     └─ stage4_gauntlet
│        └─ stage5_endurance
│           └─ stage9_companion
│              └─ stage10_party
│                 └─ stage11_tanking
│                    └─ stage12_triage
│                       └─ stage13_raid_single
│                          └─ stage14_raid_gauntlet
│                             └─ stage23_crossroads   (+ 6 merges)
├─ stage6_run
│  └─ stage7_travel
│     └─ stage8_flight
└─ stage15_pvp
   ├─ stage16_evade
   │  └─ stage17_hide
   │     ├─ stage18_stealth                           (a leaf: four layouts)
   │     └─ stage19_arena
   │        ├─ stage20_duo_led
   │        └─ stage21_flag                           (+ merges stage7_travel)
   │           └─ stage22_warsong
   └─ mix_duel_pvp                                    (a pilot, trained by name)
```

`stage23_crossroads` extends `stage14_raid_gauntlet` and merges `stage22_warsong`, `stage19_arena`,
`stage15_pvp`, `stage8_flight`, `stage9_companion`, `stage4_gauntlet` and `stage1_duel`: it is where the PvE
line, the PvP line and the travel line become one policy.

> **One stage is restricted, and it must stay a leaf.** `stage18_stealth` is played by the four class/roles
> whose own kit carries a stealth aura, because closing on someone unseen is a thing only a real stealth aura
> can do. Its checkpoint therefore holds four of the eighteen layouts, and `init_from: auto` takes the **first
> checkpoint in the chain that exists** -- so a stage seeding from it would find that one, stop looking, and
> start the other fourteen from random weights without saying so. `stage19_arena` extends `stage17_hide`,
> reaching past it, and `Problem()` in `Stages.cpp` refuses anything that tries to extend or merge a
> `NeedsStealth` stage. Every other stage is played by all eighteen, each drawing its races as usual, so every
> class/role meets its own kit and its own racials.

## The stages

Seats are the learned agents an episode runs: `Solo` is one, `Party` up to five, `Raid` eight groups of five,
`Mirror` two seats fighting each other, `Teams` two sides of `TeamSeats`. A directed arena adds two more agents,
one commanding each side (see 4.12).

| Stage | Extends | Seats | Blocks added | What it is |
|---|---|---|---|---|
| `stage1_duel` | — | Solo | core, duel, pet | A same-level creature out of aggro range: close in and kill it fast, taking little damage |
| `stage2_pack` | stage1_duel | Solo | + pack | A pack of 2-4, casters included, usually linked: targets, interrupts, crowd control |
| `stage3_hazards` | stage2_pack | Solo | + support | **Drill.** A pack with something on the ground in *every* pull, rather than only from ladder rung 3 up, so walking out of one is learnable on its own |
| `stage4_gauntlet` | stage3_hazards | Solo | + gauntlet | Pull after pull with short breaks: heals, food and drink |
| `stage5_endurance` | stage4_gauntlet | Solo | same | **Drill.** A known run of eight pulls, won by finishing it: 900 s, ending on an elite pack two levels up |
| `stage6_run` | stage1_duel | Solo | + travel (−pack) | A place 40-160 yd away **on foot**: mounting is masked, so the trip is made with the speed cooldowns the class has |
| `stage7_travel` | stage6_run | Solo | same | A place 60-320 yd away by path: mount when it pays, get there, arrive on foot. Level 20+ |
| `stage8_flight` | stage7_travel | Solo | same | A place 350-700 yd away in Nagrand: take off, fly over what is in the way, land, dismount. Level 60+ |
| `stage9_companion` | stage5_endurance | Solo | + companion | The gauntlet beside a scripted owner: follow, assist, guard and heal it |
| `stage10_party` | stage9_companion | Party | + party | Four learned seats and the scripted owner against elite-heavy pulls |
| `stage11_tanking` | stage10_party | Party | same | **Drill.** Seat 0 is always the tank: hold what the pull brings, and keep it off the others |
| `stage12_triage` | stage11_tanking | Party | same | **Drill.** Seat 0 is always the healer: keep the hurt one up, and spend mana to do it |
| `stage13_raid_single` | stage12_triage | Raid | same | A raid of eight groups against one elite and its adds, won or lost as the single pack is |
| `stage14_raid_gauntlet` | stage13_raid_single | Raid | same | A raid clearing pull after pull, recovering between them |
| `stage15_pvp` | stage1_duel | Solo | + pvp (−pack) | One-on-one against a scripted enemy player |
| `stage16_evade` | stage15_pvp | Solo | same | **Drill.** A scripted enemy player ten levels up for 120 s: the fight cannot be won, so the score is being alive at the end. Break away, break line of sight, use the class's escape |
| `stage17_hide` | stage16_evade | Solo | same | **Drill.** The same fight six levels up, for every class and race: get out of sight and stay there, and hide again after being found. Terrain, distance, Blink, Disengage, Feign Death, Invisibility, Vanish, Prowl, Shadowmeld -- whatever the kit and the race give it |
| `stage18_stealth` | stage17_hide | Solo | same | **Drill, and a leaf.** For the four class/roles whose own kit carries a stealth aura (rogue and the three druids): close on a stronger enemy unseen, hold inside strike range, and open from it. Shadowmeld does not qualify -- it breaks on movement, so it cannot close on anything |
| `stage19_arena` | stage17_hide | Mirror | same | Self-play one-on-one: two learned seats of any classes |
| `stage20_duo_led` | stage19_arena | Teams (2) | + pack, context, hostiles, support, order | Two against two under a **director**: told who to kill, whose turn it is, and where to go (4.12) |
| `stage21_flag` | stage19_arena (+ stage7_travel) | Mirror | + travel, flag | Capture the flag one-on-one: bases 100-180 yd apart, first to three captures. Level 20+ |
| `stage22_warsong` | stage21_flag | Teams (10) | + party | Ten against ten for the flag on a real Warsong Gulch instance: escort the carrier, hold the base, stop theirs |
| `stage23_crossroads` | stage14_raid_gauntlet (+ 6 merges) | Mirror/Party | + pvp, context, hostiles | PvE and PvP in one policy: every earlier situation, an ambush mid-gauntlet, a ganked owner |

### Trained by name

| Stage | Extends | Seats | What it is for |
|---|---|---|---|
| `mix_duel_pvp` | stage15_pvp | Solo | **Pilot.** Half the episodes a creature duel, half a scripted enemy player. Exists to test arena mixing, merge seeding and distillation on a small problem |

**A drill** fixes what one episode is about, where the curriculum otherwise teaches the same skill inside a stage
won by something else and the credit for it is smeared over the clear. Drills are now *on* the trunk rather than
beside it (`stage4_gauntlet` seeds from `stage3_hazards`, `stage9_companion` from `stage5_endurance`,
`stage12_triage` from `stage11_tanking`), so a drill is never a dead end whose lesson nothing inherits.

Two things to know before reading a drill's scores. The hazard charge lands about four times harder on a tank
than on a ranged seat, because a tank cannot walk out of what it is holding an enemy in. And a forced-healer
stage will find any fault in the resurrection path faster than anything else in the curriculum -- it found the
farmable revive described in 4.6.

## 4.1 Defining a stage

A stage is one `StageDefinition` entry in `Stages/Stages.cpp`:

```cpp
stages.push_back({
    .Name = "stage9_companion",          // scenario name
    .Suffix = "_companion",              // model names: warrior_tank_companion
    .Extends = "stage4_gauntlet",        // seeds from it (the trunk)
    .Summary = "the gauntlet beside a scripted owner: follow, assist, guard and heal it",
    .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion },   // layout order
    .Arenas = { { .Name = "companion", .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                  .Owner = true } },
});
```

An `ArenaDefinition` describes one situation:

| Field | Values |
|---|---|
| `Name` | Unique within the stage. Used in episode info, `stage.json`, tuning keys and per-arena gates |
| `Weight` | Share of episodes, overridable with `<TuningPrefix>Arena.<stage>.<arena>.Weight` |
| `Seats` | `Solo` (1), `Party` (4 slots beside the owner, 1-4 filled each episode), `Mirror` (2 that fight each other), `Raid` (40: eight groups of five, a tank and a healer at the head of each) |
| `Against` | `Creature`, `Pulls`, `ScriptedPlayer`, `MirrorSeat`, `Ambush`, `Travel` (a place to get to), `Flag` (a flag match between mirror seats) |
| `Schedule` | `None`, `SinglePack` (ends on clear), `Gauntlet` (pull after pull) |
| `Owner` | A scripted owner the seats fight for |
| `PartyGroup` | The owner and seats form a core group |
| `Pvp` | Resilience gear, no self-resurrection |
| `EpisodeSeconds` | 0 = the host's `EpisodeSeconds` |
| `Ambushers` | 0-2 scripted enemy players who attack the owner |
| `Flying` | Travel: the objective is far enough that flying beats riding |

A `StageDefinition` may also name its own `MapId` and `SpawnPoints` (0 = the host's `SpawnMapId` and `SpawnPosition`)
and a `MinLevel` that raises every character's level, a fixed host level included. On a continent (a map that isn't
instanceable, like Outland for `stage8_flight`) every env shares the map: each env's seats take one of the spawn
points by env index and live in their own phase (`StageScenario::EnvPhase`), so no env sees another's.

**Validation.** `CurriculumStages()` checks each definition in order and leaves out (with an error log) any stage
that:

- doesn't start with the `core` block, lists a block twice, or lacks `duel` (every stage fights something that fights
  back),
- extends or merges a stage that isn't an earlier valid stage, merges its base or the same stage twice, or merges
  without extending,
- has no arenas, more than `MAX_ARENAS` (8), or two arenas with the same name,
- has an inconsistent arena:
  - a pull schedule without pulls, or pulls without a schedule
  - pulls without `pack`, or a gauntlet without `gauntlet`
  - an owner without pulls or an ambush, or without `companion`
  - a party group without an owner, party seats and `party`
  - mirror seats without `MirrorSeat` or `Flag`, or either without mirror seats
  - travel without `travel` or other than one seat on its own (no owner, PvP or ambushers), `Flying` without travel,
    or a flag match without `travel` and `flag`
  - more than 2 ambushers, ambushers without an owner and `pack`, or an `Ambush` arena with other than exactly one
    ambusher
  - fighting a player without `pvp`, a one-on-one against a player that isn't `Pvp`, or `Pvp` without a player

The base only has to exist. Seeding maps blocks by name, so a stage may drop base blocks it doesn't need, and several
stages may share a base.

## 4.2 How `StageScenario` runs a stage

`StageScenario` implements `Scenario` for any `StageDefinition`.

### Construction

1. **Tuning.** `CurriculumTuning::Load(settings.TuningPrefix)` reads every value (4.9). `DecisionScale =
   DecisionMs / 50`.
2. **Layouts.** One `Layout` per class/role in `StageSettings::ClassRoles` (or all 18) whose assets have at least one
   race. `Layout::Index` is its position in this list, which is the index the learner sees.
3. **Scripted-player assets.** If any arena has an owner or a scripted enemy player, every class/role's assets are
   built now, because those bots can be any class/role. Building takes seconds per class/role, and doing it now avoids
   stalling the world thread during an episode reset.
4. **Spec.** `AgentsPerEnv` is the largest arena's seat count. `ObsDim` and `NumActions` are the largest layout's.
   `StateDim` is fixed (4.8).
5. **Encounters**, created only if some arena needs them, in **build order**: opponent, owner, party group, pulls,
   creature, ambush. The owner comes before the party group (which it leads) and the pulls (which spawn around it).
   Ambushers come last because they find the owner and take the slots the pulls leave. **Reward order** (which is also
   the order of episode info columns) is creature, pulls, owner, party, opponent, ambush. For each arena the scenario
   keeps the subset it uses, in both orders, plus its weight and episode length.
6. **Pools.** The creature opponent pool and the consumable pool are loaded now rather than on the first episode.
7. **Episode info columns.** The core columns (4.10), then each encounter's, then one `reward_<term>` column per reward
   term any encounter pays.
8. **Stage files.** If `LayoutsDir` is set, the manifests and `stage.json` are written (see
   [3.9](03-animus-lib.md#39-the-stage-description-stagejson)).

### Building an episode (`Rebuild`)

`Setup` builds each env's first episode and marks the env `Fresh`, so the pool's first `Reset` is skipped. After that,
every `Reset` calls `Rebuild`:

1. **Draw the arena** by weight: forced by `ForceArena`, no draw for a single arena, otherwise `urand` over the
   weights. It is drawn first, so a seeded evaluation episode draws the same arena. The env's episode length is set
   from the arena.
2. **Clear totals.** Every seat's `ResetEpisode` and every encounter's `ResetEpisode`, including encounters this arena
   doesn't use, so their columns read 0.
3. **Deactivate** encounters the previous arena used and this one doesn't (remove the owner, disband the group, remove
   the enemy player). Then `BeforeRebuild` for this arena's encounters (the party group disbands before its members are
   replaced).
4. **`Begin()`** every seat's `BotSlot`, and remember each seat's current character for rollback.
5. **Pick the seats.** A party arena draws its size from `Party.SizeWeight1-4`; a raid arena takes
   all forty of its seats. Half the time
   (`Party.ClassicChance`) the roles are the classic makeup -- a tank and a healer at the head of every group of
   five, the rest damage -- shuffled over the seats in play. Otherwise each seat's
   role is drawn (`RoleTankChance`, `RoleHealerChance`). Each seat then takes a random layout of its role, or any layout
   if the run has none of that role. Other arenas give every seat any layout. A training episode draws it by the
   learner's per-layout weights (`WEIGHTS`, evenly without them); an evaluation episode doesn't draw at all: seed
   *i* plays candidate *(i + seat) mod count*, so every class/role is scored on an equal share of the seeds. Seats
   beyond the active count get no layout and no bot.
6. **Pick one level** every seat's class can be (death knights start at 55). It is `StageSettings::Level` if set;
   otherwise `Characters.HighLevelChance` percent of the time a level from `HighLevelFirst` to 80,
   `Characters.LowLevelChance` percent of the time a level from the class minimum to `LowLevelLast` (20; skipped when
   the class can't be that low), else any level from the class minimum to 80. An **evaluation** episode takes its
   band from its seed instead, as it takes its difficulty tier: seed *i* plays band *(i / the class/roles) mod 4* of
   1-20, 21-40, 41-60, 61-80 (the next band up when the seat's classes cannot be that low). Training then keeps the
   level mix the shipped companions play while the evaluation measures every band in equal numbers -- drawn, the
   middle bands were ~9% of the episodes each, too thin to read a class/role's hole from.
7. **Build each seat** (`BuildSeat`): random race and spec, `DamageScale(level)`, a bot named
   `Forge<envId>s<seat><a|b>` on the slot's idle session and account, placed in the env's instance (the first build of
   an env opens a new instance, unless the host placed the env). Party seats start spread around the spawn point, and a
   mirror seat 2 spawns 40-50 yd from seat 1 at a random bearing. Talent points are recomputed on the spawn map (death
   knights are created in Ebon Hold, where only quest-rewarded points count), then `SeatCharacter::Configure` dresses
   the bot (4.3).
8. **On failure**, every slot aborts, each seat's previous character is restored, and `Rebuild` returns false. The
   scenario sets `BuildFailed`, `IsTerminal` ends the episode at the next decision, and the following reset tries again.
9. **Commit.** Old targets despawn, every slot `Promote()`s (the old bots log out), and the first build of an env clears
   the spawn point's own creatures (`SpawnArea::Clear`). `env.Bots`, `MapId` and `InstanceId` are updated and
   `env.Targets` cleared.
10. **Encounters build** in build order (spawn the creature, the owner, the group, the first pull, the enemy player).
11. **Stock the seats**: potions, mana potions, bandages, healthstones (a warlock in the party hands them out), a
    soulstone, and a flask or elixir (4.3).

### Each decision

`ApplyActions`:

1. every active encounter's `UpdateEnemies` (linked packs join fights), then `Update` (the owner acts, the next pull
   spawns, the scripted opponent acts, ambushers arrive),
2. `AcceptResurrections`: dead seats and the owner accept a pending resurrection request as a client would, and
   the seat that cast it is credited once the ally is actually alive (`StepRevivedAlly`, `Revives`) -- see the
   note below,
3. for each seat with a living bot: `CurrentTarget` (the first encounter whose `SelectTarget` answers, else target 0),
   `BeforeSeatAction` on every encounter, build the `SeatView`, `SeatEncoder::Apply`, fold the result into the seat's
   totals, `OnSeatAction` on every encounter, and summon a called hunter beast.

> **Why accepting a resurrection needs bookkeeping.** A client answers an offer once, with one
> `CMSG_RESURRECT_RESPONSE`, so nothing in the core clears the request afterwards -- neither
> `Player::ResurectUsingRequestData` nor `ResurrectPlayer` touches `m_resurrectGUID`. Polling it every decision,
> as this must, therefore has to remember what it has already accepted. Before it did, one landed Rebirth stood
> its target back up free of charge every decision it died for the rest of the episode: 38.4 revives an episode
> against 0.97 owner deaths in stage 4, earning 57.55 of `druid_dps`'s 65.08 return. Every stage seeded from
> those learned that a druid scores by standing near a corpse.
>
> The fix is the module's and not the core's: remember which offer was accepted, wait for it (a delayed teleport
> reschedules the resurrect, so it does not always land on the decision it was taken), pay once the ally is
> alive, and clear the request then. A retry window gives up on one that never lands rather than barring the
> seat for the episode.

`Observe`: each seat's row through `SeatEncoder::Observe` (tracking when the seat entered combat for the combat-time
feature), then `WriteState`.

`Reward`: `BeforeRewards` on the arena's encounters in reward order (a pull's clear is decided once here, for every
seat), then for each seat compute `LastStepDamage` (damage / damage scale) and `LastStepDamageTaken` (damage taken / max
health), which several encounters read, and let each encounter add its terms to the seat's ledger. The ledger's step
total is the seat's reward. Finally `AfterRewards` (a cleared gauntlet pull is removed).

`IsTerminal`: a failed build, or any active encounter's `IsTerminal`.

### The seat view and encoder

`SeatView` is everything the blocks can't read from the world themselves: the layout, bot and target; the character
as built (level, race, spec, talent build); last-step damage, power change and damage taken; combat time; the supplies
it carries; whether self-resurrection is allowed; a hunter's stable; the enemy slots and selected slot; pull state
(pulls cleared, quiet time, pull time, elite pull, food and drink items); the owner; the three teammates and the
party's tank; the enemy player and whether it is a learned seat. Encounters fill the parts they own (`Encounter::View`).

`SeatEncoder::Observe` applies these rules in order:

- The row and mask start at zero, and **action 0 (no-op) is always allowed**.
- The character features (level, race, spec, talents) are always written, alive or dead.
- **A dead bot** sees only the duel block's dead features (whether it can resurrect itself), and its only possible
  action is the self-resurrect action.
- **No target** (between gauntlet pulls) blocks observation and actions, unless the layout acts without a target, which
  is any layout with the gauntlet block (food, drink, and self-cast spells between pulls).
- **Hidden enemies.** An enemy the bot can neither see nor detect (`CanSeeOrDetect`: stealth, invisibility) is left out
  of the view, as a client leaves it off the screen. An enemy slot holding one reads empty, the pvp block writes only
  what the bot remembers about a hidden opponent, and a hidden target is `HiddenTarget` instead of `Target`: every block
  sees no target, but the seat still observes and acts, and the duel block shows where the target was last seen and
  lets the seat search there. The critic's state (4.8) keeps everything.
- Otherwise every block writes its slice and mask.

`SeatEncoder::Apply` ignores actions of a missing or dead bot (except its own resurrection). It runs every block's
`BeforeApply` on every decision, including a no-op, then hands a positive action to the block that owns it, as an
index local to that block.

## 4.3 Characters

### Class/roles

| Class | Class/roles (specs) |
|---|---|
| Warrior | `warrior_dps` (Arms, Fury), `warrior_tank` (Protection) |
| Paladin | `paladin_heal` (Holy), `paladin_tank` (Protection), `paladin_dps` (Retribution) |
| Hunter | `hunter_dps` (Beast Mastery, Marksmanship, Survival) |
| Rogue | `rogue_dps` (Assassination, Combat, Subtlety) |
| Priest | `priest_heal` (Discipline, Holy), `priest_dps` (Shadow) |
| Death Knight | `deathknight_tank` (Blood), `deathknight_dps` (Frost, Unholy) |
| Shaman | `shaman_dps` (Elemental, Enhancement), `shaman_heal` (Restoration) |
| Mage | `mage_dps` (Arcane, Fire, Frost) |
| Warlock | `warlock_dps` (Affliction, Demonology, Destruction) |
| Druid | `druid_dps` (Balance, Feral cat), `druid_tank` (Feral bear), `druid_heal` (Restoration) |

Each `SpecProfile` (`Character/ClassRoleProfile.cpp`) sets a talent tab, a stat profile for gear (strength melee,
agility melee, ranged, caster, healer, tank), a range band (melee or ranged, which drives approach shaping) and weapon
layouts tried in order (two-hand, dual wield, daggers, one-hand, one-hand and shield, one-hand and held item, staff,
two-hand stat stick and ranged, optional wand).

Tank and healer roles play their own spec's damage game in role gear until the companion and party stages give them
their actual jobs.

### What `SeatCharacter::Configure` builds

The same function builds training seats and live companions, so a model gets in play exactly what it trained with.

- **Talents.** Every character draws one of three plans (`SeatCharacter::TalentPlan`, `Characters.*TalentChance`),
  reported as the episode info column `talent_plan` and scored as its own group in every evaluation:
  - **standard** (60% by default): the spec's standard 3.3.5 build from `Character/SpecBuilds.cpp` (31 specs),
    generated by `tools/spec_builds/generate.py` from `builds.py` and checked by `validate.py`. A build lists talents
    in the order players take them. Each point goes to the first talent in that order that still wants ranks and is
    legal (row and prerequisite rules follow `Player::LearnTalent`), so a low-level character has the talents players
    pick first. Every build spends exactly 71 points at level 80. Where a spec's tree has a root, snare or silence
    a player of that spec takes for solo play, the build takes it too (fury Piercing Howl, marksmanship Concussive
    Barrage, survival Entrapment, subtlety Waylay, shadow Silence, frost death knight Hungering Cold, affliction
    Curse of Exhaustion, feral cat Infected Wounds), so the policy has them to learn with.
  - **noisy** (30%): that build stopping 1 to `Characters.TalentNoisePoints` points short, with the rest spent at
    random -- a build somebody made up the tail of.
  - **random** (10%): every point spent at random, deeper rows likelier, the spec's tree first (51 points, what its
    last row needs) and then the others.

  The variety is what makes the talent features worth reading: a standard build is the same every time for a spec and
  a level, so a policy trained on those alone can ignore its talents and memorise the spec. A policy that sees all
  three has to play the character it was handed -- which is what a live server hands it. Glyph slots the level has
  opened get the spec's standard major and minor glyphs, whatever the plan.
- **Kit** (`ClassKit`). Every spell the class trainers teach up to the level (`trainer`/`trainer_spell`, learn-spells
  resolved), talent-gated ranks when the talent was taken, and class quest spells trainers don't teach (stances, Bear
  Form, warlock demons, Raise Dead). Weapon and armor skills the race and class may have, maxed for the level.
  Reagents: totems, Ankhs, soul shards, corpse dust, flash powder, and ammo in a quiver or pouch for hunters.
- **Gear** (`GearBuilder`). A random level-appropriate item for every slot, including both trinkets, drawn from every
  obtainable item (loot, vendors, quest rewards, crafted) the class can use and whose stats suit the spec. Random-stat
  items roll only suitable suffixes. The item level must fall in the band players of that level wear
  (`ITEM_LEVEL_ANCHORS`: a few levels above while levelling, Outland gear from 58, Northrend from 70, heroic dungeon
  180-213 at 80). If nothing fits, the search widens to 10, 25, then any number of item levels below the band, never
  above. After that it falls back to lighter armor, then stat-less items. Dungeon drops are weighted 4x and quest
  rewards 3x, and items near the middle of the band are preferred. Epics appear only at 70 and 80, and resilience gear
  only in PvP arenas. Paladins, shamans, druids and death knights get a relic.
- **Enchants and gems** (`GearEnhancements`). At 70 and 80 every item is enhanced, and half of them while levelling:
  the best suitable enchant a player of that level could buy (enchanting skill 5 per level, reaching 300 at 60 and 450
  at 80; weapon procs by name), a matching gem per socket (the socket bonus when all match, meta gems last, epic gems
  only at 80), death knight runeforges, rogue poisons (Instant in the main hand; Deadly in the off hand, or Crippling
  half the time once the rogue can have it), and shaman weapon imbues (enhancement: Windfury and
  Flametongue; elemental: Flametongue; restoration: Earthliving; the highest rank known). No profession-only enchants
  or gems.
- **Riding** (`TravelBlock::LearnRiding`, layouts with the travel block). The level's riding as players learn it
  (Apprentice at 20, Journeyman at 40, Expert at 60, Cold Weather Flying at 68, Artisan at 70) and the side's mounts
  of each speed (60% and 100% ground, 150% and 280% flying).
- **Supplies** (`Supplies`, applied by `StockSeats`). Five of the best healing potions, five mana potions (for mana
  users), five bandages with the level's First Aid skill, a healthstone and soulstone for warlocks, and at 70 and 80 a
  suitable flask (half the time an elixir while levelling). Gauntlet stages add the best vendor food and, for mana
  users, drinks: `Pulls.GauntletSupplies` (7) of each alone, five with an owner.
- **`PrepareFighter`**. No XP gain (levelling would change the character under the model), a warrior's stance (a first
  login normally casts it, and nothing works without one), and for hunters a stable offer of four tameable beasts of
  different random families.

### Raid stages

`stage13_raid_single` and `stage14_raid_gauntlet` train `MAX_SEATS` learned seats as `RAID_GROUPS` groups of
`GROUP_SEATS` (`SeatPlan::Raid`), each group with its own tank and healer. The first is one elite and its adds, won
or lost as the single pack is; the second is pull after pull with recovery between, which is what a wing of a raid
instance is before its boss.

A raid is not a bigger party, so it does not fight a bigger pack. Its rungs (`RAID_RUNGS`) put the difficulty in what
the enemy is -- elite, levels above, something on the ground -- rather than in how many there are, which `PACK_SLOTS`
caps at what a seat can observe anyway. The seats outnumber the enemies on purpose: what is being trained is
coordination against a fight that punishes standing in the wrong place.

Neither stage is in the default queue, and **neither is runnable at the usual env count**: forty seats an env is
forty bots an env, so `AnimusForge.Envs` has to come down roughly in proportion (a few dozen envs, not 128) before
starting one. Train by name: `forge start stage13_raid_single`.

### What an enemy is doing

A seat used to know one thing about an enemy's spellcasting: that it was happening. One bit, no identity, no clock.
It could not tell a filler from a heal, could not see an area effect on the ground at all, and had never read a
threat table -- so interrupting well, stepping out of fire and holding aggro were all unlearnable, and every dungeon
and raid mechanic has one of those three shapes.

All of it is now described by properties rather than by which spell it is (`IncomingSpell`), because a boss's
abilities live in file-scope `enum Spells` blocks inside its own script and no registry of them exists. A level 12
gnoll shaman's Lightning Bolt and a raid boss's produce the same features, and an unseen encounter needs no new code:

- **The cast** (per enemy slot, and for the duel's target): how much of it is left, whether it is aimed at this seat,
  area, cone, channeled, interruptible (by `EffectInterruptCast`'s own test, so the feature promises what pressing an
  interrupt would do), dispellable, shared damage (a soak), a heal, a summon, its radius, its missile flight time,
  its school and its mechanic.
- **The ground it is on** (`Encoding::StandingInHazards`): how many hostile ground effects the seat is standing in,
  how far it still has to walk to leave the worst, and the bearing of that one's centre, so moving away from it is
  the way out. Free to compute -- a ground effect applies an aura, and the aura knows the object that owns it.
- **The ground it is about to be on** (`Encoding::FindNearestHazard`): the nearest hostile ground effect it is *not*
  in yet, within 30 yd -- distance to its edge, bearing, radius. This is what makes avoidance learnable rather than
  only escape: without it nothing distinguishes clear ground from ground about to be walked into. A grid search over
  dynamic objects and armed traps, run once a second and re-measured arithmetically between searches, since a ground
  effect stays where it was cast.
- **What was done to it** (`Encoding::IncomingDebuffs`): harmful auras on the seat, how many are dispellable, the
  worst stack count, the longest remaining, and which crowd control mechanics are among them.
- **Threat** (`Encoding::ThreatShare`): the seat's own threat over the threat of whoever the enemy is on, so 1 means
  it holds aggro. Until this, every "threat" in the codebase was a proxy counted from who an enemy happened to be
  swinging at.

`hazard_seconds`, `hazard_damage` and `interruptible_casts_seen` in the episode info say whether any of it is being
used -- the last is the denominator the press-to-interrupt ratio always lacked.

For any of it to be learnable, the opponents have to produce it. Nothing did: the duel's pool is default-AI
creatures, which never cast, and no creature anywhere was selected for putting something on the ground. So
`Difficulty.CasterChance` (40%) draws a share of duel opponents from the same cast-only scripts the packs use, with
`Difficulty.HazardChance` (30% of those) from the ones that create a persistent area aura; and the upper pack rungs,
the last planned pulls and every raid rung include a hazard caster (`OpponentPool::RandomHazardCaster`). One enemy,
one cast and one pool of fire in a stage 1 duel is the cheapest place any of this can be learned.

### Durative actions

Most actions are one press of one button, and a 450 s episode is 1800 of them -- far more than credit reaches back
over. Four actions instead stand for a stretch of decisions (`SeatOption`, `Options.*`), so a plan can be expressed in
one choice:

| Action | Block | What it does until it stops |
|---|---|---|
| `rest_until_ready` | gauntlet | Eats and drinks, whichever is missing, until health and mana are back to 90% |
| `hold_interrupt` | pack | Interrupts the target the moment it starts casting, with the first interrupt the seat has -- its own spell, or its pet's (a felhunter's Spell Lock) when it has none. Offered only to a seat that has one |
| `keep_range` | duel | A ranged spec: runs back to casting range whenever the target reaches melee |
| `stay_on_target` | duel | A melee spec: runs back into melee reach whenever the target leaves it |

A seat runs **two at a time**: one positioning option and one standby (`SeatOptionSet`), since keeping a caster at
range and waiting for its cast are not alternatives. Each runs in its block's `BeforeApply`, every decision, and stops
on its own condition (the fight starts, the target dies, nothing is left to eat, the interrupt fires) or when its
`Options.*` clock runs out. What any other action does to it depends on what it is:

- **positioning** (`keep_range`, `stay_on_target`): only a movement order the other way takes over -- backing off ends
  staying on the target, closing in ends keeping range, and steps that do neither (to casting range, stop, follow)
  leave it alone. A fight is spells and swings between steps, and cancelling on those is what left a melee seat
  re-issuing its own movement every decision (the rogue pressed one every 0.39 s while it stood in melee reach 96% of
  the time).
- **standby** (`hold_interrupt`): nothing the seat does takes over from it, because waiting for the target's cast is
  not something it stops fighting to do. Cancelled by any press, a hold lasted 0.6 s against casts of 1.5-2.5 s and
  interrupted next to nothing.
- `rest_until_ready`: any other action ends it, as recovering is what the seat is doing rather than something it waits
  through.

Each option's own action is masked while it runs, so nothing cancels itself. What it does is counted as a press would
be (food and drink used, an interrupt pending on a caster). The core block reports how much of each option's clock is
left, so a running option is never hidden state, and `options_started` and `option_seconds` in the episode info say how
much a class/role uses them.

### The action catalog

`ActionCatalog` (per class, built once) is the fixed action space of the core block:

- no-op, cancel queued on-next-swing ability,
- **one action per rank chain of every combat spell** a level-80 character of any of the class's races knows (trainer,
  starting and racial spells, active talents of all three trees). The action casts the highest rank the bot knows. A
  spell counts as combat if it deals damage, applies a damage-relevant buff, debuff or DoT, generates resources,
  shapeshifts or summons, or if it is what separates a player from a rotation: charges and leaps (Charge, Intercept,
  Intervene, Blink, Disengage), threat redirects (Misdirection, Tricks of the Trade), Spellsteal, and survival auras
  (speed; mechanic and school immunity such as the PvP trinket, Every Man for Himself, Will of the Forsaken, Hand of
  Freedom and Fear Ward; dodge, parry, block, reflection; Feign Death, Fade and invisibility). Mounts, teleports,
  crafting, pure heals and charm are excluded. The candidates include the spells a learned spell teaches (the Feral
  Charge talent is Feral Charge - Bear and Feral Charge - Cat), and a spell that fires a missile is judged by the
  missile's spell (Freezing Arrow drops a Freezing Trap),
- one action per trinket slot, and one each for the use effect of the main-hand and off-hand item (weapons, shields
  and held books such as Rituals of the New Moon; `item_uses` counts them),
- then the rest of the kit, from the first stage on, as a player fights with it: **tactical spells** (interrupts, stuns
  with Sap included, silences, fears, roots, polymorphs, knockbacks, taunts, snares, disarms, traps, Distract and
  offensive dispels) cast at the target, and **sustain spells** (heals, HoTs, absorbs and friendly dispels) cast on the
  bot itself. Until they were core actions a duel healer had no heal and a duel mage no Polymorph or Ice Barrier.
  Each catalog entry in the manifest names its `group` (`combat`, `tactical`, `sustain`). Every layout's manifest,
  and its entry in `stage.json`, lists `action_names`: every action of the layout by name (`frostbolt_116`,
  `use_off_hand`, `move_to_range`, `pet_stay`), which evaluations use to count the actions each episode took.

The catalog also keeps the lists apart for the blocks that cast them elsewhere:

- `Tactical()`: the tactical spells above.
- `Sustain()`: the sustain spells above.
- `Revives()` (companion and party blocks): Resurrection, Redemption, Ancestral Spirit, Revive, Rebirth (their target
  is a corpse, `TARGET_FLAG_CORPSE_ALLY`), and a warlock's soulstone.

Each spell action also carries its **kind**, read from its first rank's effects: `Healing` (heals, HoTs, absorbs),
`Rankable` (a healing chain with more than one rank), `DirectHeal` (a heal on one unit and nothing else), `Defensive`
(a damage-taken reduction, immunity, split damage or avoidance buff under five minutes), `LongBuff` (a positive buff
of ten minutes or more) and `KeepsAura` (a HoT, absorb, long buff or defensive kept up on one unit, not stacking).
`Layout::BuffGroups` joins the long buffs a unit can only have one of (chains sharing a `spell_group`, such as the
blessings, or Fortitude and Prayer of Fortitude).

**Friendly targets.** A positive spell that takes a unit target (a heal, shield, HoT, blessing, Hand, Power Infusion,
Innervate) is cast on the **support block's selected friend** (`Encoding::SupportTarget`): the bot itself, the owner or
a teammate. Without the block (stages 1, 2, 6, 7 and the travel stages) it is the bot itself, as before. There is one
action per spell whoever it lands on; the companion and party blocks no longer copy the heals per ally (they listed
every heal twice, and the policy could not see what was already on the owner).

**Rank tiers.** A `Rankable` heal is cast at the seat's rank tier (`Encoding::KnownRank`): the highest known rank, or
the highest active rank about two thirds or a third of the way up the known ranks, for mana on a long fight. Every
mana spell keeps all its ranks active in the spellbook; only rage, energy and runic power abilities, paladin auras and
druid forms supersede their lower ranks (`Player::addSpell`).

Every action is masked each decision by the core's own `Spell::CheckCast`, run without casting (race, level, talent,
cooldown, GCD, power, stance, range, reagents). Beyond it, a cast that can only be wasted is never offered: a
`DirectHeal` on a friend at full health, a `KeepsAura` spell whose own aura on that friend still has more than a quarter
of its duration (or charges) left, and any positive unit-target spell while the selected friend is dead or gone. A
spell whose only effect is a control aura on its caster (Grovel, from the hidden GENERIC skill every character has)
is not in the catalog at all. As on a client, **no spell or item can start while a cast is in its cast
time**. The core only enforces that for client casts, and without the check a bot's new cast would silently replace the
current one.

Casting goes through `ApplySpellAction`, which builds the `SpellCastTargets` a client would send for the spell.

**Pacing** (`Actions.*`, `SeatMemory`, the same for forge seats and live companions). A decision comes every
`DecisionMs` (250 ms), and a policy free to act on every one re-issues orders no
player would: stage1_duel's warlocks sent their pet in 125 times an episode and started and stopped the same cast
over and over while never engaging. So the scenario masks, on top of every block's own checks, an action pressed too
recently: the same action again within `Actions.RepeatMs` (1000 ms; `Actions.MoveRepeatMs`, 300 ms, for movement
orders, so steering stays responsive), stopping a cast before it has run `Actions.StopCastMinMs` (500 ms), and
starting a spell the bot stopped itself within `Actions.RecastAfterStopMs` (2000 ms). Spells keep their GCD and
cooldowns as well. Two locks keep a plan from dissolving into dithering: a movement order back the way the last one
went (in toward the target after one away, or the reverse) waits `Actions.ReverseMoveMs` (1000 ms), and a stance,
form, presence, aspect, aura, seal, armor or pet stance holds `Actions.ModeLockMs` (5000 ms) before another change of
its kind (stage1_duel's seats gave 60-140 movement orders a fight; warrior tanks changed stance 22 times, hunters their
aspect 12). A paced action a policy sends anyway does nothing. `actions_per_minute` in the episode info shows how busy a
seat was.

**Repeats** (`Actions.Repeat`, every stage). Pacing caps how soon an action can be pressed again, not how often: a
policy can still press one button every second for a whole episode (stage1_duel's warlocks gave their pet 93 orders an
episode while it dealt 1% of their damage). Each press of an action counts that same action's presses within the last
`Actions.RepeatWindowMs` (10 s); past `Actions.RepeatFree` (3) of them, each press costs `Actions.Repeat` (0.02).
Movement orders are always free. How often the seat acts overall is not charged, only the same action over and
over. `repeated_presses` in the episode info
counts the charged presses.

## 4.4 Blocks

| Block | Observation (summary) | Actions |
|---|---|---|
| `core` | 67 globals (see below), then 6 features per catalog action (known, cooldown, aura on target, aura on self, stacks, time since the seat pressed it), then rank / max rank per class talent, then points per tree / 71 | The catalog |
| `duel` | Distance and bearing to the target, behind it, it faces the bot, its combat, target and casting state; the bot's movement, combat, stealth and auto-attack; damage taken last step; pet out, health, attacking; combat time; current cast progress and time left; a cancellable form; potions, healthstones and bandages carried and their cooldowns; Recently Bandaged; can resurrect itself; a hidden target, time since it was seen, and distance and bearing to where it was last seen; the target in line of sight; what the target is (creature type one-hot, max health against the bot's, damage multiplier, the share of the bot's hits its armor takes off, run speed, level difference, immunity to six magic schools and to fear, stun, root, snare, silence and polymorph); the bot stunned, feared or confused, rooted, silenced, snared; hunters' stable families and pet types | Move to target (to where a hidden target was last seen), move behind, move to casting range (25 yd), back off 10 yd, stop, start attack, pet attack, stop casting, cancel form, healing potion, mana potion, healthstone, bandage self, soulstone self (warlock), resurrect self, break line of sight (the nearest walkable place 8-26 yd away the target cannot see), 4 call-beast actions (hunter) |
| `pet` (hunters, warlocks, death knights, mages; empty for others) | The pet's presence, health, power, distance to the target, attacking it, casting, stance, following or staying; what it is (a ferocity, tenacity or cunning beast, an Imp, Voidwalker, Succubus, Felhunter or Felguard, a ghoul, a Water Elemental); whether it leaves on its own and how soon; its four most useful abilities (interrupts, then crowd control, dispels, threat, help, damage): present, on cooldown and what each does | Cast each ability (at the target, or on itself when helpful) as the pet bar does; passive, defensive, aggressive; follow; stay |
| `pack` | Living and in-combat enemy counts; 4 enemy slots (present, alive, health, distance, bearing, behind, attacking the bot or its pet, casting, in combat, crowd-controlled, current target, elite, level difference, in line of sight); (the tactical spells are core actions, cast at the selected enemy) | Select target slot 1-4 |
| `gauntlet` | Pulls cleared, pull active, time since the last fight, time into the pull, elite or higher-level pull, eating, drinking, food and drink left, time until an unengaged pull comes to the bot, time until the next pull spawns (the sustain spells are core actions) | Eat, drink (offered only where the item's cast check passes) |
| `companion` | The owner's presence, health, mana, distance, bearing, combat, movement, level difference and class; enemies on it; which slot it attacks; which enemies attack it; each revive's known and cooldown | Follow, assist (owner's target), guard (an enemy attacking the owner), one revive-on-owner per revive |
| `party` | Living party size, the most hurt ally's health, living tank and healer present; per teammate: presence, health, mana, distance, bearing, combat, role, class, attackers, target slot, which enemies attack it | Follow the tank; per teammate: assist, guard, revives |
| `party` teammate goals | Each teammate's goal one-hot (`SeatGoal`), so a party can divide the work | |
| `party` raid summary | The seat's group index, the living share of the raid and of its own group, the share of the living in combat, the most hurt living seat anywhere, and living tanks and healers over `RAID_GROUPS` | |
| `support` (stages 3-5, 8) | The selected friend and rank tier (one-hot); per friend slot (self, owner, then the party block's teammate slots): presence, alive, health, mana, distance, line of sight, attackers, role, the bot's own HoT (duration left) and absorb on it, buff coverage | Select a friend (the target of positive unit-target spells); set the rank tier (high, mid, low) |
| `pvp` | The opponent's class, role, level difference, mana, rage/energy/runic power, crowd-controlled, stealthed, pet out, casting a heal; the bot stunned/feared, rooted or silenced; whether the opponent is a learned agent; what a player tracks from what it saw used: the opponent's trinket cooldown, racial control break cooldown and number of spells of a minute or more cooling down; diminishing returns (controlled and opening stuns, fear, disorient, root, silence, horror, cyclone) on the opponent and on the bot, and the crowd control each has left; the opponent hidden (then only class, role, level, the cooldowns and diminishing returns are written) | none |
| `context` (12) | Owner present and alive, living teammates, living enemy players and creatures in the slots, nearest enemy player's distance, a player attacks the bot or the owner, PvP flag, battleground/arena map, dungeon/raid map, self-resurrection allowed, group size | none |
| `hostiles` (14 per slot) | Per enemy slot: player or creature, class, casting a heal, stealthed, pet out | none |
| `travel` (16) | Mounted, on a flying mount, can summon a ground or flying mount now, riding skill, indoors, height above the ground; the objective's presence, distance, bearing and height; at the objective; in combat; speed; moving | Mount the fastest ground mount, mount the fastest flying mount, dismount, move to the objective (by path, or straight in the air), climb 15 yd, descend 15 yd |
| `flag` (17) | Carrying the other side's flag; the seat's flag at base, carried or dropped; the other's at base or dropped; distance and bearing to both bases and to the nearest dropped flag; both scores | none |
| `order` (13) | What the side's director asked of this seat: the posture and rally one-hots, distance and bearing to the rally place, distance, bearing, health and whether the seat is already on the called target, and whether this seat holds the duty. All zero in an arena with no director | none: an order is advice, not a lever |

The core block's 67 global features are: level; race one-hot (10); spec one-hot (3); health; mana; rage; energy; runic
power; six runes; combo points; form one-hot (13); GCD; casting; queued next-swing; main-hand, off-hand and ranged
swing timers; main-hand speed; target health; target distance; in melee range in front; attack power; spell power;
melee and spell crit; melee and spell haste; melee and spell hit; expertise; armor penetration; last-step damage;
last-step power change; time into the episode (/ 5 min, `EPISODE_TIME_SCALE_MS`); and what the seat has been doing
(`SeatMemory`): time since its last movement order, which way that order went (in toward the target, away, neither),
time since its last stance, form, aspect, aura, seal, armor or pet stance change, and its own and its target's health
against their average over the last few seconds. All are normalised (see
`Blocks/CoreBlock.h` for the scale of each). The episode time is elapsed time, not the share of the limit left: a
companion has no limit, and without a clock a bot standing still out of combat sees the same row every decision, so a
deterministic policy can repeat a loop forever. A policy sees one observation at a time: without the memory features it
re-decided from scratch every decision, running in and backing off by turns and dancing between stances. A forge seat
and a live companion keep the same `SeatMemory`, so a model plays with what it trained with.

Movement and casting constrain each other: movement actions are masked while casting, and cast-time or channelled
spells are masked while running. The bot turns to face its target whenever it isn't running. Stop casting and cancel
form need no target, so they stay available between pulls. Layouts with the travel block act without a target too.

Anyone in the air without flight (a dismount, a cast that took the mount away) falls to the ground with a player's fall
damage (`MoveFall`, `Player::HandleFall`).

With the pack block, every spell, movement and pet action aims at the **selected enemy**. When it dies, the nearest
living enemy becomes the selection.

## 4.5 Encounters

An `Encounter` owns one part of what an env contains besides the seats. It keeps its own per-env state and has hooks
for each phase: `RewardTerms`, `AddEpisodeInfo`, `ResetEpisode`, `BeforeRebuild`, `Build`, `UpdateEnemies`, `Update`,
`SelectTarget`, `BeforeSeatAction`, `OnSeatAction`, `View`, `BeforeRewards`, `Reward`, `AfterRewards`, `WriteState`,
`IsTerminal`, `OnRecovered`, `OnPullStarting`, `Deactivate`, `Teardown`.

**`CreatureEncounter`** (`Opposition::Creature`). It spawns a random creature whose natural level range covers the
seat's level: normal rank, attackable, default AI with no script, no NPC services, not a civilian, guard or trigger,
walking on the ground in plain sight (no flying, hovering, swim-only or rooted movement, and no stealth or invisibility
aura on its addon), and spawned somewhere in the world. **Difficulty adapts per class/role** (`Difficulty.*`): tier t
below `EliteTier` (4) is a normal creature t x `LevelsPerTier` (1) levels above the seat, and from `EliteTier` on an
elite, (t - `EliteTier`) levels above, up to `MaxTier` (6). A class/role moves up a tier once it wins (kills without
dying) `RaiseAbove` (90%) of `Window` (200) fights at its tier, and down below `LowerBelow` (60%);
`ReviewChance` (25%) of its training fights come from a lower tier, so none is forgotten, and `StretchChance` (10%)
from the tier above, which does not count towards moving it: an evaluation scores every tier, so a class/role that has
stalled should not be meeting the tiers above its own for the first time there (the rogue sat at tier 4 and lost 44% of
the elite fights it was scored on). A fight that simple play wins
every time teaches nothing a plan would add. An evaluation spreads its seeds over every tier, every class/role over
every one (seed i plays class/role i mod the class/roles and tier (i / the class/roles) mod the tiers), so two
checkpoints meet the same fights, and the summary scores each tier on its own
(`difficulties`; stage targets can gate a tier, `target.difficulties`). Tiers restart at 0 with the worldserver.
`difficulty` and `opponent_elite` in the episode info say what each fight was. The bookkeeping is
`DifficultyLadder`, which the single pack's ladder shares. The creature is summoned at the seat's
level plus its tier's levels (`PendingSummonLevel`) 40-50 yd away at a random line-of-sight bearing on level ground
the seat can walk to (a path at most 1.5 times the straight line), facing a random way, hostile and aggressive, without
health regeneration. It starts out of aggro range. A creature with no path to its victim stops and regenerates, then
evades home at full health after 10 s, which no play can win: after 3 s without a path it is put beside its victim
instead (as instance trash is with `Creature.Instance.TeleportToUnreachableTarget`). `target_unreachable_seconds` and
`target_teleports` count it. Rewards are the one-on-one terms (`CombatReward::OneOnOne`). The episode is terminal on the
kill or on death with no resurrection left.

**`PullsEncounter`** (`Opposition::Pulls`). The pack pool adds creatures whose SmartAI only casts or talks (about 3500
casters and ability users) to the duel pool.

- **Single pack** (stage 2): creatures at the seat's level, clustered 40-50 yd away. `Pulls.LinkedChance` (70%)
  are linked, meaning once one member is in combat the rest attack. The episode is terminal on clear, death or the
  clock. **The pack climbs a ladder per class/role**, with the duel's `Difficulty.*` rates (up at 90% of 200 packs
  cleared without dying, down below 60%, 25% reviews), up to `Pulls.MaxTier` (5). Every rung has a spellcaster: a
  creature whose SmartAI casts a spell with a cast time, one an interrupt can stop (`OpponentPool::RandomCaster`).
  The other members are any pack creature, and the slots are shuffled.

  | Rung | Pack |
  |---|---|
  | 0 | 2: a caster and one more |
  | 1 | 3: a caster and two more |
  | 2 | 4: a caster and three more |
  | 3 | 4: two casters and two more |
  | 4 | 3: a caster, an elite and one more |
  | 5 | 4: two casters, an elite and one more, a level above the seat |

  Evaluations spread their seeds over the rungs as the duel's over its tiers, and `difficulty` in the episode info is
  the rung. A stage viewer's `spawn` tier picks the rung.
- **Gauntlet** (stages 3-5, 8): 1-4 creatures, or `EliteChance` (15%) a single elite, or `HigherLevelChance` (25%) a
  pack 1-3 levels higher (at most one level below level 20 and two below 30). In a party arena each member is elite
  with `PartyEliteChance` (50%) and up to 2 levels above. After a clear the field empties and the next pull spawns
  `NextPullMinMs`-`NextPullMaxMs` (8-20 s) later, out of aggro range. With an owner, pulls spawn around the owner.
  **Alone** the gauntlet is paced: a pull nobody has engaged comes to the seat `ArriveMinMs`-`ArriveMaxMs` (20-40 s)
  after it spawned (creatures that can't see the seat walk to it), and each pull cleared takes `ArriveShrinkMs` (1.5 s)
  off that, down to `ArriveFloorMs` (10 s), and `NextPullShrinkMs` (1 s) off the break, down to `NextPullFloorMs`
  (4 s). Resting has a clock, and staying away from the pulls is no way to last.
- **Elites.** The pool takes elites with health and damage multipliers up to 3 and 2.5 (normal creatures: 2), which
  keeps open-world and quest elites; at 2 the whole world had six.
- **Clear and interrupts.** A pull's clear is decided once per decision in `BeforeRewards`. The interrupt reward pays
  when a seat cast an interrupt, stun, silence, fear or polymorph at a casting enemy and that enemy's cast was then cut
  short by something other than itself or its death. A cast that finishes on its own doesn't count.
- **Deaths in owner arenas.** Deaths don't end the episode. After a pull, the dead wait up to
  `Resurrection.GraceMs` (20 s), and the next pull waits with them, for a resurrection they can get: their own
  Soulstone or Reincarnation, or a living seat's resurrection spell. Then whoever is still dead, companion or owner,
  stands up with `RecoverFraction` (half) health and mana (`NotifyRecovered`). A pull that kills everyone is cleared
  away (a wipe). Every death is paid once, and again after standing up.

**`OwnerEncounter`** (`Owner = true`). A scripted player (`ScriptedPlayer`) within `Owner.LevelSpread` (2) levels of
the seats, with a random role (tank 25%, healer 25%, DPS 50%) and a class that can fill it, dressed like a seat, given
the seats' faction. It is the env's ally 0.

- Between pulls it wanders near the spawn point and regenerates (`RegenFraction` per second).
- A tank owner starts every pull and taunts enemies off others. A healer owner heals the most hurt party member
  under `HealBelow` and stays within `HealerRange` of the tank. A DPS owner walks in after `OwnerEngageMin/MaxMs` (in a
  party, after `PartyOwnerEngageMin/MaxMs` so the tank can pull), and starts the pull itself `OwnerPullsChance` percent
  of the time. It casts a spell every `SpellMin/MaxMs`.
- Linked packs join in on whoever their engaged member fights.

**`PartyEncounter`** (`PartyGroup = true`). Every episode the owner, as leader, and the active seats form a real core
`Group` marked as a sim group (`CoreHooks::MarkSimGroup`), so party buffs, auras, group heals and every "party member"
check work as in play. It is disbanded before its members are replaced (`BeforeRebuild`). Each seat sees its three
teammates (the other seats, in order) and is rewarded for them.

**`OpponentEncounter`** (`ScriptedPlayer` or `MirrorSeat`). A scripted enemy player
(`EnemyPlayers::Create`) at the seat's level within `Opponent.LevelSpread` (1), with a random role (DPS 60%, tank 20%,
healer 20%), its spec, talents, kit and resilience gear, spawned 40-50 yd away. It engages within `EngageMaxMs` (3 s).
Melee specs fight in melee, ranged specs keep 10-30 yd (`RangedMin/Max`), and healers heal themselves below
`SelfHealBelow`. A rogue sneaks up in Stealth `ScriptedPlayers.StealthChance` (50) percent of the time and opens with
a stealth opener. `ScriptedPlayers.TacticsChance` (75) percent of engagements it plays its kit: it interrupts the
enemy's casts, crowd controls it every `ControlMin/MaxMs` (8-15 s) when it isn't already controlled, snares or roots a
melee enemy before backing off (ranged specs), breaks crowd control under `BreakBelow` (60%) health and uses a
defensive under `DefensiveBelow` (35%). Scripted enemy players see no more than a player: one that can neither see nor
detect its enemy stops attacking, goes to where it last saw it and searches around there. In a mirror arena the
"opponent" is the other seat. Both players get opposing factions and the PvP flag. Against a scripted player the
episode is terminal when the seat dies or kills it. In a mirror arena it is terminal when either seat dies, except in
a flag match. PvP arenas allow no self-resurrection.

**`AmbushEncounter`** (`Ambushers > 0`). One or two scripted enemy players with the opponent's class, role and gear
rules.

- Beside pulls (`ambush` arena), they arrive `Ambush.MinMs`-`MaxMs` (20-120 s) into the episode, engage within
  `EngageMaxMs`, attack the owner while it lives and then the nearest seat they can see (a hidden one only when they see
  none). They take enemy slots the pulls leave free (a pull has at most 4 minus the arena's ambushers creatures). Every
  seat earns `Ambush.Kill` (3) per ambusher killed, and the pulls and owner rewards pay the rest.
- Alone (`escort_duel`, `Opposition::Ambush`), exactly one ambusher is the whole fight from the start, paid as a
  one-on-one against it.

The pvp block observes the first living ambusher.

**`TravelEncounter`** (`Opposition::Travel`). An objective the seat has to reach: on the ground a place
`Travel.ObjectiveMin`-`Max` (60-320) yd away that it can walk to by a path at most 1.8 times the straight line, not in
water; in a `Flying` arena a place `FlyingMin`-`Max` (350-700) yd away. It arrives within 6 yd, on the ground. The
episode is terminal on arriving or death with no resurrection left.

**`FlagEncounter`** (`Opposition::Flag`, mirror seats). Warsong Gulch's rules between the two seats. The first seat's
base is where it starts; the other's is a place `Flag.BaseMin`-`Max` (100-180) yd away by path, where it is teleported.

- Touching (within `TouchDistance`, 4 yd) the other side's flag at its base or dropped takes it, and a carrier can't
  ride (it is dismounted, and every decision after). Touching one's own dropped flag returns it. Bringing the other's
  flag home while one's own is there captures it.
- A carrier who dies drops the flag where it fell. A dropped flag goes home on its own after `DroppedReturnMs` (10 s).
- The dead stand up at their base with full health after `RespawnMs` (15 s), like a graveyard wave.
- The seat's travel objective follows the flags: take the other's flag home, return one's own, chase the carrier of
  one's own flag, take the other's flag, pick it up where it lies.
- The episode is terminal at `CapturesToWin` (3).

Real battleground instances (Warsong Gulch's map, arenas with pillars) aren't used: their lifecycle (queues, a
premature end when a side is short, players teleported out at the end, one instance per match) doesn't fit an env
that keeps one instance for its lifetime.

## 4.6 Rewards

### The ledger

Each seat has a `RewardLedger`. An encounter adds `(term, value)` pairs, and the ledger sums the decision's total and
each term's episode total. Every term that any encounter of the stage pays becomes an episode info column
`reward_<term>`, so TensorBoard shows exactly what the stage pays for.

Terms: `damage_dealt`, `damage_taken`, `step_cost`, `casting`, `approach`, `stealth_opener`, `stealth_utility`,
`interrupt`, `kill`, `clear`, `health_kept`, `death`, `owner_damage_taken`, `owner_healing`, `tank_damage_refund`,
`threat`, `solo_fight`, `follow`, `owner_death`, `teammate_damage_taken`, `teammate_healing`, `teammate_threat`,
`teammate_death`, `revive`, `player_kill`, `progress`, `arrive`, `flag_capture`, `flag_pickup`, `flag_return`,
`carrier_kill`, `flag_lost`, `timeout`, `stall`, `spacing`, `readiness`, `control`, `self_healing`, `repeat`.

**Looking after itself and its friends (every stage).** `self_healing` pays `Support.SelfHealing` (0.5) times the
bot's effective healing on itself plus what its own absorbs soaked and its own damage-taken reductions prevented on
itself, as a fraction of its health. It is below every stage's damage taken weight, so a heal recovers part of what the
hit cost and being hit to heal it back never pays. On the owner and teammates, healers are paid `owner_healing` and
`teammate_healing` for healing and protection alike. Absorbs are tracked by polling the bot's own absorb auras on each
friend every decision (what they lost, or what was left when one vanished early); prevented damage is
`damage * (1 / multiplier - 1)` over the bot's own `MOD_DAMAGE_PERCENT_TAKEN` auras on the victim, at the hit
(`EnvPool::RecordPrevented`). In gauntlets, engaging a pull also pays `Support.BuffCoverage` (0.3) times the share of
the layout's buff groups up on the bot (averaged with the owner's where there is one).

**Goals** (`SeatGoal`, every stage whose policy has a goal head). The learner's goal head picks one of fight, control,
recover, protect, position or prepare every `mappo.goal_every_decisions` (16, so 4 s) and keeps it until the next
choice, and sends it to the sim with the actions (protocol 8: ACT carries the goals after the actions). The sim scores
whether each decision matched the goal -- damage for fight, an enemy other than the target held for control, healing or
resting itself for recover, healing or shielding the owner or a teammate for protect, its spec's range for position, a
buff, summon or stealth out of combat for prepare -- and pays `Goals.Match` (0.02) **once for each goal held**, on the
first decision that matches it. A goal is there to be reached, not to sit in: paid per decision, holding
`SeatGoal::Position` by standing at a spec's range earned +0.93 an episode in stage1_duel (2026-09-17), more than the
approach, casting and health terms together, and ranged seats learned to keep their distance for it. The charge is
small on purpose: it keeps the goals apart (nothing else stops a goal head collapsing into one goal), and the stage's
own terms still price the play. `goal_match_share` still counts every matching decision, paid or
not. A party's teammates see each other's goals in the party block. Columns:
`goal_<name>_share` per goal, `goal_match_share` and `goal_changes`; the learner's own metrics add `goal_<i>_share` and
`goal_kept_share` per update. The critic is goal-conditioned, so the advantage a decision gets is measured against what
that goal is worth.

Support columns (every stage): `healing_done` and `protection_done` (fractions of the bot's health), `overheal_share`,
`heals_on_full` (masked: 0), `defensive_casts`, `healing_casts`, `downranked_share`, `low_health_seconds` (any friend
below 35%); gauntlets add `buff_coverage` at engage.

### Scales

- **Decision scale.** Per-decision terms (step cost, threat, follow) are tuned for a 50 ms decision and multiplied by
  `DecisionMs / 50`, so they mean the same per second at any decision interval.
- **Damage scale.** `DamageScale(level) = 15 * e^(0.068 * level)` (about 16 at level 1, 230 at 40, 3500 at 80), so
  damage features and rewards have a similar size at every level. Pet, guardian and totem damage counts for the owner.
- **Health fractions.** Damage dealt is a fraction of the opponent's (or the pull's total) health, and damage taken a
  fraction of the seat's own maximum health.

### By stage (defaults)

**One-on-one** (duel, PvP, escort duel; `Duel.*`, `Casting.*`):

- per decision: damage dealt x2, damage taken x1, potential-based approach shaping toward the spec's range (melee
  3.5 yd, ranged 25 yd; 0.5 per 40 yd closed), step cost 0.0002
- stealth: +0.5 for a harmful spell cast from stealth that breaks it (Ambush, Garrote, Cheap Shot, Pounce, an attack
  out of Shadowmeld; it can't be repeated without earning stealth back), +0.05 for one that keeps it (Sap, Distract,
  Premeditation), once per target per stealth so it can't be farmed
- casting: -0.03 per second already spent on a cast-time spell that didn't finish, +0.03 per second of cast time for
  each one that finished in combat (channels pay through their ticks), and -0.05 for each cast the seat cut short
  itself (the stop-casting action, or moving out of its own cast), however little of it had run, so a start/stop loop
  costs more than an episode can earn. An enemy's interrupt costs only the seconds lost
- kill: +10, plus up to +1 for the share of the episode length left since the fight was engaged (the bot or its
  opponent entered combat), plus up to +0.5 for the share of health kept (damage taken is already charged as it happens, so a
  larger share would pay for surviving over winning). The approach, stealth and preparation before engaging
  cost only the discount
- death: -10 each time, including after a self-resurrection. With a self-resurrection available the seat has
  `Resurrection.GraceMs` to use it before the episode ends
- timeout (creature duel only): -10 when the episode's time runs out with neither side dead. The fight is lost, so the
  episode ends as a terminal outcome rather than a cut-off the critic bootstraps past; before it, never engaging was
  the cheapest way to lose
- stall (creature duel only): -0.08 per second the fight hasn't started once `Duel.StallGraceMs` (15 s) of the episode
  are gone. The timeout comes 900 decisions later, too far for the policy to tell standing still from closing in: at
  20M steps stage1_duel's deterministic policy stood where it spawned for the whole episode in 67 of 2048 evaluation
  fights. Preparing isn't stalling: the grace grows by the time the seat spent starting helpful spells out of combat
  (buffs, forms, stances, stealth, pet summons, conjuring; each its cast time, at least a 1.5 s global cooldown), up to
  `Duel.PreparationRefundMaxMs` (15 s), so a warlock summoning its demon or a druid shifting before the pull isn't
  charged for it and nothing has to start prepared. `preparation_seconds` in the episode info is that time, uncapped
- spacing (creature duel, ranged specs): -0.03 per second the opponent stands in melee range attacking the seat. The
  approach term only pays for closing in, so nothing kept a hunter, mage or warlock at its range
- repeats (every stage): -0.02 per press of the same action past the free ones in its window, and only when the
  press did nothing -- a spell that started casting, an item or a pet ability is never a repeat, because a caster's
  rotation is one nuke over and over. Orders to a pet already obeying, a target selected again and a stance pressed
  twice all still count (see Repeats, 4.3)
- winning outweighs winning fast: with the kill at 10, speed at most 1 and a loss at -10, a risky fast opener only pays
  more than a sure slow win above about 97% odds (at the earlier 3, 3 and -3 it was 79%)

**Pack** (`Pulls.*`): damage x2 of the pack's total health, damage taken x1, approach to the nearest enemy, +0.5 per
kill, +0.3 per interrupt, the stealth terms. Like the duel, a single pack is won or lost:

- interrupt +0.3 (`Interrupt`) times what the interrupt stopped: a heal 3x (`InterruptHeal`, it undoes damage already
  dealt), an area spell 2x (`InterruptArea`), a long cast 1.5x (`InterruptLong`), an ordinary cast 1x. The kind comes
  from the spell's own properties at the moment the cast dies. Never below 1: the flat term is how a class finds
  interrupting at all
- clear +10 (`PackClear`), up to +1 more for the share of the episode length left since a pack member entered combat
  (`FastClear`), up to +0.5 for the share of health kept (`PackHealthKept`)
- death -10 (`PackDeath`); timeout -10 (`Timeout`) when the 150 s run out with the pack and the seat both alive,
  ending the episode as a lost fight rather than a cut-off the critic bootstraps past
- overtime -0.1 per second (`Overtime`, in the timeout column) once a fight has gone on `OvertimeGraceMs` (60 s)
  since a pack member entered combat, and a death in overtime is charged the overtime left to the end of the
  episode. With the timeout alone, a -10 about 100 s away was worth about 2 to the discounted return against a whole
  -10 death now, and stage 2's warlocks learned to kite out the clock (28% timeouts at 10M steps). Dying never ends
  an overtime fight more cheaply than timing out
- stall -0.08 per second while no pack member has entered combat, once `StallGraceMs` (15 s) of the episode are gone,
  plus the preparation time as the duel's, up to `PreparationRefundMaxMs` (15 s). Counted per pull, not per episode:
  over a gauntlet, preparing once bought the full refund on every pull after it, and a seat that dropped combat to
  re-buff kept earning grace (stage 2's warlock went from 5.6 s of preparation a fight to 14.2 s, 19.4 s in the
  fights it lost)
- hazard -0.15 per second standing in a ground effect (`Hazards.Standing`) and -0.5 per fraction of maximum health
  taken from one (`Hazards.Damage`), together capped at `Hazards.Max` (3.0) an episode. The seconds are the term that
  teaches the behaviour: the damage arrives in ticks after the decision that caused it, and over two hours of stage 1
  it came to -0.003 an episode against a kill worth 10. The cap exists because melee have to stand in melee -- a
  hazard under the enemy is a real trade, and an uncapped charge teaches a seat to leave the fight
 Damage that could have been walked out of is worse
  than damage that could not, and this is the only term that pays a seat for moving its feet. It reads zero wherever
  nothing puts anything on the ground, which is most of the curriculum and none of a dungeon
- control +0.5 (`SinglePackControl`) times the damage prevented, in the seat's current health (floored at
  `ControlHealthFloor`, 20% of maximum), for every pack member other than the target that is held out of the fight
  -- each credited its own measured damage rate, or the pull's mean, or `ControlFallbackDps` for one that never got
  to act; up to `SinglePackControlMax` (1.0) a pull. Priced in the same currency as damage taken, at half its weight
  because the damage prevented is estimated rather than observed. The overtime grace also grows by the time an add
  was held, up to `ControlGraceMaxMs` (15 s), so holding one is not charged as dragging the fight out
- spacing -0.03 per second, for a ranged spec, while a living pack member attacks it in melee reach

With the gauntlet's clear and health kept (+2 and up to +2) and a -3 death, keeping health paid as much as clearing
the pack, and never engaging was the cheapest way to lose.

**Gauntlet**: the pack's per-step terms with damage taken x1.5. With an owner (stages 4, 5, 8), win-first as alone:
each cleared pull (`Pulls.Clear`) +2.5 and up to +0.5 for clearing within a minute of engaging it (not of its spawn, so
resting, sapping or stealthing in first is free), both x2 (`OwnerClearScale`), up to +0.5 for the seat's own health kept
during the pull; the seat's death -10 (`GauntletDeath`) and every owner death -15 (`Owner.Death`), so guarding the
owner comes before the seat's own health. At the earlier +2 +2 (x2) and +2 against deaths of -5 and -6, a pull cleared
was worth more than the owner's life. What alone teaches carries on beside the owner: readiness when a pull is engaged
(`OwnerReadiness`, 0.5), control (`OwnerControl`, 0.02 per enemy-second, up to `OwnerControlMax`, 1.5, a pull), seven
food and drink (`GauntletSupplies`), and a win: reaching the end with the owner never dead, no wipe and `OwnerWinPulls`
(5) pulls cleared counts as the kill, so `clean_kill` is the gauntlet won with the seat alive. **Alone** (stage 4) the gauntlet is won by lasting, and pays
win-first as the single pack does (`Pulls.SoloGauntlet*`): each cleared pull +5, up to +1 for clearing within a minute
of engaging it and up to +0.5 for health kept; a death -10, besides every pull it forfeits. Its pulls charge Stall
(-0.08 per second from `StallGraceMs` plus the preparation refund earned since the pull spawned, not while eating or
drinking) and Spacing as the single pack does. Engaging a pull pays readiness, `SoloGauntletReadiness` (0.5) times the
seat's health fraction the decision before (the lower of health and mana for mana users), so resting between pulls
pays when the next one starts rather than only through the death it avoids. Control pays `SoloGauntletControl` (0.02)
per second for each pack member of an engaged pull, other than the seat's target, that is stunned, incapacitated,
asleep, polymorphed, feared, or rooted out of melee reach and not casting, while another member is alive; up to
`SoloGauntletControlMax` (1.5) per pull. It stops when the control breaks, so controlling an add and then hitting it
pays nothing (stage 2's run used Sap, Blind, Polymorph, Hibernate and roots almost never). Reaching the end of the episode alive with
`SoloGauntletWinPulls` (5) pulls cleared counts as the kill, so `clean_kill` is a gauntlet endured; alive on fewer is a
timeout.

**Companion** (`Owner.*`, added to the gauntlet's, with kills and clears x2):

- everyone: owner damage taken (x1 for DPS, x2 for tanks and healers; a quarter of that when the owner is the tank);
  -0.01 per decision in combat while the owner isn't; +0.0005 per decision within 12 yd out of combat, -0.002 beyond
  25 yd; -15 per owner death; +1.5 when an ally the seat resurrected stands up
- tanks: +0.002 per enemy on the tank and -0.02 per enemy on the owner, per decision; half of the gauntlet's damage
  taken refunded
- everyone: effective healing on the owner x2, and what the seat's absorbs soaked and its damage reductions
  prevented there (overhealing earns nothing, because the heal hook reports health gained). Paying only healers left
  every other class at 0.000-0.005 of its healing going to the owner
- DPS and healers **beside a tank owner**: -0.004 per enemy attacking them, per decision. Beside an owner that does
  not tank, holding the enemies is the seat's job and is not charged: charged whatever the owner was, at 450 s an
  episode it came to -22.9 against +0.6 for healing the owner, the largest term in the stage

**Party** (`Party.*`, added per teammate): teammate damage taken (not for a tank teammate; x0.5 for DPS, x1 for tanks
and healers), healers' effective healing on teammates x2, tanks -0.02 per enemy on a non-tank teammate per decision,
-3 per teammate death. Kills and clears are shared. A tank isn't charged for fighting before the owner joins.

**Ambush**: +3 per ambusher killed, for every seat.

**Travel** (`Travel.*`): potential-based shaping on the distance left to the objective (+1 per 100 yd closed, taken
back for leaving), arriving +3 plus up to +3 for the share of the episode left, damage taken x1 (falls, what it rode
past), death -3, step cost 0.0002. Nothing pays for mounting: a mount is worth its cast time only on a long enough
trip, and the policy learns which.

**Flag match** (`Flag.*`, instead of the one-on-one terms): capture +5, the other side capturing the seat's flag -3,
taking the other's flag +1, returning its own +1, killing the carrier of its own flag +1.5, death -1, step cost 0.0002,
and potential-based shaping toward the seat's current objective (+0.5 per 100 yd), started over whenever the
objective changes, so a flag changing hands pays nothing by itself.

## 4.7 Scripted baselines

`Baselines::Choose` reads a seat's row through its layout, so the baselines follow layout changes automatically.

- **`greedy`**: the first allowed spell or trinket in catalog order. Every layout supports it.
- **`fight`** (layouts with the duel block), first match wins:
  1. with the travel block and an objective: dismount at it; far from it and not mounted, a flying mount where one
     flies, else a ground mount; on a flying mount climb to 20 yd, land at the objective; otherwise head for it (and
     wait while moving, rather than cast something that would dismount),
  2. with the gauntlet block and no target: eat when health is low, drink when mana is low,
  3. support: below 30% health, the first allowed defensive; the most hurt living friend below 60% (the bot itself
     without the support block) selected, then its first allowed heal (a healer cancels a form first if needed); a
     healer selects the owner or a tank teammate under attack without its HoT or shield and casts a kept-up heal,
  4. (the masks keep it from healing a friend at full health or re-casting what is still up),
  5. with the pet block and a pet class: out of combat with no living pet, call a stable beast or cast the best summon
     (Felguard, Voidwalker, Felhunter, Succubus, Imp; Raise Dead; Water Elemental); with a pet out, send it at the
     target,
  6. a spec of the ranged band (hunters, casters, healers) holds range: more than 28 yd from a living target, move
     to casting range (24 yd); with the target out of line of sight, move toward it; a hunter the target is hitting
     in melee reach while its pet attacks the target backs off 10 yd (its shots can't be used there) and lets the
     pet hold it; auto-attack only once the target is in melee reach (a hunter with no pet yet, or one still held),
  7. a melee spec starts auto-attack,
  8. and moves to a living target beyond melee reach while not already moving,
  9. while its pet attacks the target, the pet's first allowed damaging ability (pets don't autocast, and an Imp or a
     Water Elemental can't melee, so this is all they do),
  10. otherwise its rotation (not `greedy`), first match wins:
     - in no form, the spec's own: Moonkin Form (balance), Cat Form (feral cat), Dire Bear or Bear Form (feral bear),
       Shadowform (shadow). Other forms and stances are never cast; `SeatCharacter::PrepareFighter` puts a warrior in
       its stance,
     - the first allowed damaging spell in catalog order: school or weapon damage, a leech, or a melee or ranged
       weapon attack. A spell that only ticks is cast while its aura isn't on the target, and crowd control that
       damage breaks (confuse, fear, transform) never,
     - out of combat, a buff that isn't on the bot: an aura with no cooldown of its own, not speed, stealth,
       invisibility or feigning death, and at most one of each exclusive kind (a seal, a paladin aura, an armor, an
       aspect),
     - otherwise nothing.

     A cast resets the caster's swing timer (`Spell::cast`), and the first spell in catalog order is often a buff that
     can be cast again forever. `greedy` presses one every decision the pacing allows, so a paladin with a slow
     two-hander never lands a swing, and a caster holding range casts Lightning Shield or Inner Fire instead of ever
     starting the fight.

They are the reference numbers a trained policy has to beat (evaluation baseline) and a mechanics smoke test
(`forge run <stage> fight`).

## 4.8 The critic state

The centralised critic sees a class-agnostic global state of the env. `StateDim = 21 + 4 x 23 + 4 x 25 = 213`.

| Part | Features |
|---|---|
| Global (21) | Episode time fraction; pull active; pulls cleared / 10; time to next pull / 20 s; elite pull; linked pull; owner present, alive, health, mana, x, y (relative to the spawn point, / 40), in combat; arena one-hot (8) |
| Per seat (4 x 23) | Present, alive, health, mana, other power, level / 80, role one-hot (3), class one-hot (10), in combat, casting, x, y |
| Per enemy slot (4 x 25) | Present, alive, health, x, y, casting, elite, level difference / 5, in combat, victim is the owner, victim is seat s (4), max health against seat 0's, damage multiplier, armor reduction against seat 0, run speed, creature type one-hot (7) |

The episode time *fraction* (the share of the episode's own limit spent) appears only in the critic state, because live
play has no time limit. Observations carry elapsed episode time instead (the core block's last global feature). In
self-play, each seat's opponent is the other seat and already appears in the seat part.

## 4.9 Tuning

Every value that shapes the curriculum is a config key `<TuningPrefix><Group>.<Name>`: `AnimusForge.Curriculum.*` in
the forge and `Animus.Curriculum.*` in mod-animus. `CurriculumTuning::Visit` lists them once, for loading and for
writing. Min/max pairs are put in order on load.

| Group | Controls |
|---|---|
| `Characters.*` | High-level threshold and chance, how talent points are spent, how often a pet class starts with its pet out |
| `Party.*` | Size weights, classic makeup chance, role chances, teammate reward weights |
| `Duel.*` | One-on-one reward weights and preferred ranges |
| `Casting.*` | Cast time wasted and completed, the charge per self-inflicted cancel |
| `Actions.*` | Pacing: how soon the same action, the same movement order, a stop of a new cast and a recast of a stopped spell are allowed again |
| `Pulls.*` | Linked, elite and higher-level chances, pull timing, owner engage timing, recovery fraction, pull reward weights |
| `Owner.*` | Level spread, role chances, owner reward weights, follow distances |
| `Resurrection.*` | Grace period, revive reward |
| `Opponent.*` | Level spread, engage time, role chances |
| `Ambush.*` | Arrival window, engage time, kill reward |
| `ScriptedPlayers.*` | Spell and heal intervals, wandering, regeneration, heal thresholds, ranges; PvP stealth and tactics chances, crowd control interval, defensive and break thresholds |
| `Travel.*` | Objective distances on the ground and in the air, travel reward weights |
| `Flag.*` | Base distance, captures to win, respawn and dropped-flag timers, touch distance, flag match reward weights |
| `Arena.<stage>.<arena>.Weight` | Arena weights (read by `StageScenario`, not `Visit`) |

The effective values are written into `stage.json` under `tuning` and copied into each run directory. To watch a stage
in mod-animus exactly as a model trained on it, copy that run's `tuning` into `Animus.Curriculum.*`.
`animus-forge/conf/mod_animus_forge.conf.dist` documents every key.

## 4.10 Episode info

Every stage reports these **core columns** per seat:

- `damage`, `dps`, `white_damage`, `special_damage`
- `level`, `race`, `spec`, `class`, `role`, `talent_plan` (0 standard, 1 noisy, 2 random), `unspent_talent_points`,
  `equipped_items`
- `spell_casts`, `trinket_uses`
- `present` (0 for an empty party seat; ignore that row), `arena` (index into `stage.json` arenas), `opponent_seat`
- `killed`, `died`, `time_to_kill`, `damage_taken`, `health_left`, `stealth_openers`, `stealth_utility_casts`,
  `pet_summoned`, `pet_at_start`, `pet_damage_share` (of the seat's damage, what its pets and guardians dealt),
  `pet_died`, `pet_abilities` (pet bar abilities started), `pet_orders` (stances, follow, stay, sending the pet in),
  `item_uses` (use effects of the main-hand or off-hand item),
  `opponent` (creature entry)
- what the seat did with its pet: `pet_attack_orders`, `pet_passive_orders`, `pet_defensive_orders`,
  `pet_aggressive_orders`, `pet_follow_orders`, `pet_stay_orders` (each order given), `pet_out_seconds`, and the share
  of that time the pet was attacking something (`pet_attacking_share`), set passive (`pet_passive_share`) or told to
  stay (`pet_staying_share`). A pet's abilities are the policy's to cast: its spells are learned with autocast off
- `casts_completed`, `casts_cancelled`, `cast_seconds_wasted`, `cancelled_stopped`, `cancelled_moved`,
  `cancelled_target`, `cancelled_other`
- `consumables_used`, `self_resurrections`
- how a fight ended, to tell the ways of losing apart: `timed_out` (creature duel: time ran out with neither side
  dead), `engaged`, `engage_time`, `target_health_left`, `distance_at_end`, `form_at_end` (the `ShapeshiftForm`),
  `power_left` (of the primary power), `target_evade_seconds` and `out_of_sight_seconds` (creature duel: the opponent
  evading, and engaged without line of sight to it), `target_unreachable_seconds` and `target_teleports` (creature duel:
  the opponent without a path to its victim, and put beside it for that), `actions_per_minute` (actions other than the
  no-op), `repeated_presses` (presses charged by `Actions.Repeat`)
- how the seat fights, to grade a spec's playstyle (they reward nothing):
  - `melee_damage_share`, `shot_damage_share`, `spell_damage_share`: the seat's own damage by the game's damage class
    (`SpellInfo::DmgClass`), as shares of all its damage, so with `pet_damage_share` they add up to 1. Melee is melee
    swings and melee abilities (Raptor Strike, Sinister Strike), shots are ranged weapon attacks (Auto Shot, Steady
    Shot, a wand) and spells are the rest, DoTs included. The damage hook doesn't say which spell dealt a hit, so the
    library notes the spell in `ModifySpellDamageTaken` and `ModifyPeriodicDamageAurasTick`, which run just before it
    for the same attacker and victim. Spell damage it can't match counts as a spell
  - `in_melee_share`, `target_on_pet_share` (one-on-one arenas): the share of the fight, engaged with the seat alive,
    it spent within melee reach of the opponent, and the share the opponent spent attacking its pet or guardian. A
    hunter's shots can't be used inside melee reach (`SPELL_FAILED_TOO_CLOSE`), so for a hunter `in_melee_share` is
    the share of the fight it played melee. For a caster it is mostly where the opponent chose to fight
  - `target_rooted_share`, `target_snared_share`, `roots_applied`, `snares_applied` (one-on-one arenas): the share of
    the same time the opponent spent rooted (Frost Nova, Entangling Roots) or slowed (Concussive Shot, Wing Clip,
    Frost Shock, Earthbind) by the seat, its pet or its totems, and how often one went on where there was none
  - `feign_deaths`, `feign_death_resets` (one-on-one arenas): how often the seat feigned death, and how often its
    opponent then evaded home at full health (within 3 s of the feign ending) because nothing else held it. With a pet
    on the opponent, feign death hands the fight to the pet; without one it throws the fight away

Encounters then add their own columns:

- pulls: `kills`, `interrupts`, `pack_size`, `linked`, `pulls_cleared`, `food_used`, `drink_used`, `sustain_casts`,
  `deaths`, `wipes`; gauntlets also `engage_health`, `engage_mana`, `pulls_started_low`, `pulls_arrived`,
  `rest_seconds`, `eat_failed`, `drink_failed`, `meals_cut_short`, `control_seconds`
- owner: `owner_class`, `owner_role`, `owner_died`, `owner_deaths`, `owner_damage_taken`, `owner_healing`,
  `threat_on_bot`, `threat_on_owner`, `revives`
- party: `seat`, `teammates_died`, `teammate_damage_taken`, `teammate_healing`, `threat_on_teammates`
- opponent: `won`, `opponent_class`, `opponent_role`
- ambush: `ambushers`, `ambushers_killed`
- travel: `arrived`, `travel_seconds`, `start_distance`, `mounted_fraction`, `flying_fraction`
- flag: `flag_captures`, `flag_pickups`, `flag_returns`, `carrier_kills`, `flag_deaths`, `match_won`

Then come the `reward_<term>` columns. Columns of encounters an episode's arena doesn't use read 0. The exact list for a
stage is `episode_info` in its `stage.json`.

## 4.11 Stage by stage

In the order `forge start` trains them. Each seeds from the stage above it in the tree (4. head).

### Stage 1: `stage1_duel`

A new character against a real creature (4.5), with the class's whole kit, in 90-second episodes (a timeout is a
lost fight, and a healer against a creature with twice the usual health needs the time). Nothing seeds it, so its
networks start from scratch. Hunters are
offered four beasts each episode through `call_beast` actions, because Call Pet needs a pet saved in the database. The
observation shows each beast's family and pet type, so the policy can learn its preference. Warlock demons, Raise
Dead, Water Elemental and Feral Spirit are ordinary spell actions with their reagents in the bags. A warlock carries 5
Soul Shards: they don't stack, and the 20 it once had filled the 16-slot backpack, so no potion, bandage, healthstone
or soulstone fit and stage 1's warlocks never used one. The bot gains no XP.

Pets are played as a player has them:

- **A summon the core does not call a pet is still seen.** `Unit::GetGuardianPet` only returns what is registered as
  the owner's pet, so a ghoul raised without Master of Ghouls, Army of the Dead, an Infernal or Feral Spirits used to
  leave the whole pet block reading zeros while the thing fought: stage1_duel's death knight tanks raised a ghoul in
  90% of their fights, took a tenth of their damage from it and never saw one. `PetBlock::FindPet` falls back to the
  first creature the seat controls, and `PetBlock::OBS_COMMANDABLE` says whether it takes orders -- a guardian has no
  action bar of the owner's, so every pet action of it stays masked, exactly as a player's would be.
- **Six ability slots.** A hunter beast with its talents spent carries more than four castable abilities (a focus
  dump, its family's special, a taunt, a sprint, and what the talents added), and the slots keep the best kinds
  first, so at four a ferocity pet's Rabid or Call of the Wild was never offered.
- **A hunter's beast arrives as a player's does:** at the hunter's level, fed to full happiness (an unhappy beast
  deals 75% damage), with its level-up spells learned and its talent points spent along a standard build for its
  tree (`PetTalents::Spend`).
- **A new pet starts defensive.** Creating a pet's `CharmInfo` sets it passive, and a player's summon then loads the
  stance saved with the pet, which a bot never has, so every pet stayed passive and only fought what it was sent at.
  `PetBlock::DefaultStance` sets a newly seen pet that came out passive to defensive, once per pet, so a stance the
  policy picks afterwards stands. Companions do the same.
- **A warlock's demon isn't stunned by the masks.** A strict `Spell::CheckCast` of a demon summon casts Summoning
  Disorientation (32752) on the warlock's current pet, meant for a summon the player starts. The action masks check
  every summon spell every decision, so stage1_duel's demons were stunned for nearly the whole of every fight,
  ignored every attack order and dealt no damage. `SpellChecks::CheckCast` checks those summons loosely, and does
  the global cooldown and shapeshift checks the strict pass would have made itself.
- **A called beast is fed and talented.** It arrives happy (a freshly tamed beast is unhappy and deals 75% damage) and
  its talent points are spent (`PetTalents`): the build players took for its tree -- ferocity, tenacity or cunning --
  point by point through `Player::LearnPetTalent`, the rest at random. Family-specific talents (Dash, Dive, Charge,
  Swoop, Mobility) are left out, since the core cannot tell which families may take them.
- **A dead pet can be brought back.** Revive Pet is a hunter action, and `call_beast` is allowed over a dead pet (the
  corpse is dismissed first).
- **Half the pet classes arrive with their pet out** (`Characters.PetOutChance`): a hunter one of its offered beasts,
  a warlock a random demon it knows, a death knight with Master of Ghouls its ghoul, a frost mage with Glyph of Eternal
  Water its elemental, summoned without a cast and with the summon ready again. The rest summon it themselves, so the
  policy learns both to get a pet out and to use (or replace) the one it has.
- **Follow and stay are hidden while fighting** (the seat or its pet in combat): they call the pet off its target, and
  stage1_duel's warlocks cycled attack, follow and stay eight times a fight while their pets never landed a hit. They
  are there out of combat, to position a pet before a pull.
- **The `fight` baseline uses pets:** out of combat it calls a stable beast or casts its best summon (Felguard,
  Voidwalker, Felhunter, Succubus, Imp; Raise Dead; Water Elemental) when no living pet is out, and sends the pet at
  the target, so the per-class/role gates of pet classes compare with a character that plays its pet.

Learner (`configs/stage1_duel.yaml`, the root every other config extends):

- **Networks:** hidden `[256, 512, 512]`: a 256-wide adapter per class/role and a two-layer 512-wide shared trunk,
  which puts the capacity where every class/role trains it. Every stage keeps these sizes, or the trunk can't be
  copied.
- **PPO:** gamma 0.997 and lambda 0.985 per 100 ms of game time (`reference_decision_ms`, compounded to
  `AnimusForge.DecisionMs` so horizons stay the same in seconds: a ~33 s horizon and a ~5.5 s GAE credit trace, printed
  at start), clip 0.2, entropy 0.01 with an entropy floor at 30% of `ln(legal actions)` (boosted up to 4x), learning
  rates 3e-4, 4 epochs stopped early past approx KL 0.04, 8 minibatches, rollout 128 with overlapping updates, value
  normaliser beta 0.99, advantages normalised per class/role. Budget 300M env steps.
- **Evaluation:** every 10M steps and at the start, 1024 seeded episodes against `fight`. Training episodes lean
  toward the class/roles furthest from their gates (`layout_sampling`, by score gap and `clean_kill`, at most 4x).
- **Convergence:** patience 3, window 4, z 2, at least 2% and 0.01 improvement, not before 30M steps.
- **Target:** at least baseline for every class/role (16+ episodes, 1 standard error of slack) on the same spread of
  difficulty tiers, 95% of base-tier fights won outright (`clean_kill`, by its Wilson bound; `target.difficulties`),
  and no livelocks, overall and for every class/role; confirmed on 4096 held-out episodes. Up to 2 restarts with 3x
  entropy decaying over 10M steps and fresh optimizers.

### Stage 2: `stage2_pack`

Adds the pack block: target slots and the enemy-slot observation (the tactical spells come with the core from stage 1). Linked packs mean pulling one
enemy pulls all of them. The interrupt reward teaches casting interrupts at the right moment. 150 s episodes: a pack
is up to four of the duel's creatures, which took stage 1's policy about 17 s each. Rewards are the duel's win-first
ones (4.6).

Config: rollout 256, gamma 0.999 and lambda 0.99 (~100 s horizon). The target is clean wins of at least 85% overall
and 65% per class/role (Wilson bounds) **on rungs 0-2** (`target.base_difficulty: 2`), the 2-4 creature packs of the
first run, which had no ladder and reached 90% overall at 20M steps; the caster and elite rungs above count through the
score. `until_passed` is off, so a stage that converges short of it halts after its restarts instead of training on.

### Stage 3: `stage3_hazards`

**Drill.** Stage 2's pack, except that every pull contains a creature that puts something on the ground
(`OpponentPool::RandomHazardCaster`), whatever rung the difficulty ladder is on. The pack ladder only reaches
hazard casters at rung 3, so a class/role that stalls below it never meets one and never learns to step out of
a hazard; this makes that lesson learnable on its own. It adds the support block for the hazard charge.

It is on the trunk: `stage4_gauntlet` seeds from it, so the lesson carries into every PvE stage after it. The
hazard charge lands about four times harder on a tank than on a ranged seat, because a tank cannot walk out of
what it is holding an enemy in -- read the per-role columns before the overall one.

### Stage 4: `stage4_gauntlet`

Adds the gauntlet block: sustained combat, recovery between pulls with food, drink and the core's sustain spells.
Between pulls there is no target, so target features are 0 and only self-cast actions are allowed. The gauntlet arena
runs 450 s, paced so that eight or more pulls fit (pulls come to the seat, sooner as it clears them), and a solo
gauntlet is won by lasting to the end with five pulls cleared (rewards above). Its episode info adds the recovery
columns: `engage_health` and `engage_mana` (means over the pulls engaged, taken the decision before), `pulls_started_low`
(below half health or 30% mana), `pulls_arrived` (came to the seat unengaged), `rest_seconds`, `eat_failed`,
`drink_failed`, `meals_cut_short` (food or drink that ended early with health or mana still to restore) and
`control_seconds` (enemy-seconds kept out of the fight, as the control reward counts them).
Config (extends stage 2's): gamma 0.999 and lambda 0.99 (~100 s horizon, ~9 s credit trace, so resting before a pull or
stealthing in is tied to the clear it pays for), rollout 256, budget 400M, at least 40M steps, evaluations every 15M.
Target, provisional until a run calibrates it: 55% of gauntlets won overall and 40% per class/role (Wilson bound),
four pulls cleared on average, no livelocks; `until_passed: false`. The first run (300 s, pulls that waited, a win by
merely lasting) reached 63-66% survived with 12% of its wins on at most one pull cleared, rogues and healers avoiding
the pulls.

### Stage 5: `stage5_endurance`

A planned run: eight pulls in a fixed order, the same every episode, seeded from stage 4 and using its blocks. The
order is an opener of two, three, four with two casters, a small one, four, three with an elite, four with an elite a
level up, and last an elite pack two levels up. Nothing about the fights is new -- stage 4 taught them -- so what is
left is the plan: what to spend on the opener, what to keep for the last pull, and whether the small fourth pull is
used as a rest. It is won by clearing the last pull alive; the 900 s clock running out is a loss however far it got,
and `pulls_cleared` (out of 8) is how far. `PullSchedule::Sequence` builds it; `eval.trace_episodes: 4` records four
whole runs decision by decision, which is how a plan is read.

It is on the trunk: `stage9_companion` seeds from it, so the plan it learns is carried into the companion and
party stages rather than being a dead end beside them.

### Stage 6: `stage6_run`

The first travel stage, and the one that comes before mounts exist. A place 40-160 yd away in Kalimdor, 120 s,
and **mounting is masked** (`ArenaDefinition::OnFoot`) -- masked rather than merely unpaid, because a masked
action cannot be explored into and the lesson stays clean. What is left is what a player does before it can
ride: the speed cooldowns the class has (Sprint, Dash, Travel Form, Aspect of the Cheetah), not stopping, and
not wandering off the path.

`mounted_fraction` is the check that the mask holds: it must read 0.0000. Blocks: core, duel, pet, travel --
the pack block is dropped, so the travel line trains straight off the duel.

### Stage 7: `stage7_travel`

Getting somewhere, off the duel. A character of level 20 or more, with its level's riding and its side's mounts, starts
in Old Hillsbrad (which allows mounts) with a place 60-320 yd away by path. A mount's cast time only pays on a long
trip, and arriving on foot is what lets it fight at the end. Blocks: core, duel, pet, travel. 150 s episodes. Config:
budget 150M, at least 20M steps, a travel report.

### Stage 8: `stage8_flight`

Flying, off travel. Characters of level 60 or more (Expert Riding, and Artisan with a fast flying mount from 70) start
in Outland's Nagrand, where flying mounts fly, at one of eight spawn points, each env in its own phase. The place is
350-700 yd away: flying is several times faster and passes over everything, but dismounting in the air falls with a
player's fall damage, so the policy learns to take off, keep a height, land and dismount. Battlegrounds never allow
flying mounts (the zone must be Outland or Northrend, `SpellInfo::CheckLocation`), so this is for the open world.
180 s episodes. Config: gamma 0.999 and lambda 0.99, budget 150M.

### Stage 9: `stage9_companion`

Adds the companion block and the scripted owner. The seat learns to follow, assist, guard, heal and resurrect it, and
role-specific behaviour appears (tank threat, healer throughput, DPS threat discipline). Deaths recover after pulls and
the episode always runs its full length (450 s; without its own the arena took the host's 60 s), so letting the owner
die is never a way to escape penalties.

The target (provisional, for the first run to calibrate): `clean_kill` -- the win above, with the seat never dead -- of
50% overall and 35% per class/role by the Wilson bound, the owner dead in at most 35% of episodes, wipes at most 0.2 an
episode, at least 5 pulls cleared on average, no livelocks. The role checks are reported per class/role and per role
(the summary's `roles`): `owner_heal_share`, the share of the owner's damage taken the seat healed, for healers, and
`threat_share`, the share of the enemies' attention on the seat rather than the owner, high for tanks and low for the
rest. `target.role_metrics` gates them once a run shows what each role reaches.

### Stage 10: `stage10_party`

Adds the party block. One to four learned seats (like a player bringing one to four companions) plus the owner form a
sim group. Every seat plays the same policy and sees the other three. An empty seat has no character and only the
no-op, and the learner drops its rows. Pulls are elite-heavy. The arena runs 450 s, as stage 4's. Config: budget 600M,
evaluation every 20M steps, at least 60M steps, a party-focused report. It inherits stage 4's target, which says nothing
of teammates' deaths yet: set its own before it runs.

### Stage 11: `stage11_tanking`

**Drill.** Stage 10's party, with seat 0 always the tank (`ArenaDefinition::SeatRoles`). The ordinary party
draws every role, so the tanking lesson is smeared over whoever happened to play it; here the episode is about
holding what the pull brings and keeping it off the others. `threat_share` is the column that says whether it
happened. On the trunk: `stage12_triage` seeds from it.

### Stage 12: `stage12_triage`

**Drill.** The same party with seat 0 always the healer: keep the hurt one up, and spend mana to do it. A
forced-healer stage will find any fault in the resurrection path faster than anything else in the curriculum --
it found the farmable revive described in 4.6, where reviving a teammate paid more than keeping it alive. On
the trunk: `stage13_raid_single` seeds from it.

### Stage 13: `stage13_raid_single`

A raid of eight groups of five against one elite and its adds, won or lost as the single pack is. What is new
is the size: forty learned seats in one episode, every one playing the same policy and seeing the others in its
group. Nothing about the fight is new -- the party stages taught it -- so what is being trained is a policy that
does not fall apart when the group it is in is one of eight.

### Stage 14: `stage14_raid_gauntlet`

The raid clearing pull after pull, recovering between them, over 600 s. It is the last PvE stage: everything
the PvE line taught -- the duel, the pack, the hazard, the gauntlet's recovery, the companion, the party's
roles, the raid's size -- is in one episode. `stage23_crossroads` seeds from it.

### Stage 15: `stage15_pvp`

The PvP branch. It extends the duel and keeps only core, duel and pet, adding pvp. The pack, gauntlet, companion and
party blocks aren't in its layouts, so the PvP line can train right after the duel. Scored against `fight`. Config:
gamma 0.999 and lambda 0.99, as a fight turns on what happened tens of seconds before (a stealthy approach, a trinket
baited out).

### Stage 16: `stage16_evade`

**Drill.** A scripted enemy player **ten levels above** the seat
(`ArenaDefinition::OpponentLevelBonus`), for 120 s. The fight is not winnable straight, and that is the point:
everything up to here rewards winning the fight in front of it, so a losing fight is a class of situation the
policy has never been paid to handle and it dies with its cooldowns up. The score is being alive when the clock
runs out (`survived`).

**Time spent unseen is counted and never paid.** The optimal policy for paid seconds out of sight is to walk to
the far corner at t=0 and stand there, which is exactly the farmable shape `animus.rewards` exists to catch.
What is paid is `RewardTerm::BrokeContact`: **once per seen→unseen transition, with a cooldown**
(`Evade.BreakCooldownMs`, 5 s), so strobing around a pillar earns nothing. A break counts as a *line-of-sight*
break when it was achieved without stealth -- the effect, not the button press.

Columns: `survived`, `escaped` (a single unbroken stretch out of sight of at least `Evade.EscapeMs`, 8 s),
`contact_breaks`, `line_of_sight_breaks`, `unseen_seconds`, `unseen_longest_seconds`, `re_stealths`.

Measured `fight` baseline on this arena, 2048 episodes: `survived` 0.405, `contact_breaks` 0.514,
`unseen_seconds` 3.72, `escaped` 0.094, `won` 0.290. `fight` never tries to hide, so those breaks are incidental
terrain occlusion during a chase -- they are the floor a policy that learned nothing already clears, and the
gates sit well above them (`survived` 0.55, `contact_breaks` 0.90, `unseen_seconds` 6.0).

The level bonus is ten because six stopped being a losing fight once the spawn had cover: terrain blocks the
scripted opponent's casting as readily as it hides the seat, and the baseline's `won` went 0.188 → 0.447 on the
same change. At ten it is back to 0.290 -- still beatable about three times in ten, which is deliberate. The
lesson is recognising a losing fight and leaving it, not obeying a rule that says every fight here is lost.

The gate drops the baseline comparison (`min_over_baseline: 0`) on purpose: the yardstick would be a policy
trained to win fights that are not winnable here, so surviving is a new axis rather than a better version of
the old one. `stage19_arena` seeds from this stage, not from `stage15_pvp`, so every class carries the lesson
into self-play.

**Two things had to be true before any of this could work, and neither was.**

`Unit::CanSeeOrDetect` does not raycast -- it is grid visibility plus stealth and invisibility detection, and it
stays true through a wall. Every "can this see that" in the curriculum used it, so the drill's first run read
0.0000 for `unseen_seconds` and `contact_breaks` across 2048 episodes, and the scripted hunter could never lose
a quarry that was not stealthed, which meant its `Search` behaviour had essentially never run. `Encoding::CanSee`
now answers that question -- detect, then `IsWithinLOSInMap` -- and the hiding tracker, the scripted hunter and
the director's `SideCanSee` all go through it. (The per-seat observation filters deliberately still use the bare
check: those run for every seat in every stage, and changing what a seat observes is a different change.)

And the arena has to have something to hide behind. With line of sight working but the default open-field spawn,
twelve of the eighteen class/roles still read exactly 0.000 breaks -- only the three that can stealth registered
anything, because on flat ground a warrior cannot break line of sight at all. Both drills now spawn inside
Durnholde Keep and among the Southshore farms, on the same instance map, at exact ground coordinates taken from
the world database.

`ScriptedPlayer::Search` needs no give-up timer: it mills within `SEARCH_RADIUS` (10 yd) of the last sighting,
which is smaller than the outer cover rings `FindCover` uses (8, 16 and 26 yd), so breaking line of sight at 16
or 26 yd genuinely escapes.

### Stage 17: `stage17_hide`

**Drill.** The same losing fight six levels up rather than ten, for **every class and every race**. The lesson
is becoming unseen and staying unseen, and hiding again once the hunter has found you.

Stealth is one way to do that and the rarest: four of the eighteen class/roles have a stealth aura in their own
kit -- `rogue_dps` (Stealth) and the three druids (Prowl). It is not the lesson. Every class can get out of
sight with terrain, with distance, and with whatever its kit and its race give it -- Blink, Disengage, Feign
Death, Invisibility, Ice Block, Sprint, and Shadowmeld for any night elf of any class. So the stage grades the
**outcome**, not which button produced it.

Six levels rather than stage 16's ten, so the fight is winnable often enough that hiding is a choice rather
than the only move left. That is the whole difference between the two: stage 16 is about leaving a fight that
is lost, this one is about not being found once you have.

Columns that carry the gate:

| Column | What it says |
|---|---|
| `escaped` | One unbroken stretch out of sight of at least `Evade.EscapeMs` (8 s) -- the hunter lost the seat rather than blinked |
| `re_hides` | Contact broken again after the first time: getting back out of sight once something is already looking for you, which is the harder half and the one every class can do |
| `survived` | A sanity floor, not the thing being asked for |

Measured `fight` baseline, 2048 episodes over all eighteen layouts: `escaped` 0.079, `re_hides` 0.240,
`contact_breaks` 0.541, `unseen_seconds` 3.17, `survived` 0.551, `won` 0.450, `re_stealths` exactly 0.0000.
**Every one of the eighteen produced both gate metrics**, which is the check that mattered: no layout is asked
for something it has no way to do. The spread runs from `warlock_dps` (survived 0.257) to `deathknight_tank`
(0.781), and the layout floor is set against the bottom of it.

`re_stealths` and `stealth_openers` are **reported and never gated**. Fourteen class/roles have no stealth
button, and a gate on one would ask them for something they cannot do; the columns are still worth reading,
because they are what a rogue, a druid or any night elf actually presses.

> **An earlier version of this stage was restricted to the class/roles that could stealth, and that was wrong
> twice over.** It excluded fourteen class/roles from a lesson all of them need. And the test it used -- does
> the action catalog contain a stealth aura -- returned eleven of the eighteen, because the catalog is the
> union over every race a class may be and that union holds Shadowmeld (58984), the night elf racial. A warrior
> that rolled a human would have played a stealth stage with no stealth at all. `StageDefinition::NeedsStealth`
> and its validation are gone; nothing in the curriculum restricts a stage to a subset of class/roles.

Racials stay fully available everywhere, here and in every other stage: the action catalog carries Shadowmeld,
Will of the Forsaken, Blood Fury, Escape Artist and the rest, and `Encoding::IsSpellActionAllowed` masks each
by `HasActiveSpell`, so the race that actually rolled is the one whose racials are offered.

### Stage 18: `stage18_stealth`

**Drill, and a leaf.** The one stage in the curriculum restricted to a subset of class/roles, and the reason
the restriction is worth its cost.

Hiding and stealth are different lessons. Stage 17 is *not being found*: every class can do it, with terrain,
with distance, and with whatever its kit and race give it -- Shadowmeld included. This stage is being **close**
and not found: crossing the ground to someone who is looking for you, arriving inside strike range with the
opener still in hand, and holding there. Shadowmeld cannot do that at all, because it breaks the moment you
move. Only a real stealth aura can, so only the four class/roles whose own kit carries one play it:
`rogue_dps` (Stealth) and the three druids (Prowl). `StageDefinition::NeedsStealth` asks `ClassKit`, the class
trainers' list, so the answer is true of every member of the class rather than of one race of it.

The opponent is six levels up, as on the hide stage: the fight has to be one the opener decides, or getting
into position is a flourish before a fight that was winnable anyway.

**`RewardTerm::Stalk` is the only reward in the curriculum paid per decision rather than on a transition**, and
that is deliberate rather than an oversight. What made the order nudge farmable -- 5.01 an episode, 23.7% of
gross, cut to a fifth of a percent -- was that it paid for a state that was *free to hold*: a focus that never
changed still paid every decision. This pays only while the seat is stealthed, unseen, and within
`Stealth.StalkYards` (10 yd) of a **living** opponent that is actively looking, which is the opposite of free:
detection is a distance check the seat is losing the whole time it stands there. `Stealth.StalkMax` caps the
episode's total at 1.0 regardless, so the opener it sets up stays the larger prize and loitering cannot change
the sum.

Time unseen is still never paid. The distance condition is the entire difference between this and the farmable
shape: staying stealthed inside melee range of something hunting you is a skill, and staying unseen in the far
corner of the map is the absence of one.

| Column | What it says |
|---|---|
| `stalked_into_range` | Got inside the band at all, stealthed and unseen -- the gate's headline |
| `stalk_seconds` | Held there, rather than touching the band and being spotted |
| `stalk_longest_seconds` | ... in one unbroken approach |
| `stalk_approaches` | Times it came from outside the band to inside it |
| `closest_stealthed` | The nearest it got while stealthed. Reported, never gated: it is a distance, and a gate floor cannot say "lower is better" |
| `stealth_openers` | The position used for what it is for |

### Stage 19: `stage19_arena`

Self-play. Two learned seats of random classes and roles at one level, both played by the policy, so every fight is
training data for both sides. A policy's score against itself doesn't track progress, so evaluation uses
`eval.opponent_baseline`: the `fight` baseline plays seat 2, the score is seat 1 against it, and the baseline score is
`fight` against `fight` on the same seeds. Budget 200M.

### Stage 20: `stage20_duo_led`

Two against two under a **director**: one more agent a side, choosing the team's posture, the enemy it
concentrates on, the shape it takes, whose turn the next duty is, and -- since the place channel landed -- where
to go (4.12). It is the stage the director machinery is exercised on, and the one where the fog of war is
visible: `DirectorEncounter` sees only what its own side's living seats can see (`StageScenario::SideCanSee`),
so `director_enemies_seen` and `order_focus_unseen` distinguish a director that is blind from one that is
merely bad.

Blocks: core, duel, pack, pet, pvp, context, hostiles, support, order. Its arena sets `Directed`, and
`DirectorLearned` decides whether that director is the scripted yardstick or an agent that learns. **The
comparison between the two has not been run**, and 4.12 says why it should be before more budget goes into the
learned one.

### Stage 21: `stage21_flag`

Warsong Gulch's rules between two learned seats (4.5), extending the arena and merging travel: the fight, and mounting
between bases 100-180 yd apart, with a carrier kept on foot. Blocks: core, duel, pet, pvp, travel, flag. 300 s
episodes, first to three captures. As in the arena, evaluation plays the second seat with `fight` (which heads for the
flags on a mount). Config: gamma 0.999 and lambda 0.99, budget 300M, at least 30M steps.

### Stage 22: `stage22_warsong`

Warsong Gulch at its proper size: ten a side, both sides learned, on a real battleground instance. The flag
rules are stage 20's; what is new is that a side is ten seats and a group, so the objective has to be shared --
a carrier to escort home, a base somebody has to hold, and an enemy carrier ten of them can chase. It adds the
party block on top of the flag line.

The instance logs `GetBGObject: gameobject (type: 10) not found` repeatedly. That is pre-existing core noise,
not a stage fault.

### Stage 23: `stage23_crossroads`

Every line joins. It extends `stage14_raid_gauntlet` (the trunk and the PvE blocks) and merges `stage22_warsong`,
`stage19_arena` (the pvp block), `stage15_pvp`, `stage8_flight` (travel), `stage9_companion`, `stage4_gauntlet` and
`stage1_duel`, adding `context` and `hostiles`. Its layouts contain the PvE, PvP and travel blocks (the pet block
included). It is the last stage in the queue, and the one whose checkpoint is what ships.

| Arena | Weight | Episode | Situation |
|---|---|---|---|
| `companion` | 20 | 300 s | Stage 9's |
| `party` | 20 | 300 s | Stage 10's |
| `arena_1v1` | 15 | 60 s | Stage 18's |
| `pvp_scripted` | 10 | 60 s | Stage 15's |
| `gauntlet` | 10 | 450 s | Stage 4's |
| `duel` | 5 | 60 s | Stage 1's |
| `ambush` | 15 | 300 s | Companion gauntlet plus 1-2 ambushers arriving 20-120 s in |
| `escort_duel` | 5 | 90 s | Owner plus one enemy player, no pulls |

Learner: distilled with `teachers: auto` (each earlier arena is taught by the first parent that has it; the two new
arenas learn from reward alone), coef 1.0 halving every 50M steps. 256 evaluation episodes every 25M steps, the
arena_1v1 seat scored against `fight`. Budget 1B, at least 100M steps. Per-arena targets: +10% over baseline for the six
inherited arenas, at least baseline for `ambush` and `escort_duel`, each with at least 16 episodes.

### Pilot: `mix_duel_pvp`

Stage 15's blocks, seeded from `stage15_pvp`, merging `stage1_duel`, with `duel` and `pvp_scripted` arenas half and
half. Each is taught by the parent that trained it, and each must beat baseline by 10% on its own. It checks that one
policy can train PvE and PvP side by side on a small problem before `stage23_crossroads` mixes eight larger arenas.
It is a leaf and is trained only by name: `forge start mix_duel_pvp`.

## 4.12 Team play and the director

`SeatPlan::Teams` splits an arena's seats down the middle: `TeamSeats` a side, `TEAM_COUNT` (2) sides, seat
`s` on side `s / TeamSeats`. Two a side is an arena, ten a side is a battleground. `StageScenario::SideOf`
answers which side a seat plays for and `SideSeats` lists a side's seats in order.

A Teams arena fights the other side, not a scripted opponent: `OpponentEncounter` makes every cross-side pair
hostile, offers each seat the enemy side as selectable slots (so target selection has something to choose
between), and ends the episode when a whole side is down. One seat a side reduces to exactly the old `Mirror`
behaviour.

### The order

A **director** commands one side. It decides what the team is doing -- who to kill, what posture to hold,
where to gather, whose turn the next interrupt is -- while the seats keep deciding how. Two of the four are
things a seat provably cannot work out alone: nothing in seat 7's own view says it is next in the rotation,
and ten seats each picking their own target is the classic way to lose a fight you should win.

The order reaches a seat through the `Order` block, which has **thirteen observations and no actions**. An
order is advice, not a lever: the seat reads the posture, the rally point, the called target and whether it
holds the duty, and still chooses its own action. Everything reads zero in an arena without a director.

A call is dropped the moment it cannot be followed -- the focus when its enemy dies, the duty when its seat
does. Without that the order stands at a corpse until the director's next decision and, if it never spends
another focus action, for good: measured at 37% of all decisions carrying an order aimed at someone dead.

### Naming a place

A director can send its side somewhere: `TeamRally::Point` with a place the director named, and
`TeamPosture::Hold` to stay there rather than chase. The place channel existed end to end and was inert --
`SideOrder::Place` → `SeatView::TeamOrder::RallyPlace` → `OrderBlock`'s distance and bearing observations --
with nothing ever setting it. Only the naming was missing.

**The place is addressed as anchor + offset + ring, never as a coordinate.** One action per reachable spot
would make the action space the size of the world, which is the thing that has to keep working when this leaves
the arena for the open world. Instead the director names one field at a time, as it does with everything else:

| Field | Values |
|---|---|
| `PlaceAnchor` (6) | `TeamCentre`, `Focus`, `LastSeenEnemy`, `Objective`, `OwnBase`, `EnemyBase` |
| `PlaceOffset` (5) | `At`, `Toward`, `Away`, `Left`, `Right` |
| `PlaceRing` (2) | `Near` (`Director.PlaceNearYards`), `Far` (`Director.PlaceFarYards`) |

Thirteen actions, and they scale from a 2v2 arena to a continent unchanged, because a ring is a distance and an
anchor is whatever the side is currently about.

The offset is measured **about the anchor→enemy-centre axis** (falling back to the objective, then the last
sighting, then the side's own facing), not against absolute compass bearings. An absolute bearing would be
scale-free too, but it is not learnable from what the director observes: a director told to go "60 yards north"
cannot tell whether north is toward the enemy or off the map. Relative offsets need no extra observation and
are what a human caller actually says. (Bearings were added to the observation anyway -- `SEAT_BEARING_SIN/COS`
and `ENEMY_BEARING_SIN/COS` -- because the director previously could not tell *where* anything was, only how
far.)

`Place` and `HasPlace` are **derived**, recomputed by `ResolvePlace` every decision, so a place anchored to the
focus or the team centre tracks as they move, and `HasPlace` is true exactly when the rally is `Point` and the
anchor resolved. Two independent ways to say "there is a place" is how a channel like this drifts out of step.
The resolved point is snapped to walkable ground (`Encoding::SnapToGround`, the `Map::GetHeight` probe
`DuelBlock::FindCover` already used) -- an unsnapped place sends ten seats into a wall.

Going there is paid by `RewardTerm::PlaceMatch`, **once on crossing into the place radius, with a per-seat
cooldown**, and routed through `ShapingPaid` so the director's own reward takes it back out exactly as
`OrderMatch` is. Never per decision: the per-decision order nudge came to 5.01 an episode, 23.7% of gross, and
had to be cut to a fifth of a percent. A transition nudge with a cooldown cannot be farmed by oscillating
across the boundary.

The whole group is gated behind `ArenaDefinition::Places`, so an arena with nowhere worth sending anyone keeps
the thirteen actions masked and pays no exploration for a vocabulary it cannot use. `TeamPosture::Scout` was
deliberately **not** added: a posture is read by the whole side, so "one scouts, nine hold" is already
`Hold` + `Rally::Point` + the duty, and a second way to say the same thing is worth adding only once the
metrics show the place channel is used at all.

### The director is not omniscient

`ViewSide` used to read every enemy's `IsAlive`, health, combat and casting state, live position and even its
class/role straight out of the world with no visibility check. Now **the director sees only what its own side's
living seats can see**: `StageScenario::SideCanSee(env, side, unit)` is true when any living seat of that side
`CanSeeOrDetect`s it, so a side that wiped stops spotting. The seen-mask is computed once per decision in
`Update` and read by `ViewSide`, because a `SideCanSee` per slot per side would be O(own × enemy)
stealth-and-invisibility checks on the world thread -- forty a side a decision in a ten-a-side fight.

An enemy slot is one of three things:

- **Seen now** -- live values.
- **Seen before** -- the remembered position and health, with `ENEMY_UNSEEN_TIME` saying how stale it is
  (scaled by `MAX_UNSEEN_TIME_MS`, 20 s). `ENEMY_CASTING` and `ENEMY_IN_COMBAT` go to **zero**, not to their
  remembered values: they are instantaneous facts, and a stale one is a lie the policy would learn to trust.
- **Never seen** -- presence only, and `ACTION_FOCUS_FIRST + slot` is masked. `ENEMY_PRESENT` now means "ever
  seen".

Memory is per side, keyed by slot rather than GUID (the GUID is held only to notice a slot being reassigned),
which keeps an allocation off the per-decision path.

Three leaks survived the obvious fix and had to be closed separately. `Forget` cleared a focus whose target
died -- to a learned director, a call quietly vanishing *is* the ground-truth signal "he is dead" -- so it is
gated on remembered state when the director is learned. `Call` refused a focus slot whose bot was not alive,
and the refusal was observable through `SinceCall` not advancing, so it now accepts any slot the side
*believes* alive. And a seat's own `OrderBlock` focus observation read a live `Unit*` with no visibility
filter, so a seat could read a called focus's distance and health through a wall; it is now filtered by the
seat's own sight, falling back to the order's last-known place.

**Two deliberate non-changes.** The **critic stays omniscient** -- `WriteState` is unfiltered on purpose;
centralised critic with decentralised execution is what MAPPO is, and filtering it would make the value
function worse for nothing. And the **scripted director keeps reading ground truth**: it is a yardstick and
instrumentation, not a policy, and a yardstick that had to scout would stop being fixed.

This makes the learned director strictly *less* informed than the omniscient yardstick it is scored against, so
the directed stages' `min_over_baseline` was relaxed in the same change, and `order_focus_unseen` and
`director_enemies_seen` were added so a director that is blind is distinguishable from one that is merely bad.

### Scripted or learned

`ArenaDefinition::Directed` gives an arena a director; `DirectorLearned` makes it an agent rather than a
script.

The **scripted** director (`DirectorEncounter::Command`) thinks every 10 decisions and calls the lowest-health
enemy, rotating the duty around the side and switching to Recover below 40% average health. It is the yardstick:
its call quality is near-perfect by construction, so it is what a learned director is measured against.

The **learned** director is an ordinary layout (`DirectorLayout`) with its own 257 observations and 42 actions,
seeded down the stage chain like any other. Two more agents an env, one a side, opted into per arena; an
undirected episode marks them absent rather than resizing anything.

Its action space is one call per decision, not several heads at once: `hold`, then posture (6), rally (8),
place anchor (6), place offset (5), place ring (2), focus slot (`PACK_SLOTS`), duty slot (`TEAM_SEATS`). The
standing order is state it edits, and an action names the single field it changes -- everything else keeps what
it was. That is closer to what a leader does than four
simultaneous heads would be (a call stands until it is changed) and it needs no new transport: the wire carries
one categorical action per agent.

It is paid the mean of its side's seat rewards, less any order-compliance shaping those seats earned, because
that is the one part of their reward it can move without the fight going any better. A director that kept it
would learn to call whoever its seats were already fighting.

It decides on a **slower clock** than the seats -- `mappo.slow_layout` in the learner -- choosing every
`slow_every_decisions` and holding in between, with its transitions stored and discounted over its own
decisions. Both clocks are printed when a run starts:

```
Per 250 ms decision:         gamma 0.99750 (horizon 100 s), GAE trace 0.97275 (credit   9.2 s)
Per 2.5 s director decision: gamma 0.99600 (horizon 625 s), GAE trace 0.97608 (credit 105 s)
```

The cadence is not only about credit. With the director choosing every decision it moved the standing order on
0.706 of them, against the scripted director's 0.03; nothing can follow a call that changes every 1.4
decisions.

### What to measure

A director is worth having only where it beats its own absence, so a directed stage is read against the
undirected one it came from. The columns, all reported per seat:

| Column | What it says |
|---|---|
| `order_focus_alive` | The call named a living enemy at all |
| `order_focus_lowest` | ... and it was the most hurt of them, the call the scripted director makes |
| `order_focus_chance` | What naming one of the living at random would have scored |
| `order_focus_kept` | The share of a side's seats on the called target |
| `order_changes` | How often the call moved |
| `order_focus_unseen` | The called enemy was one the side could not see -- a blind call, not a bad one |
| `director_enemies_seen` | How many enemy slots the side could see, out of those present |
| `order_place_called` | A place was named at all: the first non-zero reading is what says the channel stopped being inert |
| `order_place_reached` | ... and a seat got there |
| `order_place_distance` | How far the side was from it |

`lowest` against `chance` is the one that matters, and it is the same scale whether a side is two or ten: it
tells a director that calls well apart from one the arena makes look good. `order_focus_kept` alone cannot --
a seat that ignores its director and a director that names nothing worth fighting look identical in it, and at
two a side its chance floor is 0.5, because a seat parked on the first enemy slot is on the called target half
the time by construction.

**Honest status.** As of this writing the learned director has not beaten chance on `lowest` in four 30M runs,
through a compliance reward, a slower clock and a clean channel. The seats improve (evaluation 6.6-6.9 to
8.3-8.8 in every run) but that is them learning two on two, and it happens just as much without a director.
The open question is whether a director is worth anything at two a side at all, which the scripted director
answers directly: run `stage20_duo_led` with `DirectorLearned` off and compare. If a perfect caller does not
beat the undirected arena, there is nothing at this stage for a learned one to find.
