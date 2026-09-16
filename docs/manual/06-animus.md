# 6. Animus

Animus (`mod-animus`, repository `Moloch17/animus`) brings the forge's trained models to an ordinary AzerothCore realm
with real clients. It offers two features:

- **Class/role companions.** A player summons up to four characters of any class and role at their level. The
  companions join the player's party and play their class/role models in combat.
- **The stage viewer.** A game master runs any curriculum stage exactly as the forge trains it, in their own instance,
  and watches the seats play their models, a scripted baseline, or random actions.

## 6.1 Constraints

- **Stock core only.** mod-animus uses public AzerothCore APIs and animus-lib. It never calls forge-only APIs and
  installs no `CoreHooks`. Anything that would need one belongs behind a seam in animus-lib.
- **No external dependencies.** Inference is the hand-written C++ MLP in animus-lib (`MlpPolicy`), not ONNX or
  LibTorch.
- **The model makes every combat ability choice.** No hand-written rotations are mixed in.
- **Observations are exactly the training observations.** Companions and stage seats go through the same
  `SeatEncoder` and blocks as training seats. Features only training could supply, such as the share of an episode's
  time limit left, aren't observed. What live play can supply is filled in the same way: a companion party counts
  combat time, pull time and time into its own episodes.
- **Never built into the forge core.** A forge build disables it (`-DMODULE_MOD-ANIMUS=disabled`).

## 6.2 Source map

| File | Role |
|---|---|
| `src/animus_loader.cpp` | Registers animus-lib's scripts, then `AddSC_animus` |
| `src/Hooks/AnimusScripts.cpp` | `.animus` commands (game master, not from the console), `WorldScript` (config load, update, shutdown), `PlayerScript::OnPlayerLogout`, `UnitScript::DealDamage` |
| `src/AnimusMod.{h,cpp}` | Module root: config, `ModelLibrary`, layouts, parties by owner, viewers by game master |
| `src/AnimusConfig.{h,cpp}` | Settings and the viewer's `StageSettings` |
| `src/Companion/CompanionParty.{h,cpp}` | One player's companions |
| `src/Viewer/StageViewer.{h,cpp}` | One game master's stage |
| `conf/mod_animus.conf.dist` | Every key, documented |
| `mod-animus.cmake` | Installs `models/*.amdl`, clones and requires animus-lib |
| `models/` | Where you may put models for the install step to copy |

## 6.3 Installing

1. Put the module in a stock AzerothCore's `modules/`. Configuring clones animus-lib into `modules/mod-animus-lib` if
   it is missing. Build both the same way (static, the default, or both dynamic).
2. Rebuild and install the worldserver.
3. Copy `conf/mod_animus.conf.dist` to your config directory as `mod_animus.conf`.
4. Put the models in place (6.4).

The install step copies `models/*.amdl` to `ANIMUS_MODELS_INSTALL_DIR` (default `<install prefix>/data/animus`).
**It doesn't copy the `.json` manifests**, which must sit beside their models, so copy those by hand, or put everything
directly into `Animus.ModelDir`.

## 6.4 Models

`Animus.ModelDir` (default `animus`) is resolved against `DataDir` when relative, and used as-is when absolute.

- In Docker, the defaults line up: `AC_DATA_DIR` is `/azerothcore/env/dist/data`, and installing from `ac-dev-server`
  writes into the shared client-data volume.
- The stock `worldserver.conf` sets `DataDir = "."` (the worldserver's working directory). Either set `DataDir` to
  `<install prefix>/data`, configure with `-DANIMUS_MODELS_INSTALL_DIR=<DataDir>/animus`, or give `Animus.ModelDir` an
  absolute path.

For each layout it needs, the module looks for `<ModelDir>/<class>_<role><stage suffix>.amdl` and the `.json` manifest
beside it. `ModelLibrary` accepts a model only if the manifest file is exactly the manifest this server builds for that
layout (same stage, class/role, sizes, block offsets, actions, talents), and the `.amdl` header matches the layout's
dimensions. Failures are logged once and cached. Models load on first use, and again after `.reload config`, which
resets the library.

Where the models come from: `forge export <stage>` in the forge writes both files per class/role into
`AnimusForge.ModelDir`. Copy them to the realm. Build the realm with an animus-lib revision whose blocks, stages and
catalogs match the one the model trained with, or the manifests won't match. A layout's manifest doesn't depend on
which other class/roles the run trained (`AnimusForge.ClassRoles`), only on its own stage, class/role and blocks.

## 6.5 Class/role companions

### Commands

All `.animus` commands need game master security and don't work from the console.

| Command | Effect |
|---|---|
| `.animus summon <race> <class> <role>` | Build a companion of that race, class and role at your level (`human priest heal`, `orc warrior tank`). It joins your party |
| `.animus list` | Your companions, their class/roles and levels, whether their models are loaded, and whether they are waiting for you to land |
| `.animus dismiss` | Remove all your companions |

### Summoning

| Argument | Accepted |
|---|---|
| `race` | `human`, `dwarf`, `nightelf`, `gnome`, `draenei`, `orc`, `undead` (or `forsaken`), `tauren`, `troll`, `bloodelf` |
| `class` | `warrior`, `paladin`, `hunter`, `rogue`, `priest`, `deathknight` (or `dk`), `shaman`, `mage`, `warlock`, `druid` |
| `role` | `dps` (or `damage`), `tank`, `heal` (or `healer`) |

Names ignore case, underscores and hyphens (`night_elf`, `NightElf`). The class/roles are the forge's 18 (4.3).
`AnimusMod::Summon` and `CompanionParty::Add` refuse when:

- the module is disabled,
- a name isn't a race, class or role (the reply lists the valid ones),
- the class doesn't have the role (`mage tank`), or the race can't be the class (`orc paladin`),
- the race belongs to the other faction,
- you are on a flight path or a vehicle (`BotFactory::IsAway`),
- you are in a battleground or arena, which only takes queued players, or between maps (`BotFactory::CanJoin`),
- you already have four companions,
- you are in a group you don't lead, or the group is full.

You can summon in the open world, in a dungeon or raid instance, and on a boat, zeppelin or elevator. Otherwise:

1. **Layout.** `LayoutFor(profile)` builds and caches the class/role's layout at `Animus.Curriculum.Stage` (default
   `stage5_party`). The first build of a class/role's assets takes a few seconds and stalls the world thread.
2. **Bot.** `BotFactory::Create` makes a bot named `Animus<n>` with account `0x7E000000 + n`, of the race you named
   and a random gender, at your level or the class's first level if that is higher (a death knight is at least 55),
   and places it beside you (`PlaceNear`).
3. **Character.** Exactly as the forge builds a seat: `InitTalentForLevel` on your map, `SeatCharacter::Configure`
   (a random spec of the role, its standard talent build and glyphs, trainer spells, gear with enchants and gems;
   non-PvP) and `PrepareFighter` (no XP, a warrior's stance, a hunter's stable offer).
4. **Group.** If you have no group, one is created with you as leader. The companion is added as a normal member.
5. **Supplies.** `Restock`: potions, bandages, stones and flask, plus food and drink for layouts with the gauntlet
   block, stocked after joining so a warlock in the group hands out healthstones.
6. If its model isn't available, the reply says so. The companion then only follows you.

### Where companions go with you

Companions follow you through every loading screen (`CompanionParty::Update`, `UpdateMember`):

- **Another map or an instance.** As soon as you arrive, each companion is brought to you, into the same instance with
  your group's difficulty, in or out of combat. Entry requirements don't apply to it. Only an instance that refuses
  anyone at that moment (full, or a raid encounter in progress) keeps it out until it takes it.
- **Transports.** A companion boards a transport when you do and steps off when you step off, through a map change
  too.
- **Flight paths and vehicles.** While you are on one, the companions leave the world (they are *parked*). When you
  are off it, they come back beside you, wherever you landed.
- **Battlegrounds and arenas.** Companions wait where you left them and rejoin you when you come back.
- **Other teleports** a companion starts itself (a summoning spell, a transport changing maps) complete as a client
  would acknowledge them.

A companion never answers an instance's lock warning, so it is only ever bound to an instance temporarily.

### The party as an env

`CompanionParty` stands in for a training env. **You are the owner, the companions are the seats, and whatever is
fighting any of you is the current pull.** Every world update:

**Tracking the pull** (`UpdatePull`). Enemies are units that attack you, a companion, or any of your pets, plus the
units you and the companions attack. Only living, valid attack targets on your map count.

- A new enemy takes a free slot (up to 4). Once all slots are taken, it replaces a slot whose enemy is dead or gone.
- The first enemy of a pull starts a new **episode** if none has started yet, or if the party has been quiet for at
  least 20 s (`NEW_EPISODE_QUIET_MS`). A new episode resets pulls cleared, restarts the episode clock the models
  observe, and restocks every living companion, just as training starts every episode with full bags.
- The pull ends once no enemy in it is alive and still fighting (dead, gone, or evaded home). Then pulls cleared is
  incremented, slots and target selections reset, and the quiet timer starts.

**Each companion** (`UpdateMember`):

- **Dead.** It accepts a pending resurrection at once, as a client would. Otherwise, once the party is quiet, you are
  alive and out of combat, and it has been dead 10 s, it stands up with half health.
- **Out of combat.** More than 100 yd away: teleport to you. With a model and more than 30 yd away: run back behind
  you. Without a model: stay within 6 yd.
- **Deciding.** Every `Animus.Curriculum.DecisionMs` (100) of accumulated update time, if its model is available,
  the companion decides.

**A decision** (`Decide`) mirrors a training seat:

1. Track combat start (the combat-time feature).
2. Fill the last-step features as the forge's reward step would: damage dealt since the last decision divided by the
   level's damage scale, damage taken divided by max health (both from the module's `DealDamage` hook, stored in
   atomics because map threads write them), and the power change.
3. Choose the target: the selected enemy slot, or the nearest living enemy (which becomes the selection).
4. Build the `SeatView`: you as the owner, the other companions as teammates, the enemy slots, pull timing, the
   episode clock (time since the episode started / 5 min), supplies, stable. Run `SeatEncoder::Observe`.
5. With no target and a layout that can't act without one, stop.
6. `MlpPolicy::Decide` picks the highest-scoring allowed action. `SeatEncoder::Apply` performs it as a client would:
   casts, item uses, movement, target selection, pet commands, heals and resurrections on party members. A called
   hunter beast is summoned.

**Logout and dismissal.** When you log out, your companions are removed (`OnPlayerLogout`). A companion is removed from
the group and logged out without saving (`BotFactory::Destroy`). Because a stock core has no sim groups, companions'
group membership is written to the database like any member's and removed on dismissal. Rows left behind by a crash
are cleaned up by the core at startup (group members without a character).

## 6.6 The stage viewer

### Commands

| Command | Effect |
|---|---|
| `.animus stage list` | Every stage with its summary and arenas |
| `.animus stage start <stage> [policy] [arena]` | Teleport to the stage's spawn point and run it there |
| `.animus stage status` | The stage you watch: its episode, arena, seats and models |
| `.animus stage reset` | End the current episode and start a new one at the next decision |
| `.animus stage stop` | Remove the stage |

Policies: `model` (each seat's exported model; the default `Animus.Stage.Policy`), `random`, `greedy`, `fight`.

### Lifecycle

**Begin** (`StageViewer::Begin`). The stage, policy and arena are checked. A named arena forces that arena for every
episode; otherwise arenas are drawn by weight. The spawn map must be instanceable. GM mode is turned on as `.gm on`
would (the reply says so, and it stays on after the stage stops), and the game master is teleported to
`Animus.Stage.SpawnPoint.*` (default: the forge's own spawn point, the Old Hillsbrad Foothills entrance),
which creates or enters an instance of that map. Starting a stage replaces the one you already watch. At most
`Animus.Stage.MaxViewers` (4) stages run at once, and each takes the lowest free env id, which keeps bot accounts and
names apart.

**Travelling.** The viewer waits up to 60 s for you to arrive. If you land anywhere other than an instance of the spawn
map, the stage ends.

**Build.** A `StageScenario` is created with the viewer's `StageSettings`:

- one env, the viewer's env id, `Animus.Stage.*` decision interval, episode length, class/roles, spawn point and level
- the `Animus.Curriculum.` tuning prefix
- report means over each episode
- no layouts directory

A baseline policy is checked against the stage. The arena is forced if one was named. The env pool is told to build
env 0 **in your instance** (`EnvPool::PlaceEnv`), then `Setup`, `ResetAll` and `PoolRegistry::Register`, so animus-lib's
own hooks count the seats' damage and healing. The first build of a class/role's assets stalls the world for a few
seconds.

**Running.** Each world update:

- if you left the instance or logged out, the stage ends,
- `AdvanceClock(diff)`,
- every `Animus.Stage.DecisionMs` of accumulated time, **one** decision. A long world update doesn't queue up extra
  decisions:
  1. `Collect()` (rewards, episode end, auto-reset, observe). When an episode ended, a chat report shows the arena,
     length and whether it ended or hit the time limit, and per present seat: level, class/role, damage and DPS, damage
     taken, kill, died, health left and total reward (the sum of the `reward_*` columns),
  2. a requested reset calls `ResetAll()`,
  3. actions: `model` asks `ModelLibrary` for each present seat's layout and calls `Decide`. A seat with a missing or
     refused model does nothing, and you are told once per model. Any other policy calls
     `EnvPool::ChooseLocalActions`,
  4. `ApplyActions()`.

**Stop.** Unregister the pool, tear down the env (seats, owner, enemy players, creatures, group). It is safe to call
more than once.

### Watching a stage as it was trained

- **GM mode is on.** `.animus stage start` turns it on, so the stage's creatures and enemy players, which choose their
  targets among the seats and the owner, leave you alone. Turning it off in the middle of a pull lets them attack you.
  Your presence changes nothing the seats observe.
- **Match the settings** to the forge run: `Animus.Stage.DecisionMs`, `EpisodeSeconds`, `Level` and `SpawnPoint.*`
  like the run's `AnimusForge.*` keys, and `Animus.Curriculum.*` like the run's `stage.json` `"tuning"`.
  `Animus.Stage.ClassRoles` only chooses which characters appear. Unlike the forge's list, it doesn't change what the
  models were trained on.
- **Nothing is saved.** Seats, owners and enemy players have no character rows. The rows a stock core writes for their
  instance binds and groups are removed when they go.

## 6.7 Differences from the forge

| Aspect | Forge | Stock core with mod-animus |
|---|---|---|
| Clock | Fixed ticks of `DecisionMs` game time | Real time. Decisions happen when `DecisionMs` of update time has accumulated |
| Bot sessions | Sim sessions: no logout or bind writes | Normal: a few rows written on logout and instance bind, bind rows deleted on destroy |
| Party group | Sim group, memory only | Normal group rows, removed on dismissal or stop |
| Evaluation seeds | Reproducible (`rand_seed`) | Not available (`SeedRandom` returns false) |
| Packets | None | Everything is visible to real clients: movement, casts, combat log |
| Episodes | Defined by the stage | Stage viewer: defined by the stage. Companions: a pull after 20 s of quiet starts one |
| Owner | Scripted player | You |
