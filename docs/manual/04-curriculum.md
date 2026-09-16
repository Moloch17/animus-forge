# 4. The curriculum

The curriculum is the set of scenarios the policies train on. It lives in animus-lib under
`src/Scenario/Curriculum/`. It has eleven stages that train one policy for all 18 class/roles, starting from a
one-on-one fight. One branch ends with a stage that mixes PvE and PvP; another teaches getting somewhere (riding,
flying) and plays Warsong Gulch's rules.

```
stage1_duel ─┬─ stage2_pack ─ stage3_gauntlet ─ stage4_companion ─ stage5_party ─┬─ stage8_crossroads
             ├─ stage6_pvp ─ stage7_arena ─┬──────────────────────────────────────┘
             │                             └─ stage11_flag
             └─ stage9_travel ─┬─ stage10_flight      (stage11_flag also merges stage9_travel)
```

| # | Stage | Extends (merges) | Blocks | Seats | Opposition | Episode |
|---|---|---|---|---|---|---|
| 1 | `stage1_duel` | none | core, duel, pet | 1 | A same-level creature, out of aggro range | Ends on the kill or death |
| 2 | `stage2_pack` | stage1 | + pack | 1 | A pack of 2-4, usually linked | Ends on clear or death |
| 3 | `stage3_gauntlet` | stage2 | + gauntlet | 1 | Pull after pull with breaks | Runs until death or the time limit |
| 4 | `stage4_companion` | stage3 | + companion | 1 | The gauntlet beside a scripted owner | Full length, deaths recover |
| 5 | `stage5_party` | stage4 | + party | 1-4 + owner | Elite-heavy pulls, in a real group | Full length |
| 6 | `stage6_pvp` | stage1 | core, duel, pet, pvp | 1 | A scripted enemy player | Ends when either dies |
| 7 | `stage7_arena` | stage6 | core, duel, pet, pvp | 2 (self-play) | The other seat | Ends when either dies |
| 8 | `stage8_crossroads` | stage5 (stage7, 6, 4, 3, 1) | the PvE and PvP ten | up to 4 | Eight arenas, including ambushes | Per arena |
| 9 | `stage9_travel` | stage1 | core, duel, pet, travel | 1 | A place 60-320 yd away by path, level 20+ | Ends on arriving or death |
| 10 | `stage10_flight` | stage9 | core, duel, pet, travel | 1 | A place 350-700 yd away in Nagrand, level 60+ | Ends on arriving or death |
| 11 | `stage11_flag` | stage7 (stage9) | core, duel, pet, pvp, travel, flag | 2 (self-play) | Warsong Gulch's rules, level 20+ | First to three captures |
| - | `mix_duel_pvp` (pilot) | stage6 (stage1) | core, duel, pet, pvp | 1 | Duel or scripted player, half and half | Per arena |

`mix_duel_pvp` is not in the default queue of `forge start`. It exists to test arena mixing, merge seeding and
distillation on a small problem. Train it by name; a plain `forge fast` trains it with every other stage.

## 4.1 Defining a stage

A stage is one `StageDefinition` entry in `Stages/Stages.cpp`:

```cpp
stages.push_back({
    .Name = "stage4_companion",          // scenario name
    .Suffix = "_companion",              // model names: warrior_tank_companion
    .Extends = "stage3_gauntlet",        // seeds from it (the trunk)
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
| `Seats` | `Solo` (1), `Party` (4 slots, 1-4 filled each episode), `Mirror` (2 that fight each other) |
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
instanceable, like Outland for `stage10_flight`) every env shares the map: each env's seats take one of the spawn
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
5. **Pick the seats.** A party arena draws its size from `Party.SizeWeight1-4`. Half the time
   (`Party.ClassicChance`) the roles are the classic tank, healer, DPS, DPS in shuffled order. Otherwise each seat's
   role is drawn (`RoleTankChance`, `RoleHealerChance`). Each seat then takes a random layout of its role, or any layout
   if the run has none of that role. Other arenas give every seat any layout. A training episode draws it by the
   learner's per-layout weights (`WEIGHTS`, evenly without them); an evaluation episode doesn't draw at all: seed
   *i* plays candidate *(i + seat) mod count*, so every class/role is scored on an equal share of the seeds. Seats
   beyond the active count get no layout and no bot.
6. **Pick one level** every seat's class can be (death knights start at 55). It is `StageSettings::Level` if set;
   otherwise `Characters.HighLevelChance` percent of the time a level from `HighLevelFirst` to 80,
   `Characters.LowLevelChance` percent of the time a level from the class minimum to `LowLevelLast` (20; skipped when
   the class can't be that low), else any level from the class minimum to 80.
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
2. `AcceptResurrections`: dead seats and the owner accept a pending resurrection request as a client would, and the
   seat that cast it is credited (`StepRevivedAlly`, `Revives`),
3. for each seat with a living bot: `CurrentTarget` (the first encounter whose `SelectTarget` answers, else target 0),
   `BeforeSeatAction` on every encounter, build the `SeatView`, `SeatEncoder::Apply`, fold the result into the seat's
   totals, `OnSeatAction` on every encounter, and summon a called hunter beast.

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
  suitable flask (half the time an elixir while levelling). Gauntlet stages add five of the best vendor food and, for
  mana users, five drinks.
- **`PrepareFighter`**. No XP gain (levelling would change the character under the model), a warrior's stance (a first
  login normally casts it, and nothing works without one), and for hunters a stable offer of four tameable beasts of
  different random families.

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
- `Sustain()`: the sustain spells above, which the companion and party blocks cast on allies.
- `Revives()` (companion and party blocks): Resurrection, Redemption, Ancestral Spirit, Revive, Rebirth, and a
  warlock's soulstone.

A layout's **ally spells** (companion and party blocks, `Layout::AllySpells`) are every positive spell of the sustain
list and the catalog that takes a friendly unit target: its heals first (`AllyHealCount`), then shields, Hands,
Innervate, Misdirection, Tricks of the Trade, Earth Shield, Power Infusion, blessings and the like.

Every action is masked each decision by the core's own `Spell::CheckCast`, run without casting (race, level, talent,
cooldown, GCD, power, stance, range, reagents). As on a client, **no spell or item can start while a cast is in its cast
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
| `gauntlet` | Pulls cleared, pull active, time to the next pull, time into the pull, elite or higher-level pull, eating, drinking, food and drink left (the sustain spells are core actions) | Eat, drink |
| `companion` | The owner's presence, health, mana, distance, bearing, combat, movement, level difference and class; enemies on it; which slot it attacks; which enemies attack it; each ally spell's and revive's known and cooldown | Follow, assist (owner's target), guard (an enemy attacking the owner), one cast-on-owner per ally spell, one revive-on-owner per revive |
| `party` | Living party size, the most hurt ally's health, living tank and healer present; per teammate: presence, health, mana, distance, bearing, combat, role, class, attackers, target slot, which enemies attack it | Follow the tank; per teammate: assist, guard, ally spells, revives |
| `pvp` | The opponent's class, role, level difference, mana, rage/energy/runic power, crowd-controlled, stealthed, pet out, casting a heal; the bot stunned/feared, rooted or silenced; whether the opponent is a learned agent; what a player tracks from what it saw used: the opponent's trinket cooldown, racial control break cooldown and number of spells of a minute or more cooling down; diminishing returns (controlled and opening stuns, fear, disorient, root, silence, horror, cyclone) on the opponent and on the bot, and the crowd control each has left; the opponent hidden (then only class, role, level, the cooldowns and diminishing returns are written) | none |
| `context` (12) | Owner present and alive, living teammates, living enemy players and creatures in the slots, nearest enemy player's distance, a player attacks the bot or the owner, PvP flag, battleground/arena map, dungeon/raid map, self-resurrection allowed, group size | none |
| `hostiles` (14 per slot) | Per enemy slot: player or creature, class, casting a heal, stealthed, pet out | none |
| `travel` (16) | Mounted, on a flying mount, can summon a ground or flying mount now, riding skill, indoors, height above the ground; the objective's presence, distance, bearing and height; at the objective; in combat; speed; moving | Mount the fastest ground mount, mount the fastest flying mount, dismount, move to the objective (by path, or straight in the air), climb 15 yd, descend 15 yd |
| `flag` (17) | Carrying the other side's flag; the seat's flag at base, carried or dropped; the other's at base or dropped; distance and bearing to both bases and to the nearest dropped flag; both scores | none |

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
`ReviewChance` (25%) of its training fights come from a lower tier, so none is forgotten. A fight that simple play wins
every time teaches nothing a plan would add. An evaluation spreads its seeds over every tier (tier = seed index
mod (`MaxTier` + 1)), so two checkpoints meet the same fights, and the summary scores each tier on its own
(`difficulties`; stage targets can gate a tier, `target.difficulties`). Tiers restart at 0 with the worldserver.
`difficulty` and `opponent_elite` in the episode info say what each fight was. The creature is summoned at the seat's
level plus its tier's levels (`PendingSummonLevel`) 40-50 yd away at a random line-of-sight bearing on level ground
the seat can walk to (a path at most 1.5 times the straight line), facing a random way, hostile and aggressive, without
health regeneration. It starts out of aggro range. A creature with no path to its victim stops and regenerates, then
evades home at full health after 10 s, which no play can win: after 3 s without a path it is put beside its victim
instead (as instance trash is with `Creature.Instance.TeleportToUnreachableTarget`). `target_unreachable_seconds` and
`target_teleports` count it. Rewards are the one-on-one terms (`CombatReward::OneOnOne`). The episode is terminal on the
kill or on death with no resurrection left.

**`PullsEncounter`** (`Opposition::Pulls`). The pack pool adds creatures whose SmartAI only casts or talks (about 3500
casters and ability users) to the duel pool.

- **Single pack** (stage 2): 2-4 creatures at the seat's level, clustered 40-50 yd away. `Pulls.LinkedChance` (70%)
  are linked, meaning once one member is in combat the rest attack. The episode is terminal on clear or death.
- **Gauntlet** (stages 3-5, 8): 1-4 creatures, or `EliteChance` (15%) a single elite, or `HigherLevelChance` (25%) a
  pack 1-3 levels higher. In a party arena each member is elite with `PartyEliteChance` (50%) and up to 2 levels above.
  After a clear the field empties and the next pull spawns `NextPullMinMs`-`NextPullMaxMs` (8-20 s) later, out of aggro
  range. With an owner, pulls spawn around the owner.
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
`carrier_kill`, `flag_lost`, `timeout`.

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
- stall (creature duel only): -0.05 per second the fight hasn't started once `Duel.StallGraceMs` (15 s) of the episode
  are gone. The timeout comes 900 decisions later, too far for the policy to tell standing still from closing in: at
  20M steps stage1_duel's deterministic policy stood where it spawned for the whole episode in 67 of 2048 evaluation
  fights
- spacing (creature duel, ranged specs): -0.03 per second the opponent stands in melee range attacking the seat. The
  approach term only pays for closing in, so nothing kept a hunter, mage or warlock at its range
- repeats (every stage): -0.02 per press of the same action past the free ones in its window (see Repeats, 4.3)
- winning outweighs winning fast: with the kill at 10, speed at most 1 and a loss at -10, a risky fast opener only pays
  more than a sure slow win above about 97% odds (at the earlier 3, 3 and -3 it was 79%)

**Pack** (`Pulls.*`): damage x2 of the pack's total health, damage taken x1, approach to the nearest enemy, +0.5 per
kill, +0.3 per interrupt, the stealth terms. Clear: +2, up to +3 for the episode length left since a pack member
entered combat, up to +2 for health kept. Death -3.

**Gauntlet**: the pack's per-step terms with damage taken x1.5. Each cleared pull: +2, up to +2 for clearing within a
minute of engaging it (not of its spawn, so resting, sapping or stealthing in first is free), up to +2 for health kept
during the pull. Death -5.

**Companion** (`Owner.*`, added to the gauntlet's, with kills and clears x2):

- everyone: owner damage taken (x1 for DPS, x2 for tanks and healers; a quarter of that when the owner is the tank);
  -0.01 per decision in combat while the owner isn't; +0.0005 per decision within 12 yd out of combat, -0.002 beyond
  25 yd; -6 per owner death; +1.5 when an ally the seat resurrected stands up
- tanks: +0.002 per enemy on the tank and -0.02 per enemy on the owner, per decision; half of the gauntlet's damage
  taken refunded
- healers: effective healing on the owner x2 (overhealing earns nothing, because the heal hook reports health gained)
- DPS and healers: -0.004 per enemy attacking them, per decision

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
  3. with the companion block and ally heals: heal a living, hurt owner (cancel a form first if needed),
  4. with the party block: heal the first hurt, living teammate a heal can reach,
  5. with the pet block and a pet class: out of combat with no living pet, call a stable beast or cast the best summon
     (Felguard, Voidwalker, Felhunter, Succubus, Imp; Raise Dead; Water Elemental); with a pet out, send it at the
     target,
  6. a spec of the ranged band (hunters, casters, healers) holds range: more than 28 yd from a living target, move
     to casting range (24 yd); with the target out of line of sight, move toward it; a hunter the target is hitting
     in melee reach while its pet attacks the target backs off 10 yd (its shots can't be used there) and lets the
     pet hold it; auto-attack only once the target is in melee reach (a hunter with no pet yet, or one still held),
  7. a melee spec starts auto-attack,
  8. and moves to a living target beyond melee reach while not already moving,
  9. otherwise its rotation (not `greedy`), first match wins:
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
  `deaths`, `wipes`
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

### Stage 1: `stage1_duel`

A new character against a real creature (4.5), with the class's whole kit, in 90-second episodes (a timeout is a
lost fight, and a healer against a creature with twice the usual health needs the time). Nothing seeds it, so its
networks start from scratch. Hunters are
offered four beasts each episode through `call_beast` actions, because Call Pet needs a pet saved in the database. The
observation shows each beast's family and pet type, so the policy can learn its preference. Warlock demons, Raise
Dead, Water Elemental and Feral Spirit are ordinary spell actions with their reagents in the bags. The bot gains no XP.

Pets are played as a player has them:

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
enemy pulls all of them. The interrupt reward teaches casting interrupts at the right moment. Config: inherits
stage 1.

### Stage 3: `stage3_gauntlet`

Adds the gauntlet block: sustained combat, recovery between pulls with food, drink and the core's sustain spells. Between pulls
there is no target, so target features are 0 and only self-cast actions are allowed. Needs long episodes (several
minutes of `AnimusForge.EpisodeSeconds`). Config: gamma 0.999 and lambda 0.99 (~100 s horizon, ~9 s credit trace, so
resting before a pull or stealthing in is tied to the clear it pays for), rollout 256, budget 400M, at least 40M steps.

### Stage 4: `stage4_companion`

Adds the companion block and the scripted owner. The seat learns to follow, assist, guard, heal and resurrect it, and
role-specific behaviour appears (tank threat, healer throughput, DPS threat discipline). Deaths recover after pulls and
the episode always runs its full length, so letting the owner die is never a way to escape penalties.

### Stage 5: `stage5_party`

Adds the party block. One to four learned seats (like a player bringing one to four companions) plus the owner form a
sim group. Every seat plays the same policy and sees the other three. An empty seat has no character and only the
no-op, and the learner drops its rows. Pulls are elite-heavy. Config: budget 600M, evaluation every 20M steps, at
least 60M steps, a party-focused report.

### Stage 6: `stage6_pvp`

The PvP branch. It extends the duel and keeps only core, duel and pet, adding pvp. The pack, gauntlet, companion and
party blocks aren't in its layouts, so the PvP line can train right after the duel. Scored against `fight`. Config:
gamma 0.999 and lambda 0.99, as a fight turns on what happened tens of seconds before (a stealthy approach, a trinket
baited out).

### Stage 7: `stage7_arena`

Self-play. Two learned seats of random classes and roles at one level, both played by the policy, so every fight is
training data for both sides. A policy's score against itself doesn't track progress, so evaluation uses
`eval.opponent_baseline`: the `fight` baseline plays seat 2, the score is seat 1 against it, and the baseline score is
`fight` against `fight` on the same seeds. Budget 200M.

### Stage 8: `stage8_crossroads`

Both branches join. It extends `stage5_party` (trunk and PvE blocks) and merges `stage7_arena` (the pvp block),
`stage6_pvp`, `stage4_companion`, `stage3_gauntlet` and `stage1_duel`, and adds `context` and `hostiles`. Its layouts
contain the ten PvE and PvP blocks (the pet block included).

| Arena | Weight | Episode | Situation |
|---|---|---|---|
| `companion` | 20 | 300 s | Stage 4's |
| `party` | 20 | 300 s | Stage 5's |
| `arena_1v1` | 15 | 60 s | Stage 7's |
| `pvp_scripted` | 10 | 60 s | Stage 6's |
| `gauntlet` | 10 | 300 s | Stage 3's |
| `duel` | 5 | 60 s | Stage 1's |
| `ambush` | 15 | 300 s | Companion gauntlet plus 1-2 ambushers arriving 20-120 s in |
| `escort_duel` | 5 | 90 s | Owner plus one enemy player, no pulls |

Learner: distilled with `teachers: auto` (each earlier arena is taught by the first parent that has it; the two new
arenas learn from reward alone), coef 1.0 halving every 50M steps. 256 evaluation episodes every 25M steps, the
arena_1v1 seat scored against `fight`. Budget 1B, at least 100M steps. Per-arena targets: +10% over baseline for the six
inherited arenas, at least baseline for `ambush` and `escort_duel`, each with at least 16 episodes.

### Stage 9: `stage9_travel`

Getting somewhere, off the duel. A character of level 20 or more, with its level's riding and its side's mounts, starts
in Old Hillsbrad (which allows mounts) with a place 60-320 yd away by path. A mount's cast time only pays on a long
trip, and arriving on foot is what lets it fight at the end. Blocks: core, duel, pet, travel. 150 s episodes. Config:
budget 150M, at least 20M steps, a travel report.

### Stage 10: `stage10_flight`

Flying, off travel. Characters of level 60 or more (Expert Riding, and Artisan with a fast flying mount from 70) start
in Outland's Nagrand, where flying mounts fly, at one of eight spawn points, each env in its own phase. The place is
350-700 yd away: flying is several times faster and passes over everything, but dismounting in the air falls with a
player's fall damage, so the policy learns to take off, keep a height, land and dismount. Battlegrounds never allow
flying mounts (the zone must be Outland or Northrend, `SpellInfo::CheckLocation`), so this is for the open world.
180 s episodes. Config: gamma 0.999 and lambda 0.99, budget 150M.

### Stage 11: `stage11_flag`

Warsong Gulch's rules between two learned seats (4.5), extending the arena and merging travel: the fight, and mounting
between bases 100-180 yd apart, with a carrier kept on foot. Blocks: core, duel, pet, pvp, travel, flag. 300 s
episodes, first to three captures. As in the arena, evaluation plays the second seat with `fight` (which heads for the
flags on a mount). Config: gamma 0.999 and lambda 0.99, budget 300M, at least 30M steps.

### Pilot: `mix_duel_pvp`

Stage 6's blocks, seeded from `stage6_pvp`, merging `stage1_duel`, with `duel` and `pvp_scripted` arenas half and half.
Each is taught by the parent that trained it, and each must beat baseline by 10% on its own. It checks that one policy
can train PvE and PvP side by side before stage 8 mixes larger arenas.
