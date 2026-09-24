# The Animus Manual

This manual explains how the Animus project works, from the modified AzerothCore server at the bottom to the trained
companions that play in a normal realm at the top. It is meant to be read by someone who has to run, change or debug
any part of it.

Animus is four pieces of software. Each chapter covers one of them in depth, and the overview shows how they connect.

| Piece | Repository | What it is |
|---|---|---|
| **The forge core** | `forge` branch of [azerothcore-wotlk](https://github.com/Moloch17/azerothcore-wotlk) | AzerothCore turned into a headless simulator that runs faster than real time |
| **The curriculum layer** | `modules/mod-animus-forge/src` (was animus-lib, a repository of its own) | The curriculum, env pools and bots |
| **Animus Forge** | [animus-forge](https://github.com/Moloch17/animus-forge) (`modules/mod-animus-forge`) | The training module and its Python MAPPO learner |
| **Animus** | [animus](https://github.com/Moloch17/animus) (`modules/mod-animus`) | The module that plays the trained models on a stock AzerothCore |

## Chapters

1. [Overview](01-overview.md): the goal, the four pieces, and how a model goes from an idea to a companion in someone's
   party.
2. [The forge core](02-forge-core.md): what the `forge` branch changes in AzerothCore and why. Covers the fixed-tick
   loop, the game clock, the stripped packet and database paths, and how the fork stays rebasable.
3. [animus-lib](03-animus-lib.md): the scenario interface, env pools, sessionless bots, core seams, layouts and
   manifests, and the `.amdl` model runtime.
4. [The curriculum](04-curriculum.md): the stages and how they seed each other, their blocks, arenas,
   encounters and rewards, the characters the seats become, and team play under a director.
5. [Animus Forge](05-animus-forge.md): the training module (plans, the lock-step bridge, the learner process, console,
   progress, export) and the Python learner (MAPPO, seeding, distillation, evaluation, convergence and stage targets).
6. [Animus](06-animus.md): companions and the stage viewer on a live, stock server.
7. [Operations](07-operations.md): step-by-step workflows for setup, training, monitoring, exporting, deploying,
   extending and troubleshooting.
8. [Reference](08-reference.md): configuration keys, the wire protocol, file formats, run directories, account
   ranges, exit codes and a glossary.

## If you only read one thing

Training is one command against a running server: `./forge.sh` attaches to the console, `forge start` trains
the whole twenty-three-stage queue in order, and `forge status` says how it is going. Everything else in this
manual is detail under that.

## Where to start

- **You want to train models.** Read the [overview](01-overview.md), then [Operations](07-operations.md). Go back to
  [Animus Forge](05-animus-forge.md) when you need to understand a report or a decision the learner made.
- **You want to change what the bots learn** (a reward, a feature, a new stage). Read [animus-lib](03-animus-lib.md)
  and [the curriculum](04-curriculum.md), then "Extending the curriculum" in [Operations](07-operations.md).
- **You want to change the simulator.** Read [the forge core](02-forge-core.md) first. It explains the rules the fork
  follows, and breaking them makes upstream rebases painful.
- **You run a realm and want companions.** Read [Animus](06-animus.md).
- **A run is telling you something you do not recognise.** The learner prints two kinds of unprompted warning:
  a reward-mix line when a shaping term has grown into the objective, and a stall line when the updates have
  stopped moving the policy. Both are explained in [Operations](07-operations.md).

## Conventions

- Paths such as `src/Env/EnvPool.cpp` are relative to the repository the chapter is about. Paths that start with
  `animus-lib/`, `animus-forge/`, `animus/` or `core/` name the repository explicitly.
- Configuration keys are written in full: `AnimusForge.DecisionMs`, `Animus.Stage.Policy`. Every key can also be set
  through the environment. Upper-case it, replace dots with underscores and add the `AC_` prefix:
  `AC_ANIMUS_FORGE_DECISION_MS`.
- "Game time" is simulated time and "wall time" is real time. On the forge core the two are unrelated. That
  difference is behind most of the core's changes.
- The module READMEs (`animus-forge/README.md`, `animus/README.md`, `animus-lib/README.md`) are overviews that link
  into this manual; the detail lives here. Where the manual and the source code disagree, the code is authoritative.
  Please report the mismatch.
- Stage numbers now sort into the training order: stage 1 to stage 23 is what `forge start` walks, and no stage
  is reached before the stage it seeds from. The tree in [chapter 4](04-curriculum.md) and each stage's `Extends`
  are still what decide what follows what -- the numbers agree with them rather than replacing them.
- Numbers quoted from runs (a reward share, a revive count, an entropy) are measurements, with the stage and
  the point in the run they came from. They are there to show the size of a thing, and they go stale; re-measure
  before relying on one.
