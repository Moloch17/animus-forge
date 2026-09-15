# mod-animus-forge

Training scenarios for the Animus Forge sim host (the `forge` branch of AzerothCore). The core
stays a lightweight, faster-than-real-time instance simulator. This module owns everything about
a training run:

- in-process bots with no socket and no character row
- arenas, targets, episodes and resets
- observation, action and reward encoding
- a lock-step bridge to a Python MAPPO learner

Its main work is the class/role curriculum: eight stages that train one policy for every class and role,
from a one-on-one duel up to parties and self-play arenas.

## How it fits together

```
worldserver (forge)                                   python -m animus.train
 └─ World::ForgeUpdate ── every tick ─┐
     sScriptMgr->OnWorldUpdate        │  EnvPool: N envs, one instance map each
                                      │   ├─ Scenario::Reward / Observe / auto-reset
                                      │   └─ every DecisionTicks:
                                      │        STEP  ── unix socket ──►  MAPPO actor picks actions
                                      │        ACT   ◄───────────────   (lock-step)
                                      └─ Scenario::ApplyActions
```

- **Envs are instance maps.** Each env's bots get their own `InstanceMap`, so envs update in
  parallel on `MapUpdate.Threads`. Envs are created once at startup and reset in place; maps and
  players are never recreated, so nothing touches the database while the run is going.
- **Bots are sessionless players.** See `src/Bot/ForgeBotFactory.cpp`. They are deliberately kept out
  of `WorldSessionMgr`: a socketless session registered there is deleted, and its player saved,
  on the next update.
- **Damage is measured in `UnitScript::DealDamage`**, before the victim's AI runs.
  `npc_training_dummy` zeroes damage in `DamageTaken`, so `OnDamage` would only ever see 0.
- **The learner drives time.** In `remote` mode the world thread waits on the learner each
  decision, and while no learner is connected. Scripted policies run without Python.
- **The console drives the sim.** The sim starts idle; `forge start`, `forge pause`, `forge cancel` and the other
  [console commands](#operating-the-sim-console) decide what runs. While the world thread waits on the learner it
  keeps answering them.

## Scenarios

List the scenarios to train in `AnimusForge.Queue` (see
[Training every model](#training-every-model-animusforgequeue)). The learner auto-starts with
`configs/<scenario>.yaml` and trains in `<OutputDir>/runs/<scenario>/`.

### `warrior_dummy_20`

A level 20 human Arms warrior on a level 20 training dummy.

- **Kit:** every spell the warrior trainers teach by level 20 (from `trainer_spell`, ranks resolved
  from `Spell.dbc`). Weapon and defense skills are maxed for the level.
- **Gear:** a strength/stamina mail kit of level 16-20 greens and dungeon blues, with Haunting Blade
  (two-handed sword). No helm or trinkets. Listed in `GEAR` in `WarriorDummy20Scenario.cpp`.
- **Talents:** reset every episode, then one build is drawn uniformly from all **1371** valid ways
  to spend the 11 points in the Arms tree. Validity follows `Player::LearnTalent`: a point in row r
  needs 5r points spent, and Deep Wounds needs 2/2 Impale. The build's ranks (10 talents) are part
  of the observation, so one policy learns to play every build.
- **Talent abilities:** active talent spells in reachable rows are appended to the action space
  automatically and masked when the build lacks them. At level 20 every reachable Arms talent is
  passive; the first active one, Sweeping Strikes, needs 20 points (level 29).
- **Actions (11):** no-op, Heroic Strike, Cleave, cancel queued, Rend, Thunder Clap, Battle Shout,
  Bloodrage, Overpower, Hamstring, Mocking Blow. Charge, Victory Rush, Revenge and the shield or
  defensive abilities are learned but left out, because they can't be used or deal no damage
  against a dummy.
- **Masks:** each action is checked with the core's own `Spell::CheckCast`, run without casting.
  That covers cooldowns, the GCD, rage, stance, range and facing, and Overpower's dodge window.
- **Observation (29):**
  - rage, swing timer and weapon speed
  - which on-next-swing ability is queued
  - GCD and each cooldown
  - the Overpower window
  - Rend, Thunder Clap and Hamstring remaining on the target
  - Battle Shout and Bloodrage remaining on the bot
  - last-step damage and rage change
  - the 10 talent ranks
- **Episode info:** damage and DPS, hit counts, the talent build index, and casts per ability.
- **Baselines:** `white_only`, `hs_at_threshold`, and `rotation`, a conventional levelling priority.

### Class/role curriculum: one policy for every class and role

Seven stage scenarios -- `stage1_duel`, `stage2_pack`, `stage3_gauntlet`, `stage4_companion`, `stage5_party`,
`stage6_pvp`, `stage7_arena` -- each train **one policy for every class/role** of `AnimusForge.ClassRoles` (all 18
by default). Each stage is its own scenario, so every
earlier stage stays repeatable.

- **Seats:** each learned agent of an env is a seat. Every episode each seat becomes a new character of a
  class/role drawn from the run's list (the party draws a tank, a healer and two damage dealers). Most stages
  have one seat per env, the party four, the arena two.
- **Layouts:** a class/role's observation features and actions are fixed (its spells, talents, stable, heals),
  so each class/role is a layout of the policy. The wire protocol pads every agent's row to the largest layout
  and tags it with its layout id. The learner (`animus/mappo/networks.py`) gives each layout an input adapter
  and an action head around one shared trunk: what is learned about moving, threat, healing or interrupts is
  shared by all classes, and every class keeps its exact actions.
- **Critic:** the value of each agent is estimated from a class-agnostic global state of the env (every seat's
  health, power, class, role, position and casting; every enemy's health, position, casting, eliteness and
  victim; the owner; pull timing) together with the agent's own observation.
- **Models:** each layout exports as its own model (`<class>_<role><stage suffix>.amdl`, e.g.
  `warrior_dps_duel.amdl`): adapter, trunk and head together are one plain MLP.
- **`AnimusForge.ClassRoles`:** a comma-separated subset (e.g. `"warrior_dps, priest_heal"`) trains only those.
  Changing it changes the layouts, so models of the earlier list do not fit the new one.

#### How a stage is built: blocks, encounters, tuning

A stage is one entry in `src/Scenario/ClassRole/Stages/Stages.cpp` (`StageDefinition`):

- **Blocks** (`Blocks/`): the groups of observation features and actions its layouts have, in order -- `core` (the
  character, its spells, trinkets and talents), `duel` (movement, auto-attack, pets, casting, forms), `pack` (enemy
  slots, target selection, tactical spells), `gauntlet` (pull timing, food, drink, sustain spells), `companion`
  (the owner), `party` (three teammates), `pvp` (the enemy player). A block is stateless: it sizes its slice of a
  layout, writes its features and mask, and applies its actions. `Layout` places each block after the previous
  one, and the manifest lists every block with its spans.
- **What it extends:** the stage it builds on and seeds from. It keeps the base's blocks it needs, drops the rest and
  adds its own; several stages can extend the same base, so the curriculum is a tree:

  ```
  duel ─┬─ pack ─ gauntlet ─ companion ─ party      (PvE)
        └─ pvp ─ arena                               (PvP: core, duel, pvp)
  ```

  The sim checks each definition at startup (the base exists and comes earlier, no block twice, every part has the
  blocks it needs) and leaves out a stage that breaks a rule.
- **Encounters** (`Encounters/`): what its envs contain besides the seats -- a creature, pulls
  (one pack or the gauntlet's schedule), a scripted owner, a party group, an enemy player (scripted or the other
  seat). Each encounter builds and updates its part of the world, keeps its own episode state, and adds its reward
  terms, episode info columns and critic state. `ClassRoleScenario` builds the seats and calls every encounter's
  hooks in a fixed order.
- **Rewards** are added term by term (`Rewards/RewardLedger.h`); every term's episode sum is reported as the
  episode info column `reward_<term>` (`reward_damage_dealt`, `reward_clear`, `reward_owner_death`, ...), so
  TensorBoard shows what each stage actually pays for.
- **Tuning:** every reward weight, chance, level spread and scripted-player timing is a config key,
  `AnimusForge.ClassRole.<Group>.<Name>` (the CLASS/ROLE TUNING section of `mod_animus_forge.conf.dist`). The sim
  writes the effective values into `<OutputDir>/layouts/<stage>/stage.json` beside the layout manifests, and the
  learner copies it into the run directory.

#### Characters and the core block

Every stage plays every class and role (tank and healer roles fight with their own spec's damage play in role
gear until the companion and party stages give them their jobs):

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

The table lives in `src/Scenario/ClassRole/Character/ClassRoleProfile.cpp`, with each spec's stat profile,
range and weapon layouts. Every episode builds a new character (the env's bot is replaced):

- **Race and level:** a random race the class allows (`playercreateinfo`), random gender; half the characters
  are level 61-80 and the rest any level 1-80 (55-80 for death knights), since most players are high level
  (`Characters.HighLevelFirst`, `Characters.HighLevelChance`).
- **Talents and glyphs:** one of the role's specs with its standard 3.3.5 build (`Character/SpecBuilds.cpp`, 31
  specs: the talents players take, in the order they take them; generated from `tools/spec_builds/builds.py`, which
  `validate.py` checks). Each point goes to the first talent in that order that still wants ranks and can take one
  (row and prerequisite rules follow `Player::LearnTalent`), so a low-level character has the talents players pick
  first; every build spends exactly 71 points at 80. The glyph slots the level has opened get the spec's standard
  major and minor glyphs the level can use.
- **Kit:** every spell of the class trainers up to the level (`trainer`/`trainer_spell`, learn-spells
  resolved), talent-gated ranks when the talent was taken, and class-quest spells trainers do not
  teach (stances, Bear Form, warlock demons, Raise Dead). Weapon and armor skills are the ones the
  race and class may have, maxed for the level. Reagents: totems, Ankhs (Reincarnation), soul shards,
  corpse dust, flash powder; hunters get ammo for their ranged weapon, in a quiver or ammo pouch.
- **Gear:** random level-appropriate items for every slot including both trinkets, drawn from
  every obtainable item (loot, vendors, quest rewards, crafted) the class can use and whose stats
  suit the spec (strength melee, agility melee, ranged, caster, healer or tank). Items with random
  stats roll only suffixes that suit it. A slot takes an item the bot may wear whose item level is in
  the band players of its level wear (`ITEM_LEVEL_ANCHORS` in `GearBuilder.cpp`: a few item levels
  above the level while levelling, Outland gear from 58, Northrend gear from 70, heroic-dungeon gear
  180-213 at 80), reaching 10, 25, then any number of item levels below the band -- never above it --
  and falling back to lighter armor, then stat-less items, when nothing fits. Within the band, dungeon
  drops are picked 4 times and quest rewards 3 times as often as other items, and items near the band's
  middle more often than its edges. Epics only at levels 70 and 80 (heroic and badge gear); resilience (PvP)
  gear only in the PvP stages. Armor is plate/mail/leather/cloth by class and level; weapons follow the
  spec's layouts (two-hander, dual wield when the bot can -- daggers for assassination and subtlety -- a
  one-hander before that, one-hander with shield or off-hand item, staff, bow/gun + stat stick, wand);
  paladins, shamans, druids and death knights carry a relic.
- **Enchants and gems** (`Character/GearEnhancements.cpp`): every item at levels 70 and 80, half of them while
  levelling, gets the best enchant that suits the spec and that a player of the level could buy (enchanting
  recipes and items such as leg armor and arcanums, by the enchanting skill they take: 5 per level to 300 at
  60, 450 at 80; weapon procs by name: Berserking, Mongoose, Black Magic, ...), and every socket a gem of its
  color that suits the spec (the socket bonus when all match; meta gems last; epic gems only at 80). Death
  knights runeforge (Fallen Crusader, Razorice in the off hand, Stoneskin Gargoyle for tanks), rogues carry
  Instant and Deadly Poison for their level. No profession-only enchants or gems.
- **Supplies:** 5 of the best healing potions, 5 mana potions (mana users) and 5 bandages (with the First Aid
  skill of the level); warlocks carry a healthstone (and hand one to every seat of their party) and a
  soulstone. At levels 70 and 80 the character has drunk a flask that suits its spec; while levelling, half of
  the time, an elixir.
- **Core actions:** fixed per class, built at startup: no-op, cancel queued swing, one action per rank
  chain of every combat spell a level 80 character of any of the class's races knows (trainer,
  starting and racial spells, active talents of all three trees), casting the highest rank the bot
  knows, and one action per trinket slot. A spell counts as combat if it deals damage, applies a
  damage-relevant buff/debuff/DoT, generates resources, shapeshifts, or summons (pets, totems);
  movement, travel, crafting, pure heals and utility are left out. Every action is masked by the
  core's `Spell::CheckCast` each decision (race, level, talent, cooldown, GCD, power, stance, range,
  reagents). As on a client, no spell or trinket can be started while a cast is in its cast time (the
  core only checks that for client casts, so a bot's new cast would otherwise silently replace it).
- **Core observation:** level, race and spec one-hots, health and every power type, runes, combo points,
  shapeshift form, GCD, casting, swing timers, target health and distance, attack power, spell power,
  crit, haste, hit, expertise and armor penetration; per action: known, cooldown, its aura on the
  target and on the bot, stacks; every talent's rank; points per tree.
- **Damage scale:** stages weigh damage by a level scale (`15 * e^(0.068 * level)`), so early and late levels
  weigh alike. Pet, guardian and totem damage counts for the owner.
- **Core episode info:** damage, DPS, white/special damage, level, race, spec, unspent talent points,
  equipped items, spell casts, trinket uses.

The core block is not a scenario of its own. Learner settings come from `configs/<scenario>.yaml`.
`configs/stage1_duel.yaml` is stage 1's and the root every later stage's config extends (directly or through an
earlier stage), listing only what it changes.

#### Stage 1 (`stage1_duel`): the duel

Characters are built as above (race, level, spec, talents, kit, gear), against a real opponent. Nothing seeds this
stage: its networks start from scratch.

- **Opponent:** a random creature whose natural levels cover the bot's (normal rank, attackable,
  default AI with no script, no NPC services, not civilian, guard or trigger, spawned somewhere in
  the world), summoned at the bot's level 40-50 yd away at a random bearing (a spot in line of sight
  on level ground), facing a random direction, hostile and aggressive. It is out of aggro range, so
  the bot has to close in, and it fights back. The bot gains no XP, so its level never changes.
- **Pets:** nothing is pre-summoned. Warlock demons, Raise Dead, Water Elemental, Feral Spirit and
  the like are ordinary spell actions (with their reagents in the bags). Hunters, whose Call Pet
  needs a pet saved in the database, are offered 4 tameable beasts of different random families
  each episode through 4 `call_beast` actions; the observation shows each beast's family and pet
  type (ferocity, tenacity, cunning), so the policy can find the one it prefers.
- **Actions:** the core actions, then: move to the opponent, move behind it, move to casting range
  (25 yd), back off 10 yd, stop, start auto-attack, send pets to attack, stop casting (the current cast
  or channel, as the client's cancel-cast), cancel form (a shapeshift the client could cancel: druid
  forms, Shadowform, Ghost Wolf, Stealth; not stances or presences), drink a healing or mana potion, use a
  healthstone, bandage itself, soulstone itself (warlocks), resurrect itself when dead (Soulstone or
  Reincarnation; not in the PvP stages), and (hunters) the 4 `call_beast` actions. Item actions are masked by
  the core's cast checks (the shared potion cooldown, Recently Bandaged, combat). The bot turns to face the
  opponent whenever it is not running. Movement is masked while
  casting, and cast-time or channeled spells while running. Stop casting and cancel form need no
  target, so they stay available between pulls in later stages.
- **Observation:** the core observation, then: distance, bearing to the opponent, whether the bot is behind it
  and whether it faces the bot, the opponent's combat, target and casting state, the bot's movement,
  combat, stealth and auto-attack state, damage taken last step, pet out/health/attacking, elapsed
  episode time, the current cast's progress and time left (casts and channels), whether the bot is in
  a form it can cancel, potions, healthstones and bandages carried, the potion and healthstone cooldowns,
  Recently Bandaged, whether it will be able to resurrect itself, and (hunters) the stable. A dead bot sees
  only that it is dead and whether it can resurrect itself -- the one action it has.
- **Reward** (defaults; every weight is an `AnimusForge.ClassRole.Duel.*` key): per decision, damage dealt as a
  fraction of the opponent's health (x2) minus damage taken as a fraction of the bot's (x1), potential-based
  shaping toward the spec's range (melee 3.5 yd or 25 yd), +0.5 for a stealth-only opener from stealth, and a
  small time cost. Casting (`Casting.*`): every cast-time spell that does not finish (stopped, interrupted,
  pushed into death) costs 0.03 per second of cast time already spent, and every one that finishes while the bot
  is in combat earns 0.03 per second of its cast time. Nothing forces a cast to finish; cutting one short stays the
  policy's call when something else is worth more. Channels are paid by their ticks. Later stages keep this term.
  On the kill: +2, plus up to +3 for the time left in the episode, plus up to +2 for the share of the bot's health
  it did not lose. Death: -3 (every death, including one after resurrecting itself). The episode ends on the kill
  or the bot's death -- unless it can resurrect itself, when it has 20 s to do it (`Resurrection.GraceMs`).
- **Episode info:** the core columns, then killed, died, time to kill, damage taken, health left,
  stealth openers, whether a pet was out, the opponent's entry, casts completed, casts cancelled and
  seconds of cast time wasted, why casts were cancelled (stopped by the bot, while moving, target died or gone,
  anything else), consumables used and self-resurrections, and the reward terms.

**Bootstrapping:** every stage after the first has `init_from: auto`: the learner reads the stage's `stage.json`,
whose `seed_chain` lists the stages it extends, closest first, and seeds from the first of those that has been
trained (`<runs_dir>/<stage>/best.pt`, or its `latest.pt`). It seeds its networks layout by layout and block by
block (`animus/bootstrap.py`; class/roles and blocks are matched by name, their positions come from the
`layouts` of both stages' `stage.json`, which every checkpoint carries): each kept block's features and actions move
to where the block sits now, new blocks' inputs start at zero and their actions near zero, and dropped blocks are
left behind; the trunk is copied; the critic's state encoder and value head start fresh because the global state and
reward differ. A checkpoint without block positions (from before this) is seeded as a prefix. Queue every stage after
the stage it extends (the sim warns otherwise); the two branches can go in either order, or on two machines.

#### Stage 2 (`stage2_pack`): packs

Stage 2. The duel's characters against a pack instead of a single opponent:

- **Pack:** 2-4 creatures at the bot's level, clustered 40-50 yd away, each facing its own way. The
  pool adds creatures whose SmartAI only casts spells or talks (casters and ability users, ~3500) to
  the duel's default-AI creatures. 70% of packs are linked: once one member is in combat, the rest
  attack the bot.
- **Actions:** the duel's, then one "target slot" action per enemy (4), then **tactical spells** the
  earlier stages leave out: interrupts, stuns, silences, fears, roots, polymorphs, knockbacks,
  taunts and offensive dispels. Every spell, movement and pet action aims at the current target; when
  it dies the nearest living enemy becomes the target.
- **Observation:** the duel's (about the current target), then living / in-combat enemy counts, 4
  enemy slots (present, alive, health, distance, bearing, behind, attacking the bot or its pet,
  casting, in combat, crowd-controlled, current target, elite, level difference), and each tactical
  spell's known/cooldown.
- **Reward:** damage as a fraction of the pack's total health (x2), damage taken as a fraction of
  the bot's (x1), approach shaping toward the nearest enemy, +0.5 per kill, +0.3 per interrupt (an
  enemy whose cast is cut short, not by itself or its death, after the bot's interrupt, stun, silence, fear or
  polymorph at it; a cast that finishes on its own does not count), +0.5 for a stealth
  opener. Clearing the pack: +2, up to +3 for the time left, up to +2 for the health kept. Death -3.
  The episode ends when the pack is cleared or the bot dies.
- **Episode info:** the duel's (killed = cleared), then kills, interrupts, pack size, linked.

#### Stage 3 (`stage3_gauntlet`): the gauntlet

Stage 3: sustained combat.

- **Pulls:** pull after pull until the bot dies or the episode ends: 1-4 creatures from the pack
  pool, or (15%) a single elite, or (25%) a pack 1-3 levels above the bot. After a clear the field is
  emptied and the next pull spawns 8-20 s later, out of aggro range.
- **Recovery:** the pack stage's actions, then eat, drink, and **sustain spells**: heals,
  heal-over-time, absorbs and friendly dispels (pet heals included). Each episode the bot carries 5
  of the best vendor food for its level, and 5 drinks if it uses mana; eating and drinking need no
  combat and no movement.
- **Observation:** the pack stage's, then pulls cleared, whether a pull is active, time to the next
  pull, time into the current pull, elite/higher-level pull, eating, drinking, food and drink left,
  and each sustain spell's known/cooldown. Between pulls there is no target: target features are 0
  and only self-cast actions are allowed.
- **Reward:** the pack stage's per-step terms (damage taken weighs x1.5). Each cleared pull: +2, up
  to +2 for clearing it within a minute, up to +2 for the health kept during that pull. Death -5 and
  ends the episode (after the 20 s a bot that can resurrect itself has to do it).
- **Episode info:** the pack stage's, then pulls cleared, food used, drinks used, sustain casts, deaths.

Give the gauntlet long episodes (`AnimusForge.EpisodeSeconds` of several minutes).

#### Stage 4 (`stage4_companion`): the companion

Stage 4: the gauntlet fought beside an owner, as a companion fights beside a player.

- **Owner:** a scripted player bot within 2 levels of the companion, of a random role (tank 25%, healer 25%,
  damage dealer 50%) and a class that can fill it, dressed like the companion: one of the role's specs with a
  standard build and glyphs, its trainer spells and level-appropriate gear. It gets the companion's faction so either
  faction's races can be paired. Between pulls it wanders near the spawn point and recovers health and mana; each
  pull spawns around it. A tank owner starts every pull and taunts enemies off others; a healer owner heals the most
  hurt party member; a damage dealer walks in after 1.5-5 s (and starts the pull itself 30% of the time) and
  fights. It casts one of its own spells every 2-4 s. Linked packs join in on whoever their engaged member fights.
  Everything here is `AnimusForge.ClassRole.Owner.*`, `Pulls.*` and `ScriptedPlayers.*` tuning.
- **Actions:** the gauntlet's, then follow the owner, assist (target the owner's target), guard (target an
  enemy attacking the owner), one "cast on the owner" action per single-target heal, and one per **revive**:
  each resurrection spell (Resurrection, Redemption, Ancestral Spirit, Revive, Rebirth) on the dead owner, and
  a warlock's soulstone on the living owner.
- **Observation:** the gauntlet's, then the owner's presence, health, mana, distance, bearing, combat,
  movement, level difference and class, how many enemies attack it, which enemy slot it attacks, which
  enemies attack it, and each owner heal's and revive's known/cooldown.
- **Reward:** the gauntlet's, with kills and clears counting double, plus, by role (defaults of
  `AnimusForge.ClassRole.Owner.*`):
  - everyone: the owner's damage taken (fraction of its health; x1 for damage dealers, x2 for tanks and
    healers; a quarter of that when the owner is the tank), -0.01 per decision in combat while the owner is not,
    +0.0005 per decision out of combat within 12 yd and -0.002 beyond 25 yd, -6 each time the owner dies;
  - tanks: +0.002 per enemy attacking the tank, -0.02 per enemy attacking the owner, per decision (and
    half of the gauntlet's damage-taken penalty back);
  - healers: effective healing on the owner (x2, fraction of its health; the core's heal hook reports the
    health actually gained, so overhealing earns nothing);
  - damage dealers and healers: -0.004 per enemy attacking them, per decision;
  - everyone: +1.5 each time a dead ally it resurrected stands up (`Resurrection.ReviveAlly`; resurrect requests
    are accepted at once, as a player would).
- **Deaths do not end the episode, and resurrecting is learned.** After each pull the dead wait up to 20 s
  (`Resurrection.GraceMs`) -- and the next pull waits with them -- for a resurrection they can get: their own
  Soulstone or Reincarnation, or a living seat's resurrection spell. Only then does whoever is still dead -- the
  companion or the owner -- stand up with half health and mana, so the fallback keeps the episode going without
  doing the party's job. Every death is paid for once, again after standing up. A pull that kills everyone is
  cleared away (a wipe) and the next comes after the usual break. The episode runs its full length, so letting the
  owner die is never a way out of the penalties that follow.
- **Episode info:** the gauntlet's, then wipes, the owner's class and role, whether and how often it died, its
  damage taken, the companion's healing on it, enemy-decisions spent on the companion and on the owner, and the
  allies the seat resurrected.

#### Stage 5 (`stage5_party`): the party

Up to four learned seats -- 1-4 each episode, as a player brings 1-4 companions (`Party.SizeWeight1-4`); half the
time the classic tank, healer and damage dealers, otherwise roles drawn one by one -- of random classes that can fill
their roles, all at one level, and the companion stage's scripted owner as the fifth player, against dungeon-like
pulls. Every seat plays with the same policy and sees the other three; an empty seat has no character and only the
no-op.

- **Pulls:** 2-4 creatures, each elite half the time, up to 2 levels above the party, pull after pull, spawning
  around the owner. The owner waits 4-7 s so the tank can pull, then attacks the tank's target.
- **A real group:** every episode the owner (as leader) and the four seats form a core `Group`, so party buffs,
  auras, party-wide heals and every "party member" check work as in play. It is a sim group
  (`Group::SetSimGroup`): it lives only in memory -- no group or member rows, no character cache entries, and
  joining, leaving or disbanding never touches instance binds or homebind timers -- so rebuilding it every
  episode writes nothing to the database. It is disbanded before its members are replaced.
- **Actions:** the companion stage's, then follow the tank, then per teammate assist, guard, one "cast on
  it" action per single-target heal, and one per revive.
- **Observation:** the companion stage's, then living party size, the most hurt ally's health, whether a
  living tank and healer are present, and per teammate presence, health, mana, distance, bearing, combat, role,
  class, attackers, target slot and which enemies attack it.
- **Reward:** per seat, the companion stage's (its own damage, threat and survival, the owner's), plus per
  teammate: its damage taken (not for a tank teammate; x0.5 for a damage dealer, x1 otherwise), effective
  healing on it for healers (x2), -0.02 per enemy on a non-tank teammate per decision for tanks, and -3 each time
  it dies. Kills and clears are shared by the party. A tank is not charged for fighting before the owner joins.
  As in the companion stage, the dead wait for a resurrection after the pull before they stand up, and the
  episode runs its full length.
- **Episode info:** per seat, the companion stage's, then the seat, teammates died, teammate damage taken, its
  healing on teammates, and enemy-decisions spent on non-tank teammates.

#### Stages 6 and 7 (`stage6_pvp`, `stage7_arena`): PvP

One-on-one against a player: the PvP branch of the curriculum. `stage6_pvp` extends the duel (and seeds from
it), keeping its core and duel blocks and adding the pvp block; the pack, gauntlet, companion and party blocks are not
in its layouts at all, so the PvP line trains right after the duel, without the PvE stages. Both players get opposing
player factions and the PvP flag, which players need to attack each other.

- **`stage6_pvp` (stage 6):** a scripted enemy player at the bot's level (within 1), of a random class and
  role (damage 60%, tank 20%, healer 20%) with that role's spec, talents, kit and gear, spawned 40-50 yd away
  facing a random way. It closes in after up to 3 s: melee specs fight in melee, ranged specs hold 10-30 yd and
  cast, healers heal themselves below 60%.
- **`stage7_arena` (stage 7), self-play:** two learned seats of random classes and roles at one level in the
  same env, the second spawned 40-50 yd from the first. Both are played by the same policy, so every fight is
  training data for both sides, across class matchups.
- **Observation:** the duel's, then the opponent's class, role, level difference, mana, rage/energy/runic
  power, whether it is crowd-controlled, stealthed, has a pet out or is casting a heal, whether the bot is
  stunned/feared, rooted or silenced, and whether the opponent is a learned agent. Actions: the duel's.
- **Reward:** the duel's (damage dealt and taken, closing in, stealth openers, casts, a fast kill with health
  kept, death). The episode ends when either player dies.
- **Episode info:** the core and duel columns, then won, opponent class and opponent role (the pulls, owner
  and party columns are left out: nothing fills them in PvP).

`configs/stage6_pvp.yaml` scores against the `fight` baseline. `configs/stage7_arena.yaml` has no
baseline, convergence stop or target: against itself a policy's score does not track progress -- judge an arena
model with `animus.evaluate` on a `stage6_pvp` sim.

### Training every model: `AnimusForge.Queue`

`AnimusForge.Queue` lists the scenarios `forge start` trains, one after another, when it is given none. Empty (the
default) is the whole class/role curriculum, first stage to last; to train a single scenario, name it on the console
(`forge start stage1_duel`) or list only that one.

Each scenario runs with its auto-started learner until the learner finishes -- its evaluation score
converges and its best networks pass the stage target, or it reaches `total_env_steps` (see
[Evaluation, convergence and stage targets](#evaluation-convergence-and-stage-targets)) -- and exits cleanly;
the sim then tears the scenario down and starts the next one. A stage that stays below its target after its
restarts exits with code 3, which halts the plan there (`finished.json` and `stage.jsonl` say which gates failed).
Every run trains in `<OutputDir>/runs/<scenario>/`; export its models when you want them (`forge export`, see
[Export](#export)). When the last scenario finishes the sim idles. A learner that crashes holds the plan at that
scenario: `forge resume` restarts it from its last checkpoint, `forge cancel` stops.

`forge start` without names continues where training stopped: scenarios whose run already finished and moved on
(`finished.json` with `"advanced": true`) are left out (`AnimusForge.Queue.SkipFinished`), and the first other one --
unfinished, or halted below its target -- trains from scratch. To retrain a finished stage, move its run directory
away, name it (`forge start stage3_gauntlet`), or set `SkipFinished = 0`.

## Enabling

The module is picked up automatically from `modules/` (it has a `src/` directory). Rebuild the
worldserver.

This module is self-contained: it builds against the forge core on its own and shares no code with any other
module. Nothing else belongs in a forge build -- in particular mod-animus, which plays exported models on a stock
AzerothCore, is never built into the forge core. If another module's directory sits in `modules/`, disable it in
the forge build:

```
cmake . -DMODULE_MOD-ANIMUS=disabled
```

Settings live in `mod_animus_forge.conf`; `conf/mod_animus_forge.conf.dist` documents every key.

The build installs only the `.dist` file, and AzerothCore never reads a `.dist` directly. `acore.sh compiler build`
(which the Docker setup uses) copies it to `mod_animus_forge.conf` on install when that file does not exist yet;
after a plain `cmake --install`, copy it once, next to the installed template:

```
cp env/dist/etc/modules/mod_animus_forge.conf.dist env/dist/etc/modules/mod_animus_forge.conf
```

Until then every `AnimusForge.*` key logs "Missing property" and falls back to its default.

| Key | Default | Purpose |
|---|---|---|
| `AnimusForge.Enable` | `1` | `0` turns the module off entirely (no bots, no socket, hooks return immediately) |
| `AnimusForge.OutputDir` | the learner's directory | Where `runs/` and `layouts/` go (Docker: `/azerothcore/var/animus-forge`) |
| `AnimusForge.Queue` | `""` (the whole curriculum) | Scenarios `forge start` trains one after another when given none |
| `AnimusForge.Queue.SkipFinished` | `1` | `forge start` without names leaves out stages that already advanced |
| `AnimusForge.Queue.LocalEpisodes` | `0` | With a local policy, episodes per scenario of `forge start` before moving on |
| `AnimusForge.ClassRoles` | `""` | Class/roles the class/role scenarios play; empty = all 18 |
| `AnimusForge.Envs` | `64` | Parallel envs (one instance map each) |
| `AnimusForge.DecisionTicks` | `2` | World ticks per decision (2 = every 100 ms of game time) |
| `AnimusForge.EpisodeSeconds` | `60` | Game-time episode length |
| `AnimusForge.SpawnPoint.*` | Old Hillsbrad entrance | Dungeon map and position every env's bots start at |
| `AnimusForge.Policy` | `remote` | `remote`, `random`, or a scenario's scripted policy (`greedy`, `fight`, `rotation`, ...) |
| `AnimusForge.ReportEpisodes` | `256` | Mean episode stats (`forge status`, local runs) are taken over N episodes |
| `AnimusForge.Socket` | `/tmp/animus-forge.sock` | Learner socket path |
| `AnimusForge.Learner.AutoStart` | `1` | Start the Python learner automatically (remote policy) |
| `AnimusForge.Learner.WorkDir` / `Python` / `Config` / `LogFile` | derived | Where and how the learner runs (see Training) |
| `AnimusForge.Learner.Args` | `""` | Extra learner arguments for every scenario, e.g. `--set total_env_steps=5000000` |
| `AnimusForge.WarriorDummy20.HsRageThreshold` | `15` | Rage threshold for `warrior_dummy_20`'s `hs_at_threshold` and `rotation` |
| `AnimusForge.ModelDir` | `models/` in the module | Where `forge export` writes models |
| `AnimusForge.Progress.Interval` | `60` | Seconds between progress reports while a scenario runs; `0` = off |
| `AnimusForge.Fast.*` | see the `.dist` | The low-resolution profile of `forge fast` (see [Fast test run](#fast-test-run-forge-fast)) |
| `AnimusForge.ClassRole.*` | see the `.dist` | The curriculum's tuning: reward weights, chances, level spreads, scripted players |

Any key can also be set from the environment, e.g. `AC_ANIMUS_FORGE_QUEUE=stage1_duel`.

For best throughput set `MapUpdate.Threads` to the number of physical cores.

### Docker

The forge core's `docker-compose.yml` runs training in the `ac-worldserver` service. Everything is named
`ac-animus-forge` (compose project, containers, volumes, network, images) and uses its own host ports
(database `13306`, TensorBoard `127.0.0.1:16006`), so it runs next to a stock AzerothCore.

```
./forge.sh                                          # start everything and attach to the worldserver console
./forge.sh attach                                   # attach again later (detach: Ctrl+P Ctrl+Q)
docker compose --profile dev up -d ac-dev-server    # dev container, alongside the running training
```

`docker compose up` works too, but it only streams logs: Compose does not forward the keyboard to a container,
so the console needs `docker compose attach ac-worldserver` (which `forge.sh` runs). Ctrl+C in the console stops
the server; the learner saves a checkpoint first.

- **Training server (`ac-worldserver`):** runs the worldserver built from the bind-mounted source tree, because
  the learner needs this module's `python/` directory. On its first start it builds the worldserver and creates
  `python/.venv` (torch, the learner and TensorBoard) inside the container, then starts TensorBoard and the sim.
  The sim is idle until `forge start`; it starts the learner itself.
- **Output:** runs and layouts go to `var/animus-forge/` in the forge checkout (`AC_ANIMUS_FORGE_OUTPUT_DIR`; set
  `ANIMUS_FORGE_OUTPUT_DIR` to put them elsewhere), not into the module's source tree. Runs trained before this
  change are in `modules/mod-animus-forge/python/runs/`: move them to `var/animus-forge/runs/` to keep seeding and
  skipping from them.
- **torch build:** the first-start venv installs torch from PyPI (NVIDIA CUDA builds). Set
  `ANIMUS_TORCH_INDEX_URL` before the first start for another build, e.g.
  `https://download.pytorch.org/whl/rocm6.4` for AMD GPUs.
- **GPU:** passthrough is machine-specific; `docker-compose.yml` has commented NVIDIA and AMD blocks to copy
  into `docker-compose.override.yml`. Check it from the container:
  `modules/mod-animus-forge/python/.venv/bin/python -c "import torch; print(torch.cuda.is_available())"`.
- **Dev container (`ac-dev-server`, profile `dev`):** the same image and build volumes, no ports, for VS Code
  (`.devcontainer`) or a shell. After changing C++, build there and restart training:
  `docker compose exec ac-dev-server ./acore.sh compiler build`, then `docker compose restart ac-worldserver`
  (the restarted sim is idle: `forge start` carries on with the stages that have not advanced yet, `forge resume
  <scenario>` continues a run from its latest checkpoint). Don't start a second training worldserver in it: both
  would train into the same `runs/`.

## Operating the sim (console)

The worldserver's console is the control panel: attach to it (`./forge.sh attach`, or run the worldserver in a
terminal) and type commands without a leading dot. The console only starts when stdin is a terminal, so a server
started without one keeps running (and can't be controlled until it is restarted with one). The sim starts idle
and prints its settings; nothing trains until you say so.

| Command | What it does |
|---|---|
| `forge help` | List the commands |
| `forge status` | The progress report below, or the idle settings and the last plan's outcome |
| `forge scenarios` | Every scenario with its run: finished (and why: converged, below_target, ...), resumable checkpoint, steps, best score |
| `forge start [scenario ...]` | Train these from scratch, in order. Without names: `AnimusForge.Queue` (every stage when empty), minus the stages that already advanced (`Queue.SkipFinished`). Earlier runs are archived; a stage listed before the stage it extends is warned about |
| `forge fast [scenario ...]` | A quick low-resolution training run of these (default: `AnimusForge.Queue`, finished or not) in the fast output directory, to check training works before a long run (see [Fast test run](#fast-test-run-forge-fast)) |
| `forge resume [scenario ...]` | Unpause; or continue the first scenario from its `latest.pt`, then train the rest. Without names: where the last plan stopped. With a crashed learner: restart it from its checkpoint |
| `forge pause` | Freeze after the current decision: maps, episode clocks and the learner all wait |
| `forge cancel` | Stop the plan; the learner saves `latest.pt` first, so `forge resume` can continue it |
| `forge skip` | End the current scenario (the learner saves) and start the next one |
| `forge run <scenario> <policy> [episodes]` | Run a scripted or random policy without a learner, for N episodes or until cancelled |
| `forge export [scenario] [best\|latest]` | Export `best.pt` (else `latest.pt`) of the scenario (default: the current or last one) to `AnimusForge.ModelDir`, in the background |
| `forge clean archive` | Delete `runs/_archive/` |
| `forge clean scenario <scenario>` | Delete `runs/<scenario>/` (refused while it runs) |
| `forge clean exports` | Delete the exported models and manifests |
| `forge clean fast` | Delete the fast test runs, layouts and models (refused while a fast run runs) |
| `forge clean logs` | Delete the learner and export logs (refused while they are written) |
| `forge clean all` | All of the above, every run included (idle only) |
| `forge progress [seconds\|off]` | Show or change the progress report interval |

Commands that start or stop scenarios take effect at the next decision; the reply says what will happen. Every
removal is listed with its size.

### Progress report

While a scenario runs the console prints a report every `AnimusForge.Progress.Interval` seconds (and `forge status`
prints one at any time). The learner keeps `runs/<scenario>/progress.json` up to date after every update and
evaluation; the sim combines it with what it measures itself:

```
Forge: stage2_pack (2 of 4) | training | update 412 | 3h 12m
  Metric                            Value  Note
  -----------------------  --------------  -------------------------------------------------
  learner                       connected  pid 4242, last answer 0s ago
  sim                       4,210 ticks/s  64 envs x 1 agents, 1,234,567 decisions
  env steps                 27.0M / 60.0M  45.0%
  step rate                 2,700 steps/s  -3.0% vs last
  ETA (step limit)                 3h 23m  ~05:10
  ETA (converged, earliest)        1h 20m  at 40.0M if 1 more eval brings no new best; then the stage target...
  eval score                        184.2  2 evals, best 190.1 at 0, 2/3 without improvement
  best vs baseline                 +67.3%  fight 113.6
  reward/decision                  0.4121  +4.0% vs last
  entropy                            1.03  -8.0% vs last, 92.0% of start
  ...
  Plan:
  #  Scenario             Status                 Env steps  Best score      ETA
  -  -------------------  -----------------  -------------  ----------  -------
  1  stage1_duel          done               41.0M / 60.0M       212.3
  2  stage2_pack          training (resume)  27.0M / 60.0M       190.1   3h 23m
  3  stage3_gauntlet      pending                    40.0M           -  ~4h 06m
  Plan ETA ~15h 44m ~17:31 (~: full step limit at the current rate; converging ends sooner)
```

- **Step rate** is measured between reports on the learner's own clock, so evaluations and updates are included;
  every ETA uses it. The convergence ETA is the earliest the stage can be judged: the evaluations still allowed
  without a new best, at the configured evaluation interval, and not before `min_env_steps` after the last
  restart. After a restart the report shows the restarts used.
- **Warnings** follow the report, one line each, only when they apply: the learner has not answered for 2 minutes,
  the step rate fell over 30% below the run's average, entropy is under 25% of its first value, approx KL is over
  0.05 or the clip fraction over 30%, a metric is NaN or infinite, the best score is still below the baseline after
  2 evaluations, the next evaluation can end the stage by convergence, or the learner exited unexpectedly.
- **A stage below its target:** when the learner exits with code 3 the plan halts at that stage (outcome
  `below target`) instead of moving on; `finished.json` and `stage.jsonl` in its run say which gates failed.
- **Local runs** (`forge run`) report episodes done, the ETA to the episode limit and the episode means.

### Fast test run (`forge fast`)

A real stage trains for hours before its first stage decision. `forge fast` runs the same pipeline at low resolution
in minutes, to check that a change to a scenario, the learner or the configs still trains before starting a long
run:

```
forge fast                       # every queued stage, one after another
forge fast stage1_duel stage2_pack
```

- **Sim (`AnimusForge.Fast.*`):** 16 envs, a decision every 200 ms, 30 s episodes, and four class/roles
  (`warrior_tank, priest_heal, rogue_dps, hunter_dps`: a tank and a healer for the party, energy, rage, mana and a
  pet). Per-decision rewards are scaled to the decision interval as usual.
- **Learner (`configs/fast.yaml`):** merged over each stage's config with `--overlay`. It shrinks the budgets --
  600k env steps, an evaluation of 32 seeded episodes every 100k, `min_env_steps` 200k, small checkpoints -- and
  turns the stage targets off, so the plan moves on to the next stage when a stage converges or reaches its budget.
  Networks, gamma, entropy, patience, the baseline and restarts stay each stage's own, so the run goes through the same
  code as a real one: spec and layouts, rollouts and updates, evaluation with the baseline, convergence, checkpoints,
  `finished.json`, the next stage seeding from this one, and `forge export`. To test the target gates and restarts
  too: `AnimusForge.Fast.Learner.Args = "--set target.min_over_baseline=0.0"`.
- **Output:** runs, layouts and models go to `<OutputDir>/fast/` (`AnimusForge.Fast.OutputDir`), never into the real
  `runs/`: a fast run never archives, seeds from or overwrites a real one. Each `forge fast` trains from scratch
  there. The progress report says `(fast)`; `forge pause`, `cancel`, `skip`, `resume` (without names) and `export`
  (without a scenario) work on the fast run; `forge clean fast` deletes it all.
- **What to look at:** in `fast/runs/<stage>/`, `eval.csv` (the `at_start` evaluation against the later ones, and the
  baseline in `eval_baseline.json`), `metrics.csv` (losses, entropy, approx KL) and `finished.json`; in the console,
  the progress report's warnings (NaN metrics, collapsing entropy, a learner that stopped answering). A few hundred
  thousand steps show whether the score, loss and entropy move -- not whether the stage can be learned.

## Baselines (no Python)

Run a scripted policy from the console and read the episode means in its progress report (or `forge status`):

```
forge run stage1_duel greedy 1024        # class/role stages: first usable spell or trinket
forge run stage1_duel fight 1024         # class/role stages: close in, fight, eat, drink, heal
forge run warrior_dummy_20 white_only    # warrior_dummy_20: white swings only, until forge cancel
forge run warrior_dummy_20 rotation 512  # warrior_dummy_20: levelling priority
forge run stage1_duel random 1024
```

These are the numbers the learner has to match or beat. They double as a mechanics check. With
`white_only`, `white_hits` per episode should be roughly `EpisodeSeconds / weapon speed` (minus
misses and dodges). To baseline every queued scenario in one go, set `AnimusForge.Policy` to the scripted policy
and `AnimusForge.Queue.LocalEpisodes` to the episodes per scenario, then `forge start`.

## Training

With `AnimusForge.Policy = "remote"` and `AnimusForge.Learner.AutoStart = 1` (the defaults), the
worldserver starts the learner itself once the envs are built, for the current scenario of `forge start` or
`forge resume`:

```
<WorkDir>/.venv/bin/python -u -m animus.train --config configs/<scenario>.yaml \
    --socket <AnimusForge.Socket> --run-name <scenario> \
    --runs-dir <OutputDir>/runs --layouts-dir <OutputDir>/layouts
```

- **WorkDir** defaults to this module's `python/` directory; each `AnimusForge.Learner.*` key
  overrides one part.
- **Output** is appended to `animus-learner.log` in `LogsDir`.
- **An exit** is logged in the worldserver log, with the exit code.
- **Configs** may start with `extends: <other>.yaml`: the file is merged over that one, section by section. Every
  class/role stage extends `stage1_duel.yaml` (directly or through an earlier stage).
- **Devices:** `train_device: auto` updates on the GPU when torch sees one (CUDA or ROCm), else the CPU.
- **Checkpoints:** `checkpoint_<update>.pt` every `checkpoint_every` updates, keeping the newest `keep_checkpoints`
  (5); `latest.pt` and `best.pt` are always kept.
- **Fresh or resumed:** `forge start` trains from scratch. Whatever `runs/<scenario>/` held is first moved to
  `runs/_archive/<scenario>-<time>/` (nothing is deleted). `forge resume` passes `--resume` instead: the learner
  continues `runs/<scenario>/latest.pt` in place (networks, optimizers, step count, the convergence test, restarts
  and the best evaluation; `metrics.csv` is appended to) and refuses if the scenario's layouts or dimensions changed
  since. The env count, decision interval and episode length may change. The server itself never resumes on
  startup, and its game clock is 64-bit, so a run goes from start to finish in one server lifetime.
- **Overrides:** `AnimusForge.Learner.Args` appends arguments to every learner, typically
  `--set key=value` (dotted keys for sections: `--set eval.episodes=64`), to change config values for a
  whole queue without editing YAML -- e.g. a short pass over a curriculum before the long run.
- **Shutdown, cancel and skip:** closing the socket makes the learner save a checkpoint and exit; it is
  interrupted after 10 s (15 s for a cancel or skip) if it has not.

### Evaluation, convergence and stage targets

Training curves are noisy when every episode rolls a new character, so the class/role configs score the
networks on **seeded evaluation episodes** as they train (`eval:` in the YAML, `animus/evaluation.py`):

- **Seeds:** the learner switches the sim to evaluation (protocol `MODE`). Every env resets, and episode
  seed index *i* (0 to `eval.episodes - 1`) is built right after the world thread's random numbers are
  reseeded from (`eval.seed`, *i*): the same race, level, spec, talents, gear, opponents and spawn points
  every evaluation, whatever the env count. Combat rolls stay random, so scores are averages, not replays.
  Afterwards the learner switches back and training resumes from fresh episodes.
- **Policy:** argmax actions (`eval.deterministic`). The score is the mean episode return -- the scenario's
  own reward -- so it measures what training optimises and compares checkpoints of one scenario.
- **Baseline:** `eval.baseline` (`fight` for the class/role stages; `greedy`, the first usable spell or trinket, also exists) is played by the sim on the
  same seeds once per run and cached in `eval_baseline.json`.
- **When:** before training (`eval.at_start`, which also shows what a stage's warm start is worth), every
  `eval.every_env_steps` (20M), and at `total_env_steps`.
- **Output:** the log prints the score with its standard error, the best so far and a table of score and key
  episode stats (`eval.report`) overall and per level band (1-20, 21-40, 41-60, 61-80), learner next to
  baseline. `eval.csv` has one row per evaluation, `eval.jsonl` the full tables, TensorBoard `eval/*` and
  `eval_<band>/*`.
- **Best model:** each new best score saves `best.pt`; the stages that extend this one seed from it (see
  Bootstrapping; its `latest.pt` if there is no best).

A stage ends on two questions (`animus/stage.py`), not on a fixed episode count:

- **Done learning? (`convergence:`)** An evaluation is a new best only if it beats the best by the largest of
  `min_improvement` (a fraction of it), `min_improvement_abs` and `z` standard errors of the two scores, so a
  lucky evaluation inside the combat-roll noise does not count. The stage has converged once `patience`
  evaluations in a row set no new best *and* the trend of the last `window` scores, projected `patience`
  evaluations ahead, would not reach that margin either (a slow climb hidden by the noise keeps training), and
  not before `min_env_steps`. `patience: 0` trains to `total_env_steps`.
- **Good enough? (`target:`)** The best networks must beat the baseline by `min_over_baseline` overall (0.1 =
  10% better, sign-safe for negative rewards), reach `min_layout_over_baseline` for every class/role with at
  least `min_layout_episodes` evaluation episodes (a looser floor, so no layout is carried by the average into
  the next stage), and pass any `metrics` gates on episode info (`{killed: {min: 0.8}}`). They are checked on
  the evaluation that set the best, then again on `confirm_episodes` held-out episodes (`confirm_seed`),
  because a best picked out of many evaluations is partly luck. With no gates set, a converged stage moves on.
- **Stuck below the target? (`restarts:`)** Converging below the target is treated as a local optimum: the
  networks reload `best.pt`, the entropy bonus is multiplied by `entropy_boost` and decays back with
  `entropy_half_life_env_steps`, the optimizers start fresh (`reset_optimizers`) and, optionally, the weights are
  shrunk and perturbed towards a fresh initialisation (`shrink`, `perturb`). The convergence test then starts
  over (the best score is kept), up to `max_restarts` times.

| Converged, target passed | Converged, below target | `total_env_steps` reached |
| --- | --- | --- |
| exit 0, the plan moves on (`converged`) | restart; once restarts run out, exit 3 and the plan halts (`below_target`) | target passed: exit 0 (`total_env_steps`); below it: exit 3 (`budget_below_target`) |

A bad target is caught at startup (a metric the scenario does not report, a baseline gate without
`eval.baseline`), not at the end of the stage. `finished.json` records the reason, the restarts and the gate
values; `stage.jsonl` has every restart/advance/halt decision. The target numbers in the class/role configs are
first guesses: tune them after a pilot run, where `eval.jsonl` has each class/role's score next to the
baseline's.

To run the learner yourself, set `AnimusForge.Learner.AutoStart = 0`, then from `python/`:

```
pytest                                                    # protocol, GAE and trainer tests
python -m animus.train --config configs/stage1_duel.yaml --run-name stage1_duel
python -m animus.evaluate --checkpoint runs/stage1_duel/best.pt --episodes 128 --seed 1000 --baseline fight
```

A hand-started learner can start before or after the worldserver; it retries until the socket
exists. Disconnecting (Ctrl+C) is safe: the sim waits for the next learner and resets every env when
one connects.

A hand-started learner uses the config's `runs_dir` and `layouts_dir` (`runs`, `layouts`) unless given
`--runs-dir` / `--layouts-dir`; point them at the sim's `AnimusForge.OutputDir` so it finds the stage's `stage.json`.

Each run writes `config.yaml`, `spec.json`, `stage.json` (class/role stages: blocks, seed chain, models, the
effective tuning), `metrics.csv`, TensorBoard logs (if installed) and checkpoints to `runs/<run_name>/`; with
evaluation also `eval.csv`, `eval.jsonl`, `eval_baseline.json`, `best.pt`, `stage.jsonl` and, once done,
`finished.json`.

## Export

Training writes checkpoints to its run directory and nothing else: models are never published or copied anywhere
automatically. Export a checkpoint's actor to `.amdl` models from the console, even while training goes on:

```
forge export stage1_duel                 # best.pt, else latest.pt, into AnimusForge.ModelDir (models/)
forge export stage1_duel latest
```

It runs `python -m animus.export` in the background (output in `animus-export.log`), the same as by hand:

```
python -m animus.export --checkpoint <OutputDir>/runs/stage1_duel/best.pt --out ../models \
    --layouts-dir <OutputDir>/layouts
```

Copying exported models to a server that plays them is also done by hand.

It writes one model per layout, named by the stage's `stage.json`: `warrior_dps_duel.amdl`, `priest_heal_duel.amdl`,
... (a single-layout scenario such as `warrior_dummy_20` writes `warrior_dummy_20.amdl`). Each is the layout's input
adapter, the shared trunk and the layout's action head, in the plain MLP format below.

The file format is documented at the top of `animus/export.py`. `tests/test_export.py` checks that
the exported network reproduces the torch actor's greedy actions.

A class/role model's inputs and outputs are defined by its stage's blocks (`src/Scenario/ClassRole/Blocks/`: each
block's features, actions and what they do) placed one after another by `Layout/Layout.*`; the scenario only
describes each seat's situation (`SeatView`: enemies, owner, teammates, opponent, pull timing). Each exported
class/role model gets its layout manifest beside it (`<model>.json`, e.g. `warrior_dps_duel.json`, format 3): the
stage, class/role, class, role, sizes and specs, then every block with its observation and action spans
(`[first, count]`) and its own entries -- the catalog and talents (core), stable slots (duel), tactical spells
(pack), sustain spells (gauntlet), ally heals (companion), member slots (party). Anything that plays the model must
build exactly that manifest for it; a different manifest means the model would read its observations and actions
as something else. The sim writes a manifest only when its content changed.

## Wire protocol

`src/Bridge/Protocol.h` is the reference; `python/animus/protocol.py` mirrors it. Messages are
little-endian: `HELLO` → `SPEC` → (`STEP` → `ACT` or `MODE`)* → `CLOSE`. `MODE` switches between
training and seeded evaluation (optionally running a scripted baseline instead of the learner's actions)
and is answered with a fresh `STEP`. `SPEC` lists the agent layouts (name, observation size, action count);
observation and mask rows are padded to the largest. A `STEP` carries, for every env:
- each agent's observation, layout id and action mask, and the env's critic state
- each agent's reward
- `done` / `terminated`
- the final observation and state of an episode that just ended (for truncation bootstrapping)
- each agent's episode totals and, in evaluation, the ended episode's seed index

## Adding a scenario

**A class/role stage:**

1. Add a `StageDefinition` to `src/Scenario/ClassRole/Stages/Stages.cpp`: its name and model suffix, the stage it
   extends, its blocks (the extended stage's, then any new ones), its seats and what it fights (encounters).
2. If it needs new observations or actions, add a block: a `BlockId`, a `Block` implementation in `Blocks/`
   (`Size`, `Observe`, `Apply`, `DescribeManifest`) and its entry in `Blocks/Blocks.cpp`. Add what it needs to know
   about the world to `SeatView`.
3. If its envs contain something new, add an `Encounter` in `Encounters/` (build, update, reward terms, episode
   info, critic state), create it from the definition in `ClassRoleScenario`'s constructor, and put its tuning in
   `ClassRoleTuning` (one field and one `Visit` line; `mod_animus_forge.conf.dist` documents it).
4. Write `python/configs/<name>.yaml` with `extends:` the extended stage's config, and add the name to
   `AnimusForge.Queue` (or leave the queue empty to run every stage).

**A standalone scenario:**

1. Implement `AnimusForge::Scenario` (`src/Scenario/Scenario.h`): `Setup` builds bots and targets once,
   `Reset` restarts an episode in place, and `Observe` / `ApplyActions` / `Reward` define the MDP.
   `IsTerminal` is optional.
2. Create it by name in `CreateScenario` and list it in `ScenarioNames` (`src/Scenario/Scenario.cpp`).
3. Add its name to `AnimusForge.Queue` and write a learner config under `python/configs/`.

The protocol and learner are shape-generic. A multi-agent scenario sets `AgentsPerEnv > 1` and
provides a real global `State`, and MAPPO's shared actor and centralized critic handle it.

## Core requirements

The class/role scenarios rebuild every bot each episode, hundreds of times a second in a fast sim.
That relies on small Forge core APIs:

- `WorldSession::SetSimSession(true)` (set by `BotFactory::Create`): the session's account and
  characters exist only in memory, so logout, play time and instance binds write nothing to the
  database. Without it every rebuild queued character-database writes faster than MySQL applied
  them, and the async queue grew without bound.
- `Player::SetSocial` (set by `BotFactory::Create` to an empty `SocialMgr` list): logout and far
  teleports read the social list, which `LoadFromDB` normally attaches.

- `Group::SetSimGroup` (set by the party stage): a group that lives only in memory -- no database rows,
  character cache entries, instance bind resets or homebind timers -- so a party can be rebuilt every episode.
- `rand_seed` (`RandomSeed.h`): restarts the calling thread's `urand`/`frand`/... sequence from a seed,
  so seeded evaluation episodes roll the same characters and opponents every time.

Bots also reuse a fixed pair of player GUIDs and sessions per seat (`BotSlot`), because the core keeps some
per-GUID state (instance bind storage) for the life of the server. Their account ids come from `BotAccounts.h`,
one range per kind of bot.

## Known limits

- **Cooldowns and the GCD** use the game clock, which the core is being moved onto the sim tick
  (separate work).
- **Scripted owner and PvP opponent:** the companion and party stages' owner and stage 6's enemy player are
  scripts; the party's other members and stage 7's opponent are learned.
- **Throughput** in `remote` mode is bounded by one Python round trip per decision for all envs.
  Raise `Envs` until the learner, not the world thread, is the bottleneck.
