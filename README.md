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

Select one with `AnimusForge.Scenario`. The learner auto-starts with `configs/<scenario>.yaml`.

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

## Enabling

The module is picked up automatically from `modules/` (it has a `src/` directory). Rebuild the
worldserver. Settings live in `mod_animus_forge.conf`; `conf/mod_animus_forge.conf.dist` documents
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
- **Shutdown:** closing the socket makes the learner save a checkpoint and exit; it is interrupted
  after 10 s if it has not.

To run the learner yourself, set `AnimusForge.Learner.AutoStart = 0`, then from `python/`:

```
pytest                                                    # protocol, GAE and trainer tests
python -m animus.train --config configs/warrior_dummy.yaml
python -m animus.evaluate --checkpoint runs/warrior_dummy/latest.pt --episodes 256
```

A hand-started learner can start before or after the worldserver; it retries until the socket
exists. Disconnecting (Ctrl+C) is safe: the sim waits for the next learner and resets every env when
one connects.

Each run writes `config.yaml`, `spec.json`, `metrics.csv`, TensorBoard logs (if installed) and
checkpoints to `runs/<run_name>/`.

## Export for mod-animus

`mod-animus` runs trained actors in a normal worldserver. Export a checkpoint's actor to its `.amdl`
format:

```
python -m animus.export --checkpoint runs/warrior_dummy/latest.pt --out ../../mod-animus/models/warrior_dummy.amdl
```

The file format is documented at the top of `animus/export.py`. `tests/test_export.py` checks that
the exported network reproduces the torch actor's greedy actions. If a scenario's observations or
actions change, update mod-animus's copy of the encoding (for `warrior_dummy`,
`mod-animus/src/Companion/WarriorDummyPolicyIO.*`).

## Wire protocol

`src/Bridge/Protocol.h` is the reference; `python/animus/protocol.py` mirrors it. Messages are
little-endian: `HELLO` → `SPEC` → (`STEP` → `ACT`)* → `CLOSE`. A `STEP` carries, for every env:
- observation, critic state and action mask
- reward
- `done` / `terminated`
- the final observation and state of an episode that just ended (for truncation bootstrapping)
- episode totals

## Adding a scenario

1. Implement `Animus::Scenario` (`src/Scenario/Scenario.h`): `Setup` builds bots and targets once,
   `Reset` restarts an episode in place, and `Observe` / `ApplyActions` / `Reward` define the MDP.
   `IsTerminal` is optional.
2. Add one row to the registry in `src/Scenario/Scenario.cpp`.
3. Set `AnimusForge.Scenario` to its name and write a learner config under `python/configs/`.

The protocol and learner are shape-generic. A multi-agent scenario sets `AgentsPerEnv > 1` and
provides a real global `State`, and MAPPO's shared actor and centralized critic handle it.

## Known limits

- **Cooldowns and the GCD** use the game clock, which the core is being moved onto the sim tick
  (separate work). `warrior_dummy` does not depend on it: Heroic Strike has no cooldown and no GCD.
- **Throughput** in `remote` mode is bounded by one Python round trip per decision for all envs.
  Raise `Envs` until the learner, not the world thread, is the bottleneck.
