# mod-animus-forge

Training scenarios for the Animus Forge sim host (the `forge` branch of AzerothCore). The core
stays a lightweight, faster-than-real-time instance simulator. This module owns everything about
a training run:

- in-process bots with no socket and no character row
- arenas, targets, episodes and resets
- observation, action and reward encoding
- a lock-step bridge to a Python MAPPO learner

Scenario 1, `warrior_dummy`, is a level 1 human warrior on a training dummy. It learns when to
spend rage on Heroic Strike to maximise DPS.

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
- **The learner drives time.** In `remote` mode the world thread blocks on the learner each
  decision, and while no learner is connected. Scripted policies run without Python.

## Scenarios

Select one with `AnimusForge.Scenario` (or several with `AnimusForge.Queue`). The learner auto-starts
with `configs/<scenario>.yaml`, or `configs/class_role.yaml` when the scenario has none, and trains in
`runs/<scenario>/`.

### `warrior_dummy`

A level 1 human warrior on a training dummy. The only decision is when to spend rage on Heroic
Strike.
- **Actions (3):** no-op, queue Heroic Strike, cancel it.
- **Observation:** 9 features.

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

Eight stage scenarios -- `class_role`, `class_role_duel`, `class_role_pack`, `class_role_gauntlet`,
`class_role_companion`, `class_role_party`, `class_role_pvp`, `class_role_arena` -- each train **one policy for
every class/role** of `AnimusForge.ClassRoles` (all 18 by default). Each stage is its own scenario, so every
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
  Changing it changes the layouts, so a run cannot resume across the change.

#### Stage 1 (`class_role`): the training dummy

Every class and role maximises damage on a training dummy (tank and healer roles too: they learn their spec's
damage play in role gear):

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

The table lives in `src/Scenario/ClassRole/ClassRoleProfile.cpp`, with each spec's stat profile,
range and weapon layouts. Every episode builds a new character (the env's bot is replaced):

- **Race and level:** a random race the class allows (`playercreateinfo`), random gender, level
  1-80 (55-80 for death knights). The dummy is summoned at the bot's level, 2 yd away for melee
  specs and 20 yd for casters and hunters.
- **Talents:** one of the role's specs, then a random build spent one point at a time on a uniformly
  chosen learnable talent: the spec's tree first until it holds 51 points (the capstone row) or the
  points run out, then the other two trees. Row and prerequisite rules follow `Player::LearnTalent`.
- **Kit:** every spell of the class trainers up to the level (`trainer`/`trainer_spell`, learn-spells
  resolved), talent-gated ranks when the talent was taken, and class-quest spells trainers do not
  teach (stances, Bear Form, warlock demons, Raise Dead). Weapon and armor skills are the ones the
  race and class may have, maxed for the level. Reagents: totems, soul shards, corpse dust, flash
  powder; hunters get ammo for their ranged weapon.
- **Gear:** random level-appropriate items for every slot including both trinkets, drawn from
  every obtainable item (loot, vendors, quest rewards, crafted) the class can use and whose stats
  suit the spec (strength melee, agility melee, ranged, caster, healer or tank). Items with random
  stats roll only suffixes that suit it. A slot takes an item required at the bot's level or up to
  4 below, widening the window (9, 19, any) and falling back to lighter armor, then stat-less
  items, when nothing closer exists. Armor is plate/mail/leather/cloth by class and level; weapons
  follow the spec's layouts (two-hander, dual wield when the bot can, one-hander with shield or
  off-hand item, staff, bow/gun + stat stick, wand). No enchants, gems or relics.
- **Actions:** fixed per class, built at startup: no-op, cancel queued swing, one action per rank
  chain of every combat spell a level 80 character of any of the class's races knows (trainer,
  starting and racial spells, active talents of all three trees), casting the highest rank the bot
  knows, and one action per trinket slot. A spell counts as combat if it deals damage, applies a
  damage-relevant buff/debuff/DoT, generates resources, shapeshifts, or summons (pets, totems);
  movement, travel, crafting, pure heals and utility are left out. Every action is masked by the
  core's `Spell::CheckCast` each decision (race, level, talent, cooldown, GCD, power, stance, range,
  reagents). As on a client, no spell or trinket can be started while a cast is in its cast time (the
  core only checks that for client casts, so a bot's new cast would otherwise silently replace it).
- **Observation:** level, race and spec one-hots, health and every power type, runes, combo points,
  shapeshift form, GCD, casting, swing timers, target health and distance, attack power, spell power,
  crit, haste, hit, expertise and armor penetration; per action: known, cooldown, its aura on the
  target and on the bot, stacks; every talent's rank; points per tree.
- **Reward:** damage per decision divided by a level scale (`15 * e^(0.068 * level)`), so early
  and late levels weigh alike. Pet, guardian and totem damage counts for the owner.
- **Dummy health:** the dummy takes no damage, so its health follows a random line over the episode
  (start 20-100%, end anywhere below): execute-range abilities come up without killing it.
- **Episode info:** damage, DPS, white/special damage, level, race, spec, unspent talent points,
  equipped items, spell casts, trinket uses.
- **Baseline:** `greedy`, the first usable spell or trinket.

Learner settings come from `configs/<scenario>.yaml` (`configs/class_role.yaml` for stage 1).

#### Stage 2 (`class_role_duel`): the duel

Characters are built exactly as in stage 1 (race, level, spec, talents, kit, gear); the target is now a real
opponent:

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
- **Actions:** stage 1's actions, then: move to the opponent, move behind it, move to casting range
  (25 yd), back off 10 yd, stop, start auto-attack, send pets to attack, stop casting (the current cast
  or channel, as the client's cancel-cast), cancel form (a shapeshift the client could cancel: druid
  forms, Shadowform, Ghost Wolf, Stealth; not stances or presences), and (hunters) the 4 `call_beast`
  actions. The bot turns to face the opponent whenever it is not running. Movement is masked while
  casting, and cast-time or channeled spells while running. Stop casting and cancel form need no
  target, so they stay available between pulls in later stages.
- **Observation:** stage 1's, then: distance, bearing to the opponent, whether the bot is behind it
  and whether it faces the bot, the opponent's combat, target and casting state, the bot's movement,
  combat, stealth and auto-attack state, damage taken last step, pet out/health/attacking, elapsed
  episode time, the current cast's progress and time left (casts and channels), whether the bot is in
  a form it can cancel, and (hunters) the stable.
- **Reward:** per decision, damage dealt as a fraction of the opponent's health (x2) minus damage
  taken as a fraction of the bot's (x1), potential-based shaping toward the spec's range (melee or
  25 yd), +0.5 for a stealth-only opener from stealth, and a small time cost. Casting: every cast-time
  spell that does not finish (stopped, interrupted, pushed into death) costs 0.05 per second of cast
  time already spent, and every one that finishes while the bot is in combat earns 0.02 per second of
  its cast time. Nothing forces a cast to finish; cutting one short stays the policy's call when
  something else is worth more. Channels are paid by their ticks. Later stages keep this term.
  On the kill: +2, plus
  up to +3 for the time left in the episode, plus up to +2 for the share of the bot's health it did
  not lose. Death: -3. The episode ends on the kill or the bot's death.
- **Episode info:** stage 1's columns, then killed, died, time to kill, damage taken, health left,
  stealth openers, whether a pet was out, the opponent's entry, casts completed, casts cancelled and
  seconds of cast time wasted.

**Bootstrapping:** every stage after the first has `init_from: runs/{base_run}<previous stage>/best.pt`
(`class_role_duel` seeds from `runs/class_role/best.pt`). A run with nothing to resume seeds its networks from the
previous stage's (`animus/bootstrap.py`), layout by layout (class/roles are matched by name): each layout's
earlier features keep their places in its wider adapter (new inputs start at zero) and its earlier actions keep
their logits (new actions start near zero); the trunk is copied; the critic's state encoder and value head start
fresh because the global state and reward differ. Queue the stages in order.

#### Stage 3 (`class_role_pack`): packs

Stage 3. The duel's characters against a pack instead of a single opponent:

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
  the bot's (x1), approach shaping toward the nearest enemy, +0.5 per kill, +0.3 per interrupt (a
  cast that stops after the bot's interrupt, stun, silence, fear or polymorph), +0.5 for a stealth
  opener. Clearing the pack: +2, up to +3 for the time left, up to +2 for the health kept. Death -3.
  The episode ends when the pack is cleared or the bot dies.
- **Episode info:** the duel's (killed = cleared), then kills, interrupts, pack size, linked.

#### Stage 4 (`class_role_gauntlet`): the gauntlet

Stage 4: sustained combat.

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
  ends the episode.
- **Episode info:** the pack stage's, then pulls cleared, food used, drinks used, sustain casts.

Give the gauntlet long episodes (`AnimusForge.EpisodeSeconds` of several minutes).

#### Stage 5 (`class_role_companion`): the companion

Stage 5: the gauntlet fought beside an owner, as a companion fights beside a player.

- **Owner:** a scripted player bot of a random class (one a character of that level can be) within 2
  levels of the companion, dressed like the companion: a random damage spec and build, its trainer
  spells and level-appropriate gear. It gets the companion's faction so either faction's races can be
  paired. Between pulls it wanders near the arena and recovers health and mana; each pull spawns around
  it and it walks in after 1.5-5 s, fights the enemy attacking it (else the nearest) in melee and casts
  one of its own damage spells every 2-4 s. Linked packs join in on whoever their engaged member fights.
- **Actions:** the gauntlet's, then follow the owner, assist (target the owner's target), guard (target an
  enemy attacking the owner), and one "cast on the owner" action per single-target heal.
- **Observation:** the gauntlet's, then the owner's presence, health, mana, distance, bearing, combat,
  movement, level difference and class, how many enemies attack it, which enemy slot it attacks, which
  enemies attack it, and each owner heal's known/cooldown.
- **Reward:** the gauntlet's, plus, by role:
  - everyone: the owner's damage taken (fraction of its health; x1 for damage dealers, x2 for tanks and
    healers), -0.01 per decision in combat while the owner is not, a small bonus for staying within 12 yd
    out of combat and a penalty beyond 25 yd, -6 if the owner dies;
  - tanks: +0.01 per enemy attacking the tank, -0.02 per enemy attacking the owner, per decision (and
    half of the gauntlet's damage-taken penalty back);
  - healers: effective healing on the owner (x2, fraction of its health; the core's heal hook reports the
    health actually gained, so overhealing earns nothing);
  - damage dealers and healers: -0.01 per enemy attacking them, per decision.
  The episode ends when the companion or the owner dies.
- **Episode info:** the gauntlet's, then the owner's class, whether it died, its damage taken, the
  companion's healing on it, and enemy-decisions spent on the companion and on the owner.

#### Stage 6 (`class_role_party`): the party

Four learned seats -- a tank, a healer and two damage dealers of random classes that can fill the role, all at
one level -- and the companion stage's scripted owner as the fifth player, against dungeon-like pulls. Every seat
plays with the same policy and sees the other three.

- **Pulls:** 2-4 creatures, each elite half the time, up to 2 levels above the party, pull after pull, spawning
  around the owner. The owner waits 4-7 s so the tank can pull, then attacks the tank's target.
- **A real group:** every episode the owner (as leader) and the four seats form a core `Group`, so party buffs,
  auras, party-wide heals and every "party member" check work as in play. It is a sim group
  (`Group::SetSimGroup`): it lives only in memory -- no group or member rows, no character cache entries, and
  joining, leaving or disbanding never touches instance binds or homebind timers -- so rebuilding it every
  episode writes nothing to the database. It is disbanded before its members are replaced.
- **Actions:** the companion stage's, then follow the tank, then per teammate assist, guard, and one "cast on
  it" action per single-target heal.
- **Observation:** the companion stage's, then living party size, the most hurt ally's health, whether a
  living tank and healer are present, and per teammate presence, health, mana, distance, bearing, combat, role,
  class, attackers, target slot and which enemies attack it.
- **Reward:** per seat, the companion stage's (its own damage, threat and survival, the owner's), plus per
  teammate: its damage taken (not for a tank teammate; x0.5 for a damage dealer, x1 otherwise), effective
  healing on it for healers (x2), -0.02 per enemy on a non-tank teammate per decision for tanks, and -3 when it
  dies. Kills and clears are shared by the party. A tank is not charged for fighting before the owner joins.
  The episode ends when the owner dies or every seat has.
- **Episode info:** per seat, the companion stage's, then teammates died, teammate damage taken, its healing
  on teammates, enemy-decisions spent on non-tank teammates, and the seat.

#### Stages 7 and 8 (`class_role_pvp`, `class_role_arena`): PvP

One-on-one against a player. They keep stage 6's layouts (so they seed from it), but there are no pulls, owner
or teammates: those observations stay zero and those actions masked. Both players get opposing player factions
and the PvP flag, which players need to attack each other.

- **`class_role_pvp` (stage 7):** a scripted enemy player at the bot's level (within 1), of a random class and
  role (damage 60%, tank 20%, healer 20%) with that role's spec, talents, kit and gear, spawned 40-50 yd away
  facing a random way. It closes in after up to 3 s: melee specs fight in melee, ranged specs hold 10-30 yd and
  cast, healers heal themselves below 60%.
- **`class_role_arena` (stage 8), self-play:** two learned seats of random classes and roles at one level in the
  same env, the second spawned 40-50 yd from the first. Both are played by the same policy, so every fight is
  training data for both sides, across class matchups.
- **Observation:** stage 6's, then the opponent's class, role, level difference, mana, rage/energy/runic
  power, whether it is crowd-controlled, stealthed, has a pet out or is casting a heal, whether the bot is
  stunned/feared, rooted or silenced, and whether the opponent is a learned agent.
- **Reward:** the duel's (damage dealt and taken, closing in, stealth openers, casts, a fast kill with health
  kept, death). The episode ends when either player dies.
- **Episode info:** stage 6's, then won, opponent class and opponent role.

`configs/class_role_pvp.yaml` scores against the `fight` baseline. `configs/class_role_arena.yaml` has no
baseline or plateau stop: against itself a policy's score does not track progress -- judge an arena model with
`animus.evaluate` on a `class_role_pvp` sim.

### Training every model: `AnimusForge.Queue`

`AnimusForge.Queue` lists scenarios to train one after another (it overrides
`AnimusForge.Scenario`):

```
AnimusForge.Queue = "class_role, class_role_duel, class_role_pack, class_role_gauntlet, class_role_companion, class_role_party, class_role_pvp, class_role_arena"
```

Each scenario runs with its auto-started learner until the learner finishes -- its evaluation score
plateaus or it reaches `total_env_steps` (see
[Evaluation and plateau stopping](#evaluation-and-plateau-stopping)) -- and exits cleanly; the sim then
tears the scenario down and starts the next one. Every run trains in `runs/<scenario>/`; export its models by
hand when you want them (see [Export](#export)). After a server restart, finished scenarios' learners (`runs/<scenario>/finished.json`) exit immediately and the
queue moves on to the first unfinished one. Change the list (or its order)
in the config and restart to retrain or skip models. A learner that crashes stops the queue at that
scenario: the sim waits for a learner, as it does without a queue.

## Enabling

The module is picked up automatically from `modules/` (it has a `src/` directory). Rebuild the
worldserver.

This module is self-contained: it builds against the forge core on its own and shares no code with any other
module. Nothing else belongs in a forge build -- in particular mod-animus, which plays exported models on a stock
AzerothCore, is never built into the forge core. If another module's directory sits in `modules/`, disable it in
the forge build:

```
cmake . -DMODULE_MOD-ANIMUS=disabled
``` Settings live in `mod_animus_forge.conf`; `conf/mod_animus_forge.conf.dist` documents
every key.

The build installs only the `.dist` file, and AzerothCore never reads a `.dist` directly. Copy it
once, next to the installed template:

```
cp env/dist/etc/modules/mod_animus_forge.conf.dist env/dist/etc/modules/mod_animus_forge.conf
```

Until then every `AnimusForge.*` key logs "Missing property" and falls back to its default.

| Key | Default | Purpose |
|---|---|---|
| `AnimusForge.Enable` | `1` | `0` turns the module off entirely (no bots, no socket, hooks return immediately) |
| `AnimusForge.Scenario` | `warrior_dummy` | Scenario started automatically when the server starts |
| `AnimusForge.Envs` | `64` | Parallel envs (one instance map each) |
| `AnimusForge.DecisionTicks` | `1` | World ticks per decision (1 = every 50 ms of game time) |
| `AnimusForge.EpisodeSeconds` | `60` | Game-time episode length |
| `AnimusForge.Policy` | `remote` | `remote`, `random`, or a scenario's scripted policy (`never_hs`, `hs_at_threshold`) |
| `AnimusForge.HsRageThreshold` | `15` | Rage threshold for `hs_at_threshold` |
| `AnimusForge.ReportEpisodes` | `256` | Log mean episode stats every N episodes |
| `AnimusForge.Socket` | `/tmp/animus-forge.sock` | Learner socket path |
| `AnimusForge.Learner.AutoStart` | `1` | Start the Python learner automatically (remote policy) |
| `AnimusForge.Learner.WorkDir` / `Python` / `Config` / `LogFile` | derived | Where and how the learner runs (see Training) |
| `AnimusForge.Learner.CleanRun` | `""` | Run id: a new id archives each run and trains from scratch |
| `AnimusForge.Learner.Args` | `""` | Extra learner arguments for every scenario, e.g. `--set total_env_steps=5000000` |
| `AnimusForge.ClassRoles` | `""` | Class/roles the class/role scenarios play; empty = all 18 |
| `AnimusForge.Arena.*` | Old Hillsbrad entrance | Dungeon map and position for the arena |

Any key can also be set from the environment, e.g. `AC_ANIMUS_FORGE_SCENARIO=warrior_dummy`.

For best throughput set `MapUpdate.Threads` to the number of physical cores.

### Docker: use the dev server

The auto-started learner needs Python, torch and the GPU wherever the worldserver runs. The stock
`ac-worldserver` image has none of them, so train in `ac-dev-server`: it bind-mounts this
repository, and the local `docker-compose.override.yml` gives it `/dev/kfd` and `/dev/dri` and
removes its host ports. Those ports would clash with `ac-authserver` and `ac-worldserver`, and the
sim does not listen on them anyway.

```
docker compose --profile dev up -d ac-dev-server        # dev server + database only
docker compose exec ac-dev-server bash
```

One-time Python setup, inside the container. The venv lives on the bind mount, so it survives
container rebuilds. It must be created inside the container, not on the host: it points at the
container's Python.

```
sudo apt-get update && sudo apt-get install -y python3-venv
cd /azerothcore/modules/mod-animus-forge/python
python3 -m venv .venv
.venv/bin/pip install torch --index-url https://download.pytorch.org/whl/rocm6.4
.venv/bin/pip install -e '.[dev,tensorboard]'
.venv/bin/python -c "import torch; print(torch.cuda.is_available(), [torch.cuda.get_device_name(i) for i in range(torch.cuda.device_count())])"
```

Only the discrete RX 7900 XTX is passed into the container (by PCI path, so it survives device
renumbering between boots), and `configs/warrior_dummy.yaml` trains on it (`train_device: cuda`).
The torch check above should list a single device. Then build and run the worldserver in the
container as usual; the learner starts by itself.

## Baselines (no Python)

Set a scripted policy and read the `Episodes N (mean): ...` lines in the worldserver log:

```
AnimusForge.Policy = "never_hs"          # white swings only
AnimusForge.Policy = "hs_at_threshold"   # try HsRageThreshold = 15, 30, 60
AnimusForge.Policy = "random"
```

These are the DPS numbers the learner has to match or beat. They double as a mechanics check. With
`never_hs`, `white_hits` per episode should be roughly `EpisodeSeconds / weapon speed` (minus
misses and dodges), and `special_hits` should be 0.

## Training

With `AnimusForge.Policy = "remote"` and `AnimusForge.Learner.AutoStart = 1` (the defaults), the
worldserver starts the learner itself once the envs are built, for the scenario in
`AnimusForge.Scenario`:

```
<WorkDir>/.venv/bin/python -u -m animus.train --config configs/<scenario>.yaml \
    --socket <AnimusForge.Socket> --resume-latest
```

- **WorkDir** defaults to this module's `python/` directory; each `AnimusForge.Learner.*` key
  overrides one part.
- **Output** is appended to `animus-learner.log` in `LogsDir`.
- **An exit** is logged in the worldserver log, with the exit code.
- **Restarts:** `--resume-latest` continues from `runs/<run_name>/latest.pt`, so a server restart
  resumes training and appends to `metrics.csv` instead of starting over.
- **Clean runs:** set `AnimusForge.Learner.CleanRun` to an id (e.g. a date) to train from scratch. The
  first time each scenario starts under a new id, its `runs/<scenario>/` is moved to
  `runs/_archive/<scenario>-<time>/` (`--clean-run <id>`); the new directory records the id, so restarts
  under the same id resume the clean run. A new id starts another one.
- **Overrides:** `AnimusForge.Learner.Args` appends arguments to every learner, typically
  `--set key=value` (dotted keys for sections: `--set eval.episodes=64`), to change config values for a
  whole queue without editing YAML -- e.g. a short pass over a curriculum before the long run.
- **Shutdown:** closing the socket makes the learner save a checkpoint and exit; it is interrupted
  after 10 s if it has not.

### Evaluation and plateau stopping

Training curves are noisy when every episode rolls a new character, so the class/role configs score the
networks on **seeded evaluation episodes** as they train (`eval:` in the YAML, `animus/evaluation.py`):

- **Seeds:** the learner switches the sim to evaluation (protocol `MODE`). Every env resets, and episode
  seed index *i* (0 to `eval.episodes - 1`) is built right after the world thread's random numbers are
  reseeded from (`eval.seed`, *i*): the same race, level, spec, talents, gear, opponents and spawn points
  every evaluation, whatever the env count. Combat rolls stay random, so scores are averages, not replays.
  Afterwards the learner switches back and training resumes from fresh episodes.
- **Policy:** argmax actions (`eval.deterministic`). The score is the mean episode return -- the scenario's
  own reward -- so it measures what training optimises and compares checkpoints of one scenario.
- **Baseline:** `eval.baseline` (`greedy` for stage 1, `fight` for later stages) is played by the sim on the
  same seeds once per run and cached in `eval_baseline.json`.
- **When:** before training (`eval.at_start`, which also shows what a stage's warm start is worth), every
  `eval.every_env_steps` (20M), and at `total_env_steps`.
- **Output:** the log prints the score, the best so far and a table of score and key episode stats
  (`eval.report`) overall and per level band (1-20, 21-40, 41-60, 61-80), learner next to baseline.
  `eval.csv` has one row per evaluation, `eval.jsonl` the full tables, TensorBoard `eval/*` and
  `eval_<band>/*`.
- **Best model:** each new best score saves `best.pt` and publishes that actor; the next curriculum stage
  seeds from `runs/<previous stage>/best.pt` (its `latest.pt` if there is no best).
- **Plateau:** with `plateau.patience` set, the run stops once that many evaluations in a row fail to beat
  the best score by `plateau.min_improvement` (a fraction of it) or `plateau.min_improvement_abs`,
  whichever is larger, and not before `plateau.min_env_steps`. The class/role configs use 5 evaluations
  (100M env steps), 2% and 40M; `total_env_steps` stays the upper bound. `finished.json` records why a
  run stopped.

To run the learner yourself, set `AnimusForge.Learner.AutoStart = 0`, then from `python/`:

```
pytest                                                    # protocol, GAE and trainer tests
python -m animus.train --config configs/warrior_dummy.yaml
python -m animus.evaluate --checkpoint runs/class_role/best.pt --episodes 128 --seed 1000 --baseline greedy
```

A hand-started learner can start before or after the worldserver; it retries until the socket
exists. Disconnecting (Ctrl+C) is safe: the sim waits for the next learner and resets every env when
one connects.

Each run writes `config.yaml`, `spec.json`, `metrics.csv`, TensorBoard logs (if installed) and
checkpoints to `runs/<run_name>/`; with evaluation also `eval.csv`, `eval.jsonl`, `eval_baseline.json`,
`best.pt` and, once done, `finished.json`.

## Export

Training writes checkpoints to its run directory and nothing else: models are never published or copied anywhere
automatically. Export a checkpoint's actor to `.amdl` models by hand:

```
python -m animus.export --checkpoint runs/class_role_duel/best.pt --out exported/class_role_duel
```

Copying exported models to a server that plays them is also done by hand.

It writes one model per layout: `warrior_dps_duel.amdl`, `priest_heal_duel.amdl`, ... (a single-layout scenario
such as `warrior_dummy` writes `warrior_dummy.amdl`). Each is the layout's input adapter, the shared trunk and the
layout's action head, in the plain MLP format below.

The file format is documented at the top of `animus/export.py`. `tests/test_export.py` checks that
the exported network reproduces the torch actor's greedy actions.

A class/role model's inputs and outputs are defined by `src/Scenario/ClassRole/ClassRoleLayout.*` (what each
observation feature and action is, and where) and `SeatEncoder.*` (how observations, masks and actions are read
from and applied to the world); the scenario only describes each seat's situation (`ClassRoleScenario::ViewSeat`:
enemies, owner, teammates, opponent, pull timing). Each exported class/role model gets its layout manifest beside
it (`<model>.json`, e.g. `warrior_dps_duel.json`): the stage, class/role, sizes, block offsets, every action and
talent. Anything that plays the model must build exactly that manifest for it; a different manifest means the
model would read its observations and actions as something else.

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

1. Implement `Animus::Scenario` (`src/Scenario/Scenario.h`): `Setup` builds bots and targets once,
   `Reset` restarts an episode in place, and `Observe` / `ApplyActions` / `Reward` define the MDP.
   `IsTerminal` is optional.
2. Add one row to the registry in `src/Scenario/Scenario.cpp`.
3. Set `AnimusForge.Scenario` to its name and write a learner config under `python/configs/`.

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

Bots also reuse a fixed pair of player GUIDs per env, because the core keeps some per-GUID state
(instance bind storage) for the life of the server.

## Known limits

- **Cooldowns and the GCD** use the game clock, which the core is being moved onto the sim tick
  (separate work). `warrior_dummy` does not depend on it: Heroic Strike has no cooldown and no GCD.
- **Scripted owner and PvP opponent:** the companion and party stages' owner and stage 7's enemy player are
  scripts; the party's other members and stage 8's opponent are learned.
- **Throughput** in `remote` mode is bounded by one Python round trip per decision for all envs.
  Raise `Envs` until the learner, not the world thread, is the bottleneck.
