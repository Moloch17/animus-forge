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
- **Bots are sessionless players.** See `src/Bot/BotFactory.cpp`. They are deliberately kept out
  of `WorldSessionMgr`: a socketless session registered there is deleted, and its player saved,
  on the next update.
- **Damage is measured in `UnitScript::DealDamage`**, before the victim's AI runs.
  `npc_training_dummy` zeroes damage in `DamageTaken`, so `OnDamage` would only ever see 0.
- **The learner drives time.** In `remote` mode the world thread blocks on the learner each
  decision, and while no learner is connected. Scripted policies run without Python.

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
| `AnimusForge.Arena.*` | Old Hillsbrad entrance | Dungeon map and position for the arena |

Any key can also be set from the environment, e.g. `AC_ANIMUS_FORGE_SCENARIO=warrior_dummy`.

For best throughput set `MapUpdate.Threads` to the number of physical cores.

### Docker

The learner runs on the host, so the socket has to be on a bind mount. Put this in a
`docker-compose.override.yml` rather than editing `docker-compose.yml`:

```yaml
services:
  ac-worldserver:
    environment:
      AC_ANIMUS_FORGE_SOCKET: "/azerothcore/var/animus/animus-forge.sock"
    volumes:
      - ./var/animus:/azerothcore/var/animus
```

Then point the learner at `var/animus/animus-forge.sock`.

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

```
cd python
python -m venv .venv && . .venv/bin/activate
pip install torch --index-url https://download.pytorch.org/whl/rocm6.4   # AMD GPU; or .../whl/cpu
pip install -e '.[dev,tensorboard]'

pytest                                                    # protocol, GAE and trainer tests
python -m animus.train --config configs/warrior_dummy.yaml
python -m animus.evaluate --checkpoint runs/warrior_dummy/latest.pt --episodes 256
```

The learner can start before or after the worldserver; it retries until the socket exists.
Disconnecting (Ctrl+C) is safe. The sim waits for the next learner and resets every env when one
connects.

Each run writes `config.yaml`, `spec.json`, `metrics.csv`, TensorBoard logs (if installed) and
checkpoints to `runs/<run_name>/`.

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
