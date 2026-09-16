# mod-animus-forge

Training scenarios for the Animus Forge sim host (the `forge` branch of AzerothCore). The core
stays a lightweight, faster-than-real-time instance simulator. This module owns everything about
a training run:

- in-process bots with no socket and no character row
- arenas, targets, episodes and resets
- observation, action and reward encoding
- a lock-step bridge to a Python MAPPO learner

Its main work is the curriculum: eleven stages that train one policy for every class and role,
from a one-on-one duel up to parties and self-play arenas, joined in stage 8 into one policy for PvE and PvP, and a
branch for getting somewhere: riding, flying, and Warsong Gulch's rules.

The curriculum itself -- stages, blocks, layouts, encounters, characters, env pools and bots -- lives in
[animus-lib](https://github.com/Moloch17/animus-lib) (`modules/mod-animus-lib`), which mod-animus shares: a
stock AzerothCore with mod-animus runs the very same stages for a game master to watch (`.animus stage start`).
This module adds what only training needs: the plan and console, the learner bridge and process, progress reports
and export. Paths below starting with `animus-lib/` are in that repository.

**The Animus manual** ([`docs/manual/`](docs/manual/README.md)) explains the whole project in depth: the forge core,
animus-lib, the curriculum, this module and its learner, mod-animus, operations and a full reference.

## How it fits together

```
worldserver (forge)                                   python -m animus.train
 └─ World::ForgeUpdate ── every tick ─┐
     sScriptMgr->OnWorldUpdate        │  EnvPool: N envs, one instance map each
                                      │   ├─ Scenario::Reward / Observe / auto-reset
                                      │   └─ every tick (AnimusForge.DecisionMs):
                                      │        STEP  ── unix socket ──►  MAPPO actor picks actions
                                      │        ACT   ◄───────────────   (lock-step)
                                      └─ Scenario::ApplyActions
```

- **Envs are instance maps.** Each env's bots get their own `InstanceMap`, so envs update in
  parallel on `MapUpdate.Threads`. Envs are created once at startup and reset in place; maps and
  players are never recreated, so nothing touches the database while the run is going.
- **Bots are sessionless players.** See `animus-lib/src/Bot/BotFactory.cpp`. They are deliberately kept out
  of `WorldSessionMgr`: a socketless session registered there is deleted, and its player saved,
  on the next update.
- **Damage is measured in `UnitScript::DealDamage`**, before the victim's AI runs: a creature script may change
  the amount in `DamageTaken` (a training dummy zeroes it), so `OnDamage` would not always see what was dealt.
  animus-lib's hooks (`animus-lib/src/Hooks/AnimusLibScripts.cpp`) feed every env pool registered with its
  `PoolRegistry`; the forge registers its pool while a scenario runs.
- **The learner drives time.** In `remote` mode the world thread waits on the learner each
  decision, and while no learner is connected. Scripted policies run without Python.
- **The console drives the sim.** The sim starts idle; `forge start`, `forge pause`, `forge cancel` and the other
  [console commands](#operating-the-sim-console) decide what runs. While the world thread waits on the learner it
  keeps answering them.

## Scenarios

List the scenarios to train in `AnimusForge.Queue` (see
[Training every model](#training-every-model-animusforgequeue)). The learner auto-starts with
`configs/<scenario>.yaml` and trains in `<OutputDir>/runs/<scenario>/`.

### The curriculum: one policy for every class and role

Eleven stage scenarios -- `stage1_duel`, `stage2_pack`, `stage3_gauntlet`, `stage4_companion`, `stage5_party`,
`stage6_pvp`, `stage7_arena`, `stage8_crossroads`, `stage9_travel`, `stage10_flight`, `stage11_flag` -- each train
**one policy for every class/role** of `AnimusForge.ClassRoles` (all 18 by default). Each stage is its own scenario, so every
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

A stage is one entry in `animus-lib/src/Scenario/Curriculum/Stages/Stages.cpp` (`StageDefinition`):

- **Blocks** (`Blocks/`): the groups of observation features and actions its layouts have, in order -- `core` (the
  character, its spells, trinkets and talents), `duel` (movement, auto-attack, pets, casting, forms), `pack` (enemy
  slots, target selection, tactical spells), `gauntlet` (pull timing, food, drink, sustain spells), `companion`
  (the owner), `party` (three teammates), `pvp` (the enemy player). A block is stateless: it sizes its slice of a
  layout, writes its features and mask, and applies its actions. `Layout` places each block after the previous
  one, and the manifest lists every block with its spans.
- **What it extends:** the stage it builds on and seeds from. It keeps the base's blocks it needs, drops the rest and
  adds its own; several stages can extend the same base, so the curriculum is a tree:

  ```
  duel ─┬─ pack ─ gauntlet ─ companion ─ party ─┬─ crossroads   (PvE ...
        └─ pvp ─ arena ─────────────────────────┘                ... and PvP joined: every block)
  ```

  The sim checks each definition at startup (the base exists and comes earlier, no block twice, every part has the
  blocks it needs) and leaves out a stage that breaks a rule.
- **Arenas** (`ArenaDefinition`): what its episodes are -- the seats (one, a party, a mirror pair), what they fight
  (a creature, pulls on a pack or gauntlet schedule, a scripted or mirror enemy player, an ambush of the owner by
  enemy players beside pulls or on its own), whether there is an owner and
  a party group, whether it is PvP (resilience gear, no resurrecting oneself) and its episode length. Every episode
  draws one of the stage's arenas by weight (`AnimusForge.Curriculum.Arena.<stage>.<arena>.Weight`, default the
  definition's), after the evaluation reseed, so a seed always gets the same arena. A stage's blocks are the union of
  what its arenas need, so one policy learns every situation of the stage; stages 1-7 have one arena each. An env
  switching arenas removes what the old one had (the owner, the group, the enemy player) before rebuilding.
- **Encounters** (`Encounters/`): what its envs contain besides the seats -- a creature, pulls
  (one pack or the gauntlet's schedule), a scripted owner, a party group, an enemy player (scripted or the other
  seat). The stage creates every encounter any of its arenas uses. Each builds and updates its part of the world,
  keeps its own episode state, and adds its reward terms, episode info columns and critic state; its columns read 0
  in an episode whose arena does not use it. `StageScenario` builds the seats and calls the hooks of the episode's
  encounters in a fixed order.
- **Rewards** are added term by term (`Rewards/RewardLedger.h`); every term's episode sum is reported as the
  episode info column `reward_<term>` (`reward_damage_dealt`, `reward_clear`, `reward_owner_death`, ...), so
  TensorBoard shows what each stage actually pays for.
- **Tuning:** every reward weight, chance, level spread and scripted-player timing is a config key,
  `AnimusForge.Curriculum.<Group>.<Name>` (the CURRICULUM TUNING section of `mod_animus_forge.conf.dist`). The sim
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

The table lives in `animus-lib/src/Scenario/Curriculum/Character/ClassRoleProfile.cpp`, with each spec's stat profile,
range and weapon layouts. Every episode builds a new character (the env's bot is replaced):

- **Race and level:** a random race the class allows (`playercreateinfo`), random gender; half the characters
  are level 61-80 and the rest any level 1-80 (55-80 for death knights), since most players are high level
  (`Characters.HighLevelFirst`, `Characters.HighLevelChance`).
- **Talents and glyphs:** one of the role's specs with its standard 3.3.5 build (`Character/SpecBuilds.cpp`, 31 specs:
  the talents players take, in the order they take them; generated from animus-lib's `tools/spec_builds/builds.py`,
  which `validate.py` checks). Each point goes to the first talent in that order that still wants ranks and can take
  one (row and prerequisite rules follow `Player::LearnTalent`), so a low-level character has the talents players pick
  first; every build spends exactly 71 points at 80. The glyph slots the level has opened get the spec's standard
  major and minor glyphs the level can use.
  Not every character gets that build, though: `Characters.NoisyTalentChance` percent stop a few points short and
  spend the rest at random (`Characters.TalentNoisePoints`), and `Characters.RandomTalentChance` percent spend every
  point at random, the spec's tree first. A standard build is the same every time for a spec and a level, so a policy
  trained only on those can ignore the talent features in its observation and memorise the spec; one that meets all
  three has to read what it was given, as it must on a live server. Each episode reports which it was as
  `talent_plan`, and every evaluation scores the three separately.
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
- **Reward** (defaults; every weight is an `AnimusForge.Curriculum.Duel.*` key): per decision, damage dealt as a
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
the stage it extends (the sim warns otherwise); the two branches can go in either order.

**Merging branches:** a stage may also list `Merges`, further earlier stages (`stage.json` `merges`). With
`merge_from: auto` the learner seeds, after the extended stage, every layout's blocks that only a merged stage has from
that stage's `best.pt` (input columns and action rows; never the trunk). Those weights were trained against another
trunk, so a merge stage is usually **distilled** too (`distill:` in its config, `animus/distill.py`): on the decisions
of each arena that a parent already plays, the policy loss gains `coef` x KL(parent's policy || policy), over the
actions both have, decaying with `half_life_env_steps`. `teachers: auto` picks, per arena, the first parent (the extended
stage, then the merges) whose `stage.json` has that arena; a map `{arena: checkpoint}` names them. Each decision's
arena comes from the critic state (`stage.json` `state`). `metrics.csv` logs `distill_coef`, `distill_kl` and
`distill_rows`. The pilot `mix_duel_pvp` extends `stage6_pvp`, merges `stage1_duel` and is taught by both.

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
  talent build and glyphs, its trainer spells and level-appropriate gear. It gets the companion's faction so either
  faction's races can be paired. Between pulls it wanders near the spawn point and recovers health and mana; each
  pull spawns around it. A tank owner starts every pull and taunts enemies off others; a healer owner heals the most
  hurt party member; a damage dealer walks in after 1.5-5 s (and starts the pull itself 30% of the time) and
  fights. It casts one of its own spells every 2-4 s. Linked packs join in on whoever their engaged member fights.
  Everything here is `AnimusForge.Curriculum.Owner.*`, `Pulls.*` and `ScriptedPlayers.*` tuning.
- **Actions:** the gauntlet's, then follow the owner, assist (target the owner's target), guard (target an
  enemy attacking the owner), one "cast on the owner" action per single-target heal, and one per **revive**:
  each resurrection spell (Resurrection, Redemption, Ancestral Spirit, Revive, Rebirth) on the dead owner, and
  a warlock's soulstone on the living owner.
- **Observation:** the gauntlet's, then the owner's presence, health, mana, distance, bearing, combat,
  movement, level difference and class, how many enemies attack it, which enemy slot it attacks, which
  enemies attack it, and each owner heal's and revive's known/cooldown.
- **Reward:** the gauntlet's, with kills and clears counting double, plus, by role (defaults of
  `AnimusForge.Curriculum.Owner.*`):
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

`configs/stage6_pvp.yaml` scores against the `fight` baseline. Against itself a policy's score does not track
progress, so `configs/stage7_arena.yaml` evaluates with `eval.opponent_baseline`: the `fight` baseline plays the
second seat, the score is the learner's seat against it, and the baseline score is `fight` against `fight` on the
same seeds. Convergence and the target then work as in any stage.

#### Stage 8 (`stage8_crossroads`): the crossroads

Both branches join in one policy, the model that fights monsters and players alike. It extends `stage5_party` (the
trunk and the PvE blocks), merges `stage7_arena` (the pvp block), `stage6_pvp`, `stage4_companion`, `stage3_gauntlet`
and `stage1_duel`, and adds two blocks. Its layouts are every block: core, duel, pack, gauntlet, companion, party, pvp,
context, hostiles.

- **Arenas** (weight, episode length): `companion` (20, 300 s), `party` (20, 300 s), `arena_1v1` (15, 60 s),
  `pvp_scripted` (10, 60 s), `gauntlet` (10, 300 s), `duel` (5, 60 s), and two that need PvE and PvP at once:
  - `ambush` (15, 300 s): the companion's gauntlet, and 1-2 scripted enemy players (the PvP opponent's classes, roles
    and gear) arrive 20-120 s in (`Ambush.MinMs/MaxMs`) and attack the owner while it lives, then the nearest seat.
    They take enemy slots the pulls leave free (a pull has at most 4 minus the arena's ambushers creatures); the pulls
    still clear, pay and schedule on their creatures only. Every seat earns `Ambush.Kill` (3) per ambusher killed;
    the owner's rewards pay for protecting it.
  - `escort_duel` (5, 90 s): the owner and one enemy player, no pulls, paid as the duel against it.
- **`context` block** (12 features, no actions): owner present and alive, living teammates, living enemy players and
  creatures in the enemy slots, the nearest enemy player's distance, whether a player attacks the bot or the owner,
  PvP flag, battleground/arena or dungeon/raid map, self-resurrection allowed, group size. A live server can fill all
  of them, so the policy tells PvE from PvP without an arena id.
- **`hostiles` block** (14 features per enemy slot, no actions): player or creature, class, casting a heal,
  stealthed, pet out. The pack block's target slots select players and creatures alike.
- **Learner** (`configs/stage8_crossroads.yaml`): distilled with `teachers: auto` (each earlier arena taught by the
  first parent that has it; the new arenas learn from the reward), the arena seat of `arena_1v1` scored against
  `fight`, and every arena gated on its own episodes.
- **Episode info:** the union of every arena's columns, plus `ambushers` and `ambushers_killed`; `reward_player_kill`.

#### Stages 9-11: travel, flight and the flag match

- **`stage9_travel`** (from the duel): a character of level 20+ with its level's riding and mounts gets to a place
  60-320 yd away by path in Old Hillsbrad. A mount's cast time pays only on a long trip; arriving on foot lets it fight.
- **`stage10_flight`** (from travel): level 60+ in Outland's Nagrand, a place 350-700 yd away. Take off, keep a height,
  land, dismount; falling off a mount in the air hurts. The envs share the continent, each in its own phase.
  Battlegrounds never allow flying mounts, so flying is for the open world.
- **`stage11_flag`** (from the arena, merging travel): Warsong Gulch's rules between two learned seats with bases
  100-180 yd apart: take the other side's flag home, return one's own, stop the carrier (who can't ride); the dead
  stand up at their base after 15 s; first to three captures.

See the manual, chapter 4, for the blocks (`pet`, `travel`, `flag`), encounters and rewards.

#### Arena mix pilot (`mix_duel_pvp`)

Not part of the curriculum and left out of an empty `AnimusForge.Queue`: train it by name (`forge start mix_duel_pvp`).
It has stage 6's blocks (core, duel, pvp), seeds from `stage6_pvp`, and each episode is either the creature duel
(`duel`) or the scripted enemy player (`pvp_scripted`), half and half. It checks that one policy can train PvE and PvP
episodes side by side before the curriculum's later stages mix larger arenas. The episode info column `arena` is the
episode's index into `stage.json`'s `arenas` (with each arena's weight, seats, episode length and whether it is PvP);
the critic state has the arena as a one-hot.

### Training every model: `AnimusForge.Queue`

`AnimusForge.Queue` lists the scenarios `forge start` trains, one after another, when it is given none. Empty (the
default) is the whole curriculum, first stage to last; to train a single scenario, name it on the console
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

It needs animus-lib in `modules/mod-animus-lib`. When that directory is missing, configuring clones it
(`mod-animus-forge.cmake`, from `ANIMUS_LIB_GIT_URL` at `ANIMUS_LIB_GIT_REF`, default
`https://github.com/Moloch17/animus-lib.git` `master`) and builds it in the same configure; later configures find it
as a module like any other. Build both the same way (static, the default, or both dynamic); disabling the library
while this module is enabled stops the configure. The library needs no settings of its own.

Nothing else belongs in a forge build -- in particular mod-animus, which plays exported models on a stock
AzerothCore, is not built into the forge core. If its directory sits in `modules/`, disable it in the forge build:

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
| `AnimusForge.ClassRoles` | `""` | Class/roles the curriculum stages play; empty = all 18 |
| `AnimusForge.Envs` | `64` | Parallel envs (one instance map each) |
| `AnimusForge.DecisionMs` | `100` | Game time per decision, and the sim's world tick: every world update is one decision |
| `AnimusForge.EpisodeSeconds` | `60` | Game-time episode length |
| `AnimusForge.SpawnPoint.*` | Old Hillsbrad entrance | Dungeon map and position every env's bots start at |
| `AnimusForge.Policy` | `remote` | `remote`, `random`, or a scenario's scripted policy (`greedy`, `fight`) |
| `AnimusForge.ReportEpisodes` | `256` | Mean episode stats (`forge status`, local runs) are taken over N episodes |
| `AnimusForge.Socket` | `/tmp/animus-forge.sock` | Learner socket path |
| `AnimusForge.Learner.AutoStart` | `1` | Start the Python learner automatically (remote policy) |
| `AnimusForge.Learner.WorkDir` / `Python` / `Config` / `LogFile` | derived | Where and how the learner runs (see Training) |
| `AnimusForge.Learner.Args` | `""` | Extra learner arguments for every scenario, e.g. `--set total_env_steps=5000000` |
| `AnimusForge.ModelDir` | `models/` in the module | Where `forge export` writes models |
| `AnimusForge.Progress.Interval` | `0` | Seconds between extra progress reports while a scenario runs; `0` = only `forge status` and each stage's end |
| `AnimusForge.Fast.*` | see the `.dist` | The quick profile of `forge fast` (see [Fast test run](#fast-test-run-forge-fast)) |
| `AnimusForge.Curriculum.*` | see the `.dist` | The curriculum's tuning: reward weights, chances, level spreads, scripted players |

Relative paths in the path keys (`OutputDir`, `ModelDir`, `Socket`, `Learner.WorkDir`, `Learner.Python`,
`Learner.Config`, `Learner.LogFile`, `Fast.Learner.Overlay`) are relative to the directory of the `worldserver.conf`
the server loaded, not to its working directory. `Fast.OutputDir` is the exception: it nests inside `OutputDir`.

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
| `forge fast [scenario ...]` | A quick training run of these on an easier problem, in the fast output directory (default: `AnimusForge.Fast.Queue`, or every curriculum stage when it is empty, none skipped), to see training work before a long run (see [Fast test run](#fast-test-run-forge-fast)) |
| `forge resume [scenario ...]` | Unpause; or continue the first scenario from its `latest.pt`, then train the rest. Without names: where the last plan stopped. With a crashed learner: restart it from its checkpoint |
| `forge pause` | Freeze after the current decision: maps, episode clocks and the learner all wait |
| `forge cancel` | Stop the plan; the learner saves `latest.pt` first, so `forge resume` can continue it |
| `forge skip` | End the current scenario (the learner saves) and start the next one |
| `forge run <scenario> <policy> [episodes]` | Run a scripted or random policy without a learner, for N episodes or until cancelled |
| `forge bench [scenario]` | Time the sim at every `AnimusForge.Bench.Threads` x `Envs` pair, then the fastest few with the learner (see [Throughput](#throughput-forge-bench)) |
| `forge bench apply` | Write the last benchmark's winning settings into `worldserver.conf` and `mod_animus_forge.conf` |
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

`forge status` prints this report at any time, and the console prints it when each stage ends (also every
`AnimusForge.Progress.Interval` seconds when that is set). The learner keeps `runs/<scenario>/progress.json` up to date after every update and
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

A real stage trains for hours before its first stage decision. `forge fast` trains stages on an easier problem that
learns in minutes, through the same pipeline, to see that a change to a scenario, the learner or the configs still
trains -- and learns -- before a long run:

```
forge fast                       # every curriculum stage in order, each from scratch: a full run, nothing skipped
forge fast stage2_pack           # just this stage, seeded from the fast run of stage1_duel; trains it again if done
```

- **Sim (`AnimusForge.Fast.*`):** 32 envs and four class/roles (`warrior_tank, priest_heal, rogue_dps, hunter_dps`: a
  tank and a healer for the party, energy, rage, mana and a pet), every character at level 20 (`Fast.Level`; 0 = the
  usual random levels; a stage's minimum level raises it). The decision interval, episode lengths and rewards are the
  real ones. Without names it trains `Fast.Queue`, which is empty by default: every curriculum stage in order, the
  `mix_duel_pvp` pilot included.
- **Learner (`configs/fast.yaml`):** merged over each stage's config with `--overlay`. A stage ends when its score
  stops improving: an evaluation of 64 seeded episodes every 100k env steps, done after 4 evaluations without a new
  best but not before 500k, with 3M as a safety cap. Stage targets (per-arena ones included) are off, so the plan never
  halts on a fast stage. Networks, gamma, entropy and the baseline stay each stage's own, so the run goes through the
  same code as a real one: spec and layouts, rollouts and updates, evaluation with the baseline, convergence,
  checkpoints, `finished.json`, seeding and distillation from earlier stages, and `forge export`. To test the target
  gates and restarts too: `AnimusForge.Fast.Learner.Args = "--set target.min_over_baseline=0.0"`.
- **Output:** runs, layouts and models go to `<OutputDir>/fast/` (`AnimusForge.Fast.OutputDir`), never into the real
  `runs/`: a fast run never archives, seeds from or overwrites a real one. A stage trained again archives its earlier
  fast run in `fast/runs/_archive/`. `forge start` and `forge fast` warn about a stage whose parents have no finished
  run to seed from. `forge pause`, `cancel`, `skip`, `resume` (without names) and `export` (without a scenario) work on
  the fast run; `forge clean fast` deletes it all.
- **What to look at:** the report printed at the end of each stage (best score against the baseline), and in
  `fast/runs/<stage>/`, `eval.csv` (the `at_start` evaluation against the later ones, the baseline in
  `eval_baseline.json`), `metrics.csv` (losses, entropy, approx KL) and `finished.json`.

## Throughput (`forge bench`)

Training speed is env steps per second: decisions x envs x seats. `forge status` shows it while training, with
where a decision's wall time goes:

| Part | What it is | What changes it |
|---|---|---|
| **world** | The map update (every env is its own instance map, spread over the `MapUpdater` pool) and the rest of the world tick | `MapUpdate.Threads` in `worldserver.conf`, and how much there is to simulate (envs, seats, pulls) |
| **sim** | This module in the tick: rewards, observations, action masks and applying actions, all on the world thread | `AnimusForge.Envs` (linear), the stage's blocks and seats |
| **learner** | The world thread blocked on the learner's actions, and its updates | `AnimusForge.Learner.TorchThreads`, `rollout_length`, the network size, and the GPU for updates |

The three compete: the learner's torch and the map update threads share the same cores. What is fastest is a
property of your machine, so measure it:

```
forge bench                      # AnimusForge.Bench.Scenario (stage1_duel), every Threads x Envs pair
forge bench stage5_party         # a four-seat stage costs more per env
forge bench apply                # write the winner into the configs
```

- Each trial starts the scenario with its own thread and env count, warms up (`Bench.WarmupTicks`), and is timed
  over `Bench.MeasureTicks` decisions with a scripted policy (`Bench.Policy`). The map update pool is switched
  between trials, so nothing has to restart.
- The fastest `Bench.LearnerTop` settings then run again with the real learner (x `Bench.LearnerTorchThreads`),
  which adds inference and updates -- what training actually costs.
- Nothing is trained: every trial runs in `<OutputDir>/bench/` with evaluation, seeding and distillation off, so
  `runs/` is untouched. Results go to `bench/bench.json`, and trials are skipped once memory passes
  `Bench.MaxMemoryPercent`.
- `forge cancel` stops a benchmark and puts the thread count back.
- `forge bench apply` rewrites `MapUpdate.Threads`, `AnimusForge.Envs` and (when it helped)
  `AnimusForge.Learner.TorchThreads` in place, backing each file up as `<file>.before-bench`. The thread count
  takes effect on the next restart, the env count at the next `forge start`.

**More envs is not only speed.** One learner update is `rollout_length x envs x seats` env steps, so a different
env count changes the batch PPO trains on (and the number of updates per env step). The report says so when the
winner's env count differs from the one you run. Treat the env count as a training setting that the benchmark
prices, not as a free win.

## Baselines (no Python)

Run a scripted policy from the console and read the episode means in its progress report (or `forge status`):

```
forge run stage1_duel greedy 1024        # curriculum stages: first usable spell or trinket
forge run stage1_duel fight 1024         # curriculum stages: close in, fight, eat, drink, heal
forge run stage1_duel random 1024
```

These are the numbers the learner has to match or beat. They double as a mechanics check. To baseline every queued
scenario in one go, set `AnimusForge.Policy` to the scripted policy
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
  curriculum stage extends `stage1_duel.yaml` (directly or through an earlier stage).
- **Devices:** `train_device: auto` updates on the GPU when torch sees one (CUDA or ROCm), else the CPU.
  `torch_threads` (0 = torch's default) caps its CPU threads, which share the machine with the sim's map
  update threads; the sim passes `AnimusForge.Learner.TorchThreads`.
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

Training curves are noisy when every episode rolls a new character, so the curriculum configs score the
networks on **seeded evaluation episodes** as they train (`eval:` in the YAML, `animus/evaluation.py`):

- **Seeds:** the learner switches the sim to evaluation (protocol `MODE`). Every env resets, and episode
  seed index *i* (0 to `eval.episodes - 1`) is built right after the world thread's random numbers are
  reseeded from (`eval.seed`, *i*): the same race, level, spec, talents, gear, opponents and spawn points
  every evaluation, whatever the env count. Combat rolls stay random, so scores are averages, not replays.
  Afterwards the learner switches back and training resumes from fresh episodes.
- **Even over the class/roles:** seed *i* plays layout *i % (layout count)*, so each class/role is scored on an
  equal share of the seeds and its score is measured as well as the run's. `eval.episodes` is what makes those
  per-class/role scores (and the convergence margin, which the score's standard error sets) mean anything: 1024
  episodes over 18 class/roles leave ~57 each, and cost seconds of sim time against the training between two
  evaluations.
- **Policy:** argmax actions (`eval.deterministic`). The score is the mean episode return -- the scenario's
  own reward -- so it measures what training optimises and compares checkpoints of one scenario.
- **Baseline:** `eval.baseline` (`fight` for the curriculum stages; `greedy`, the first usable spell or trinket, also exists) is played by the sim on the
  same seeds once per run and cached in `eval_baseline.json`.
- **When:** before training (`eval.at_start`, which also shows what a stage's warm start is worth), every
  `eval.every_env_steps` (20M), and at `total_env_steps`.
- **Self-play:** with `eval.opponent_baseline` the baseline plays the opponent seats of self-play episodes
  (protocol `MODE_FLAG_SCRIPTED_OPPONENTS`) while the learner plays the rest, and those seats' rows (episode info
  `opponent_seat`) are left out -- also of the baseline's own evaluation, which is then the baseline against itself.
- **Output:** the log prints the score with its standard error, the best so far and a table of score and key
  episode stats (`eval.report`) overall, per level band (1-20, 21-40, 41-60, 61-80), per class/role and, for a stage
  that mixes arenas, per arena, learner next to baseline. `eval.csv` has one row per evaluation, `eval.jsonl` the
  full tables, `eval_episodes.jsonl` one row per scored episode (its seed, class/role, return and reported episode
  info) so a class/role's failures can be read seed by seed rather than as a mean, TensorBoard `eval/*`,
  `eval_<band>/*` and `eval_arena_<arena>/*`. `python -m animus.evaluate --episodes-file PATH` writes the same
  rows for one checkpoint.
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
  the next stage), and pass any `metrics` gates on episode info (`{killed: {min: 0.8}}`). A stage that mixes
  arenas can gate each arena on its own episodes: `arenas: {duel: {min_over_baseline: 0.1}, pvp_scripted:
  {metrics: {won: {min: 0.5}}}}`, skipping an arena with fewer than `min_arena_episodes`. They are checked on
  the evaluation that set the best, then again on `confirm_episodes` held-out episodes (`confirm_seed`),
  because a best picked out of many evaluations is partly luck. Each score gate passes within `noise_z` standard
  errors of what it requires (of the difference between the two scores): a class/role holds only its share of the
  episodes, so without that allowance one that is really level with its baseline fails about half the time. With
  no gates set, a converged stage moves on.
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
values; `stage.jsonl` has every restart/advance/halt decision. The target numbers in the curriculum configs are
first guesses: tune them after a pilot run, where `eval.jsonl` has each class/role's score next to the
baseline's.

To run the learner yourself, set `AnimusForge.Learner.AutoStart = 0`, then from `python/`:

```
pytest                                                    # protocol, GAE and trainer tests
python -m animus.train --config configs/stage1_duel.yaml --run-name stage1_duel
python -m animus.evaluate --checkpoint runs/stage1_duel/best.pt --episodes 128 --seed 1000 --baseline fight
python -m animus.evaluate --checkpoint runs/stage7_arena/best.pt --baseline fight --opponent-baseline
```

A hand-started learner can start before or after the worldserver; it retries until the socket
exists. Disconnecting (Ctrl+C) is safe: the sim waits for the next learner and resets every env when
one connects.

A hand-started learner uses the config's `runs_dir` and `layouts_dir` (`runs`, `layouts`) unless given
`--runs-dir` / `--layouts-dir`; point them at the sim's `AnimusForge.OutputDir` so it finds the stage's `stage.json`.

Each run writes `config.yaml`, `spec.json`, `stage.json` (curriculum stages: blocks, seed chain, models, the
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
... (a scenario without a stage.json and a single layout writes `<scenario>.amdl`). Each is the layout's input
adapter, the shared trunk and the layout's action head, in the plain MLP format below.

The file format is documented at the top of `animus/export.py`. `tests/test_export.py` checks that
the exported network reproduces the torch actor's greedy actions.

A class/role model's inputs and outputs are defined by its stage's blocks
(`animus-lib/src/Scenario/Curriculum/Blocks/`: each block's features, actions and what they do) placed one after
another by `Layout/Layout.*`; the scenario only
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
training and seeded evaluation (optionally running a scripted baseline instead of the learner's actions, or only
on the opponent seats of self-play episodes) and is answered with a fresh `STEP`. `SPEC` lists the agent layouts (name, observation size, action count);
observation and mask rows are padded to the largest. A `STEP` carries, for every env:
- each agent's observation, layout id and action mask, and the env's critic state
- each agent's reward
- `done` / `terminated`
- the final observation and state of an episode that just ended (for truncation bootstrapping)
- each agent's episode totals and, in evaluation, the ended episode's seed index

## Adding a scenario

**A curriculum stage:**

1. Add a `StageDefinition` to `animus-lib/src/Scenario/Curriculum/Stages/Stages.cpp`: its name and model suffix, the
   stage it extends, its blocks (the extended stage's, then any new ones), its seats and what it fights (encounters).
2. If it needs new observations or actions, add a block: a `BlockId`, a `Block` implementation in `Blocks/`
   (`Size`, `Observe`, `Apply`, `DescribeManifest`) and its entry in `Blocks/Blocks.cpp`. Add what it needs to know
   about the world to `SeatView`.
3. If its envs contain something new, add an `Encounter` in `Encounters/` (build, update, reward terms, episode
   info, critic state), create it from the definition in `StageScenario`'s constructor, and put its tuning in
   `CurriculumTuning` (one field and one `Visit` line; `mod_animus_forge.conf.dist` documents it).
4. Write `python/configs/<name>.yaml` with `extends:` the extended stage's config, and add the name to
   `AnimusForge.Queue` (or leave the queue empty to run every stage).

**A standalone scenario:**

1. Implement `Animus::Scenario` (`animus-lib/src/Scenario/Scenario.h`): `Setup` builds bots and targets once,
   `Reset` restarts an episode in place, and `Observe` / `ApplyActions` / `Reward` define the MDP.
   `IsTerminal` is optional.
2. Create it by name in `CreateScenario` and list it in `ScenarioNames` (`animus-lib/src/Scenario/Scenario.cpp`).
3. Add its name to `AnimusForge.Queue` and write a learner config under `python/configs/`.

The protocol and learner are shape-generic. A multi-agent scenario sets `AgentsPerEnv > 1` and
provides a real global `State`, and MAPPO's shared actor and centralized critic handle it.

## Core requirements

The curriculum stages rebuild every bot each episode, hundreds of times a second in a fast sim.
That relies on small Forge core APIs. animus-lib also builds on a stock core, so it reaches them through
`CoreHooks` (`animus-lib/src/Core/CoreHooks.h`), which this module fills in at load
(`src/Hooks/AnimusForgeScripts.cpp`):

- `WorldSession::SetSimSession(true)` (`CoreHooks::MarkSimSession`, called by `BotFactory::Create`): the session's
  account and characters exist only in memory, so logout, play time and instance binds write nothing to the
  database. Without it every rebuild queued character-database writes faster than MySQL applied
  them, and the async queue grew without bound.
- `Group::SetSimGroup` (`CoreHooks::MarkSimGroup`, called by the party stage): a group that lives only in memory --
  no database rows, character cache entries, instance bind resets or homebind timers -- so a party can be rebuilt
  every episode.
- `rand_seed` (`RandomSeed.h`; `CoreHooks::SeedRandom`): restarts the calling thread's `urand`/`frand`/... sequence
  from a seed, so seeded evaluation episodes roll the same characters and opponents every time.

A bot's social list, which logout and far teleports read, is attached by `BotFactory::Create` through the private
`Player::m_social` member, so no core setter is needed. Bots also reuse a fixed pair of player GUIDs and sessions per
seat (`BotSlot`), because the core keeps some per-GUID state (instance bind storage) for the life of the server.
Their account ids come from animus-lib's `BotAccounts.h`, one range per kind of bot.

## Known limits

- **Cooldowns and the GCD** use the game clock, which the core is being moved onto the sim tick
  (separate work).
- **Scripted owner and PvP opponent:** the companion and party stages' owner and stage 6's enemy player are
  scripts; the party's other members and stage 7's opponent are learned.
- **Throughput** in `remote` mode is bounded by one Python round trip per decision for all envs.
  Raise `Envs` until the learner, not the world thread, is the bottleneck. With `overlap_updates` the PPO update
  runs on a worker thread while the sim collects the next rollout, instead of the sim standing idle for it; the
  rollout then acts on the update before last, one update staler than the serial loop.
