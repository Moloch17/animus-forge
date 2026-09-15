# 4. The curriculum

The curriculum is the set of scenarios the policies train on. It lives in animus-lib under
`src/Scenario/Curriculum/`. It has eight stages that train one policy for all 18 class/roles, starting from a
one-on-one fight and ending with a stage that mixes PvE and PvP.

```
stage1_duel ─┬─ stage2_pack ─ stage3_gauntlet ─ stage4_companion ─ stage5_party ─┬─ stage8_crossroads
             └─ stage6_pvp ─ stage7_arena ────────────────────────────────────────┘
```

| # | Stage | Extends (merges) | Blocks | Seats | Opposition | Episode |
|---|---|---|---|---|---|---|
| 1 | `stage1_duel` | none | core, duel | 1 | A same-level creature, out of aggro range | Ends on the kill or death |
| 2 | `stage2_pack` | stage1 | + pack | 1 | A pack of 2-4, usually linked | Ends on clear or death |
| 3 | `stage3_gauntlet` | stage2 | + gauntlet | 1 | Pull after pull with breaks | Runs until death or the time limit |
| 4 | `stage4_companion` | stage3 | + companion | 1 | The gauntlet beside a scripted owner | Full length, deaths recover |
| 5 | `stage5_party` | stage4 | + party | 1-4 + owner | Elite-heavy pulls, in a real group | Full length |
| 6 | `stage6_pvp` | stage1 | core, duel, pvp | 1 | A scripted enemy player | Ends when either dies |
| 7 | `stage7_arena` | stage6 | core, duel, pvp | 2 (self-play) | The other seat | Ends when either dies |
| 8 | `stage8_crossroads` | stage5 (stage7, 6, 4, 3, 1) | all nine | up to 4 | Eight arenas, including ambushes | Per arena |
| - | `mix_duel_pvp` (pilot) | stage6 (stage1) | core, duel, pvp | 1 | Duel or scripted player, half and half | Per arena |

`mix_duel_pvp` is not in the default queue. It exists to test arena mixing, merge seeding and distillation on a small
problem. Train it by name.

## 4.1 Defining a stage

A stage is one `StageDefinition` entry in `Stages/Stages.cpp`:

```cpp
stages.push_back({
    .Name = "stage4_companion",          // scenario name
    .Suffix = "_companion",              // model names: warrior_tank_companion
    .Extends = "stage3_gauntlet",        // seeds from it (the trunk)
    .Summary = "the gauntlet beside a scripted owner: follow, assist, guard and heal it",
    .Blocks = { Core, Duel, Pack, Gauntlet, Companion },   // layout order
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
| `Against` | `Creature`, `Pulls`, `ScriptedPlayer`, `MirrorSeat`, `Ambush` |
| `Schedule` | `None`, `SinglePack` (ends on clear), `Gauntlet` (pull after pull) |
| `Owner` | A scripted owner the seats fight for |
| `PartyGroup` | The owner and seats form a core group |
| `Pvp` | Resilience gear, no self-resurrection |
| `EpisodeSeconds` | 0 = the host's `EpisodeSeconds` |
| `Ambushers` | 0-2 scripted enemy players who attack the owner |

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
  - mirror seats without `MirrorSeat`, or `MirrorSeat` without mirror seats
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
   if the run has none of that role. Other arenas give every seat a uniformly random layout. Seats beyond the active
   count get no layout and no bot.
6. **Pick one level** every seat's class can be (death knights start at 55). It is `StageSettings::Level` if set;
   otherwise `Characters.HighLevelChance` percent of the time a level from `HighLevelFirst` to 80, else any level from
   the class minimum to 80.
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
  is any layout with the gauntlet block (food, drink, sustain spells).
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

- **Talents.** The spec's standard 3.3.5 build from `Character/SpecBuilds.cpp` (31 specs), which is generated by
  `tools/spec_builds/generate.py` from `builds.py` and checked by `validate.py`. A build lists talents in the order
  players take them. Each point goes to the first talent in that order that still wants ranks and is legal (row and
  prerequisite rules follow `Player::LearnTalent`), so a low-level character has the talents players pick first. Every
  build spends exactly 71 points at level 80. Glyph slots the level has opened get the spec's standard major and minor
  glyphs.
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
  only at 80), death knight runeforges, and rogue poisons. No profession-only enchants or gems.
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
  shapeshifts or summons. Movement, travel, crafting, pure heals and utility are excluded,
- one action per trinket slot.

The catalog also provides three more lists that later blocks use:

- `Tactical()` (pack block): interrupts, stuns, silences, fears, roots, polymorphs, knockbacks, taunts and offensive
  dispels.
- `Sustain()` (gauntlet block): heals, HoTs, absorbs and friendly dispels.
- `Revives()` (companion and party blocks): Resurrection, Redemption, Ancestral Spirit, Revive, Rebirth, and a
  warlock's soulstone.

Every action is masked each decision by the core's own `Spell::CheckCast`, run without casting (race, level, talent,
cooldown, GCD, power, stance, range, reagents). As on a client, **no spell or item can start while a cast is in its cast
time**. The core only enforces that for client casts, and without the check a bot's new cast would silently replace the
current one.

Casting goes through `ApplySpellAction`, which builds the `SpellCastTargets` a client would send for the spell.

## 4.4 Blocks

| Block | Observation (summary) | Actions |
|---|---|---|
| `core` | 61 globals (see below), then 5 features per catalog action (known, cooldown, aura on target, aura on self, stacks), then rank / max rank per class talent, then points per tree / 71 | The catalog |
| `duel` | Distance and bearing to the target, behind it, it faces the bot, its combat, target and casting state; the bot's movement, combat, stealth and auto-attack; damage taken last step; pet out, health, attacking; combat time; current cast progress and time left; a cancellable form; potions, healthstones and bandages carried and their cooldowns; Recently Bandaged; can resurrect itself; hunters' stable families and pet types | Move to target, move behind, move to casting range (25 yd), back off 10 yd, stop, start attack, pet attack, stop casting, cancel form, healing potion, mana potion, healthstone, bandage self, soulstone self (warlock), resurrect self, 4 call-beast actions (hunter) |
| `pack` | Living and in-combat enemy counts; 4 enemy slots (present, alive, health, distance, bearing, behind, attacking the bot or its pet, casting, in combat, crowd-controlled, current target, elite, level difference); each tactical spell's known and cooldown | Select target slot 1-4; tactical spells |
| `gauntlet` | Pulls cleared, pull active, time to the next pull, time into the pull, elite or higher-level pull, eating, drinking, food and drink left; each sustain spell's known and cooldown | Eat, drink; sustain spells |
| `companion` | The owner's presence, health, mana, distance, bearing, combat, movement, level difference and class; enemies on it; which slot it attacks; which enemies attack it; each ally heal's and revive's known and cooldown | Follow, assist (owner's target), guard (an enemy attacking the owner), one heal-on-owner per ally heal, one revive-on-owner per revive |
| `party` | Living party size, the most hurt ally's health, living tank and healer present; per teammate: presence, health, mana, distance, bearing, combat, role, class, attackers, target slot, which enemies attack it | Follow the tank; per teammate: assist, guard, heals, revives |
| `pvp` | The opponent's class, role, level difference, mana, rage/energy/runic power, crowd-controlled, stealthed, pet out, casting a heal; the bot stunned/feared, rooted or silenced; whether the opponent is a learned agent | none |
| `context` (12) | Owner present and alive, living teammates, living enemy players and creatures in the slots, nearest enemy player's distance, a player attacks the bot or the owner, PvP flag, battleground/arena map, dungeon/raid map, self-resurrection allowed, group size | none |
| `hostiles` (14 per slot) | Per enemy slot: player or creature, class, casting a heal, stealthed, pet out | none |

The core block's 61 global features are: level; race one-hot (10); spec one-hot (3); health; mana; rage; energy; runic
power; six runes; combo points; form one-hot (13); GCD; casting; queued next-swing; main-hand, off-hand and ranged
swing timers; main-hand speed; target health; target distance; in melee range in front; attack power; spell power;
melee and spell crit; melee and spell haste; melee and spell hit; expertise; armor penetration; last-step damage;
last-step power change. All are normalised (see `Blocks/CoreBlock.h` for the scale of each).

Movement and casting constrain each other: movement actions are masked while casting, and cast-time or channelled
spells are masked while running. The bot turns to face its target whenever it isn't running. Stop casting and cancel
form need no target, so they stay available between pulls.

With the pack block, every spell, movement and pet action aims at the **selected enemy**. When it dies, the nearest
living enemy becomes the selection.

## 4.5 Encounters

An `Encounter` owns one part of what an env contains besides the seats. It keeps its own per-env state and has hooks
for each phase: `RewardTerms`, `AddEpisodeInfo`, `ResetEpisode`, `BeforeRebuild`, `Build`, `UpdateEnemies`, `Update`,
`SelectTarget`, `BeforeSeatAction`, `OnSeatAction`, `View`, `BeforeRewards`, `Reward`, `AfterRewards`, `WriteState`,
`IsTerminal`, `OnRecovered`, `OnPullStarting`, `Deactivate`, `Teardown`.

**`CreatureEncounter`** (`Opposition::Creature`). It spawns a random creature whose natural level range covers the
seat's level: normal rank, attackable, default AI with no script, no NPC services, not a civilian, guard or trigger,
and spawned somewhere in the world. The creature is summoned at the seat's level (`PendingSummonLevel`) 40-50 yd away
at a random line-of-sight bearing on level ground, facing a random way, hostile and aggressive. It starts out of aggro
range. Rewards are the one-on-one terms (`CombatReward::OneOnOne`). The episode is terminal on the kill or on death
with no resurrection left.

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
`SelfHealBelow`. In a mirror arena the "opponent" is the other seat. Both players get opposing factions and the PvP
flag. Against a scripted player the episode is terminal when the seat dies or kills it. In a mirror arena it is
terminal when either seat dies. PvP arenas allow no self-resurrection.

**`AmbushEncounter`** (`Ambushers > 0`). One or two scripted enemy players with the opponent's class, role and gear
rules.

- Beside pulls (`ambush` arena), they arrive `Ambush.MinMs`-`MaxMs` (20-120 s) into the episode, engage within
  `EngageMaxMs`, attack the owner while it lives and then the nearest seat. They take enemy slots the pulls leave free
  (a pull has at most 4 minus the arena's ambushers creatures). Every seat earns `Ambush.Kill` (3) per ambusher killed,
  and the pulls and owner rewards pay the rest.
- Alone (`escort_duel`, `Opposition::Ambush`), exactly one ambusher is the whole fight from the start, paid as a
  one-on-one against it.

The pvp block observes the first living ambusher.

## 4.6 Rewards

### The ledger

Each seat has a `RewardLedger`. An encounter adds `(term, value)` pairs, and the ledger sums the decision's total and
each term's episode total. Every term that any encounter of the stage pays becomes an episode info column
`reward_<term>`, so TensorBoard shows exactly what the stage pays for.

Terms: `damage_dealt`, `damage_taken`, `step_cost`, `casting`, `approach`, `stealth_opener`, `interrupt`, `kill`,
`clear`, `health_kept`, `death`, `owner_damage_taken`, `owner_healing`, `tank_damage_refund`, `threat`, `solo_fight`,
`follow`, `owner_death`, `teammate_damage_taken`, `teammate_healing`, `teammate_threat`, `teammate_death`, `revive`,
`player_kill`.

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
  3.5 yd, ranged 25 yd; 0.5 per 40 yd closed), stealth opener +0.5, step cost 0.0002
- casting: -0.03 per second already spent on a cast-time spell that didn't finish, +0.03 per second of cast time for
  each one that finished in combat (channels pay through their ticks)
- kill: +2, plus up to +3 for the share of the episode left, plus up to +2 for the share of health kept
- death: -3 each time, including after a self-resurrection. With a self-resurrection available the seat has
  `Resurrection.GraceMs` to use it before the episode ends

**Pack** (`Pulls.*`): damage x2 of the pack's total health, damage taken x1, approach to the nearest enemy, +0.5 per
kill, +0.3 per interrupt, +0.5 stealth opener. Clear: +2, up to +3 for time left, up to +2 for health kept. Death -3.

**Gauntlet**: the pack's per-step terms with damage taken x1.5. Each cleared pull: +2, up to +2 for clearing within a
minute, up to +2 for health kept during the pull. Death -5.

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

## 4.7 Scripted baselines

`Baselines::Choose` reads a seat's row through its layout, so the baselines follow layout changes automatically.

- **`greedy`**: the first allowed spell or trinket in catalog order. Every layout supports it.
- **`fight`** (layouts with the duel block), first match wins:
  1. with the gauntlet block and no target: eat when health is low, drink when mana is low,
  2. with the companion block and ally heals: heal a living, hurt owner (cancel a form first if needed),
  3. with the party block: heal the first hurt, living teammate a heal can reach,
  4. start auto-attack,
  5. move to a living target that is far away while not already moving,
  6. otherwise `greedy`.

They are the reference numbers a trained policy has to beat (evaluation baseline) and a mechanics smoke test
(`forge run <stage> fight`).

## 4.8 The critic state

The centralised critic sees a class-agnostic global state of the env. `StateDim = 21 + 4 x 23 + 4 x 14 = 169`.

| Part | Features |
|---|---|
| Global (21) | Episode time fraction; pull active; pulls cleared / 10; time to next pull / 20 s; elite pull; linked pull; owner present, alive, health, mana, x, y (relative to the spawn point, / 40), in combat; arena one-hot (8) |
| Per seat (4 x 23) | Present, alive, health, mana, other power, level / 80, role one-hot (3), class one-hot (10), in combat, casting, x, y |
| Per enemy slot (4 x 14) | Present, alive, health, x, y, casting, elite, level difference / 5, in combat, victim is the owner, victim is seat s (4) |

The episode time fraction appears only in the critic state, never in an observation, because live play has no
episodes. In self-play, each seat's opponent is the other seat and already appears in the seat part.

## 4.9 Tuning

Every value that shapes the curriculum is a config key `<TuningPrefix><Group>.<Name>`: `AnimusForge.Curriculum.*` in
the forge and `Animus.Curriculum.*` in mod-animus. `CurriculumTuning::Visit` lists them once, for loading and for
writing. Min/max pairs are put in order on load.

| Group | Controls |
|---|---|
| `Characters.*` | High-level threshold and chance |
| `Party.*` | Size weights, classic makeup chance, role chances, teammate reward weights |
| `Duel.*` | One-on-one reward weights and preferred ranges |
| `Casting.*` | Cast time wasted and completed |
| `Pulls.*` | Linked, elite and higher-level chances, pull timing, owner engage timing, recovery fraction, pull reward weights |
| `Owner.*` | Level spread, role chances, owner reward weights, follow distances |
| `Resurrection.*` | Grace period, revive reward |
| `Opponent.*` | Level spread, engage time, role chances |
| `Ambush.*` | Arrival window, engage time, kill reward |
| `ScriptedPlayers.*` | Spell and heal intervals, wandering, regeneration, heal thresholds, ranges |
| `Arena.<stage>.<arena>.Weight` | Arena weights (read by `StageScenario`, not `Visit`) |

The effective values are written into `stage.json` under `tuning` and copied into each run directory. To watch a stage
in mod-animus exactly as a model trained on it, copy that run's `tuning` into `Animus.Curriculum.*`.
`animus-forge/conf/mod_animus_forge.conf.dist` documents every key.

## 4.10 Episode info

Every stage reports these **core columns** per seat:

- `damage`, `dps`, `white_damage`, `special_damage`
- `level`, `race`, `spec`, `class`, `role`, `unspent_talent_points`, `equipped_items`
- `spell_casts`, `trinket_uses`
- `present` (0 for an empty party seat; ignore that row), `arena` (index into `stage.json` arenas), `opponent_seat`
- `killed`, `died`, `time_to_kill`, `damage_taken`, `health_left`, `stealth_openers`, `pet_summoned`, `opponent` (creature
  entry)
- `casts_completed`, `casts_cancelled`, `cast_seconds_wasted`, `cancelled_stopped`, `cancelled_moved`,
  `cancelled_target`, `cancelled_other`
- `consumables_used`, `self_resurrections`

Encounters then add their own columns:

- pulls: `kills`, `interrupts`, `pack_size`, `linked`, `pulls_cleared`, `food_used`, `drink_used`, `sustain_casts`,
  `deaths`, `wipes`
- owner: `owner_class`, `owner_role`, `owner_died`, `owner_deaths`, `owner_damage_taken`, `owner_healing`,
  `threat_on_bot`, `threat_on_owner`, `revives`
- party: `seat`, `teammates_died`, `teammate_damage_taken`, `teammate_healing`, `threat_on_teammates`
- opponent: `won`, `opponent_class`, `opponent_role`
- ambush: `ambushers`, `ambushers_killed`

Then come the `reward_<term>` columns. Columns of encounters an episode's arena doesn't use read 0. The exact list for a
stage is `episode_info` in its `stage.json`.

## 4.11 Stage by stage

### Stage 1: `stage1_duel`

A new character against a real creature (4.5). Nothing seeds it, so its networks start from scratch. Hunters are
offered four beasts each episode through `call_beast` actions, because Call Pet needs a pet saved in the database. The
observation shows each beast's family and pet type, so the policy can learn its preference. Warlock demons, Raise
Dead, Water Elemental and Feral Spirit are ordinary spell actions with their reagents in the bags. The bot gains no XP.

Learner (`configs/stage1_duel.yaml`, the root every other config extends): hidden `[512, 512]` (every stage keeps
these sizes, or the trunk can't be copied), gamma 0.997, lambda 0.95, clip 0.2, entropy 0.01, learning rates 3e-4,
4 epochs, 8 minibatches, rollout 128. Budget 300M env steps. Evaluation every 10M steps on 128 seeds against `fight`.
Convergence patience 5, window 4, z 2, at least 2% and 0.01 improvement, not before 30M steps. Target: 10% over
baseline overall and at least baseline for every class/role (16+ episodes), confirmed on 512 held-out episodes. Up to
2 restarts with 3x entropy decaying over 10M steps.

### Stage 2: `stage2_pack`

Adds the pack block: target slots, tactical spells, and the enemy-slot observation. Linked packs mean pulling one
enemy pulls all of them. The interrupt reward teaches casting interrupts at the right moment. Config: inherits
stage 1.

### Stage 3: `stage3_gauntlet`

Adds the gauntlet block: sustained combat, recovery between pulls with food, drink and sustain spells. Between pulls
there is no target, so target features are 0 and only self-cast actions are allowed. Needs long episodes (several
minutes of `AnimusForge.EpisodeSeconds`). Config: gamma 0.999, budget 400M, at least 40M steps.

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

The PvP branch. It extends the duel and keeps only core and duel, adding pvp. The pack, gauntlet, companion and party
blocks aren't in its layouts, so the PvP line can train right after the duel. Scored against `fight`.

### Stage 7: `stage7_arena`

Self-play. Two learned seats of random classes and roles at one level, both played by the policy, so every fight is
training data for both sides. A policy's score against itself doesn't track progress, so evaluation uses
`eval.opponent_baseline`: the `fight` baseline plays seat 2, the score is seat 1 against it, and the baseline score is
`fight` against `fight` on the same seeds. Budget 200M.

### Stage 8: `stage8_crossroads`

Both branches join. It extends `stage5_party` (trunk and PvE blocks) and merges `stage7_arena` (the pvp block),
`stage6_pvp`, `stage4_companion`, `stage3_gauntlet` and `stage1_duel`, and adds `context` and `hostiles`. Its layouts
contain all nine blocks.

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

### Pilot: `mix_duel_pvp`

Stage 6's blocks, seeded from `stage6_pvp`, merging `stage1_duel`, with `duel` and `pvp_scripted` arenas half and half.
Each is taught by the parent that trained it, and each must beat baseline by 10% on its own. It checks that one policy
can train PvE and PvP side by side before stage 8 mixes larger arenas.
