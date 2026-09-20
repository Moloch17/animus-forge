# 7. Operations

Step-by-step procedures. Each one links back to the chapter that explains what happens underneath.

## 7.1 Setting up the training host (Docker)

**Prerequisites.** Linux (or WSL2 on an ext filesystem), Docker with Compose, and roughly 50 GB of disk for client
data, builds and runs. A GPU is optional: updates run on the CPU without one, just more slowly.

1. **Get the core and the modules.**

   ```bash
   git clone -b forge git@github.com:Moloch17/azerothcore-wotlk.git animus-forge-core
   cd animus-forge-core
   git clone git@github.com:Moloch17/animus-forge.git modules/mod-animus-forge
   # optional: animus-lib is bundled in mod-animus-forge/animus-lib; a checkout here is built instead (to develop it)
   git clone git@github.com:Moloch17/animus-lib.git modules/mod-animus-lib
   ```

   If you also keep `modules/mod-animus` in this checkout, it must be disabled in the forge build (step 2).

2. **Machine-specific settings** go in `docker-compose.override.yml` (gitignored):

   ```yaml
   services:
     ac-worldserver:
       environment:
         CCUSTOMOPTIONS: "-DMODULE_MOD-ANIMUS=disabled"      # only if modules/mod-animus exists
         # AMD: ANIMUS_TORCH_INDEX_URL: https://download.pytorch.org/whl/rocm6.4
       # NVIDIA (needs the NVIDIA Container Toolkit):
       deploy:
         resources:
           reservations:
             devices: [{ driver: nvidia, count: all, capabilities: [gpu] }]
     ac-dev-server:
       environment:
         CCUSTOMOPTIONS: "-DMODULE_MOD-ANIMUS=disabled"
   ```

   For AMD, pass `/dev/kfd` and the card's render node, add the host's `render` and `video` group ids, and set
   `security_opt: [seccomp=unconfined]`. The commented block in `docker-compose.yml` has the full example.
   **Set `ANIMUS_TORCH_INDEX_URL` before the first start**, because the venv is created only once.

3. **Start.**

   ```bash
   ./forge.sh
   ```

   The first start takes a long time. It builds the images, downloads client data into the volume, builds the
   worldserver from source, creates the MySQL databases (`Updates.AutoSetup`), creates the Python venv with torch and
   TensorBoard, starts TensorBoard, and starts the worldserver. `forge.sh` attaches you to the console. Detach with
   **Ctrl+P Ctrl+Q**. **Ctrl+C stops the server.**

4. **Check.** The console should print "Animus Forge is idle" and the idle settings. From another terminal, check that
   torch sees the GPU:

   ```bash
   docker compose exec ac-worldserver \
     /azerothcore/modules/mod-animus-forge/python/.venv/bin/python -c "import torch; print(torch.cuda.is_available())"
   ```

   (ROCm builds also report through `torch.cuda`.)

5. **Configure.** Edit `env/dist/etc/modules/mod_animus_forge.conf`, which was created from the `.dist` on first start,
   and `env/dist/etc/worldserver.conf`. Restart with `./forge.sh stop` then `./forge.sh`. At minimum, review:
   - `MapUpdate.Threads` in `worldserver.conf`: set it to the number of physical cores.
   - `AnimusForge.Envs`: 64 by default. Raise it until the learner, not the world thread, is the bottleneck (7.11).
   - `AnimusForge.EpisodeSeconds`: 60 by default. Arenas with their own length ignore it. Stages 3-5 need episodes of
     several minutes, and stage 8's arenas set their own.
   - `AnimusForge.ClassRoles`: empty trains all 18. A subset trains faster, but a later change to the list breaks
     seeding and resuming from those runs.

**Native (without Docker).** Build the forge core with the modules as usual (`acore.sh compiler build`). Create the venv
yourself (`python3 -m venv modules/mod-animus-forge/python/.venv` and
`pip install torch && pip install -e 'modules/mod-animus-forge/python[tensorboard,dev]'`), copy the conf `.dist` to
`.conf`, and run `worldserver` in a terminal. Without `AnimusForge.OutputDir`, runs go into the module's `python/`
directory.

## 7.2 Smoke test with a scripted policy (no Python)

```
forge run stage1_duel fight 256
forge status
```

`forge run` builds the scenario and plays the `fight` baseline for 256 episodes. `forge status` shows the episode means:
`killed`, `died`, `dps`, `casts_completed` and so on. This checks that characters build, opponents spawn and the
mechanics behave, without any learner involved. Compare `random`, `greedy` and `fight`. Those numbers are what a
trained policy must beat.

To baseline every queued scenario, set `AnimusForge.Policy = "fight"` and `AnimusForge.Queue.LocalEpisodes = 1024`,
then `forge start`.

## 7.3 Fast test run

Before a long run, or after changing a scenario, the learner or a config:

```
forge fast
```

It trains every curriculum stage in order (the `mix_duel_pvp` pilot included), each from scratch, with 32 envs,
**all eighteen class/roles at the stage's own levels**, into `<OutputDir>/fast/`. Nothing is skipped, so typing it
again runs the whole curriculum again. `forge fast stage2_pack` trains one stage, seeded from the fast
`stage1_duel` run. Set `AnimusForge.Fast.Queue` to train a shorter list.

**It is a fixed-budget sweep, not an early-stopping smoke test.** Each stage trains a set number of steps and
moves on: 20,000,000 by default, and `forge fast 30M` (or `forge fast 30M stage2_pack`) overrides it for that
invocation. The mechanism is `convergence.patience: 0` in `configs/fast.yaml`, which makes
`ConvergenceTracker.converged` return false, so the stage cannot stop early and cannot trigger a restart; the
cleared `target:` block means a gate cannot halt the sweep either. That makes the sweep a genuine rehearsal of
the real build -- same class/roles, same levels, same stages, less budget -- rather than a different problem.

The budget in force is printed at the start of the run and in `forge status`, so what is reported is the budget
actually used and not the configured default.

What to check, in `fast/runs/<stage>/`:

- `eval.csv`: the `at_start` score against later scores. It should rise.
- `eval_baseline.json`: the baseline to beat.
- `metrics.csv`: entropy should fall slowly, and `approx_kl` and `clip_frac` should stay moderate.
- `finished.json`: why the stage ended.
- For merge stages, the `distill_kl` column should fall.

To exercise stage targets and restarts as well:
`AnimusForge.Fast.Learner.Args = "--set target.min_over_baseline=0.0"`. To start fresh: `forge clean fast`.

## 7.4 Training the curriculum

```
forge start
```

With an empty `AnimusForge.Queue`, this trains every default-queue stage in order (stages 1 to 11; the `mix_duel_pvp`
pilot only when named), skipping any stage whose run already advanced. Each stage:

1. builds its env pool (world stalls for a few seconds per class/role on the first build),
2. writes `layouts/<stage>/`,
3. starts the learner, which seeds from the closest trained ancestor,
4. trains until it advances (the next stage starts), halts below its target (the plan stops), or is cancelled.

To train particular stages: `forge start stage15_pvp stage18_arena`. List each stage after the stage it extends, or it
won't seed from it (the command warns you). With no arguments the queue is all twenty-two stages in number order,
which is already a valid order, so the usual case needs no arguments at all.

### Monitoring

| Where | What |
|---|---|
| `forge status` | The live report: rates, ETAs, evaluation scores against baseline, warnings |
| `forge progress 600` | The same report every 10 minutes |
| The dashboard at http://localhost:18800 | One page: the conf the run is using (and what differs from the dist default), steps, rate, ETA, evaluations against baseline, the training curves, and the last evaluation per class/role. Started by the worldserver container, refreshes every 5 s |
| `<OutputDir>/runs/<stage>/layouts.csv` | Per class/role, **every update**: what each of them is doing in the training episodes themselves (sampled actions, each at its own ladder difficulty). metrics.csv averages all eighteen together and the evaluation tables come only every `eval.every_env_steps`; this is the live view, and the dashboard shows it as "Class and role, right now". Read behaviour from it, not scores -- the gates stay on the evaluations |
| TensorBoard at http://localhost:16006 | `episode_*`, losses, entropy, `eval/*`, `eval_<band>/*`, `eval_arena_<arena>/*` |
| `env/dist/logs/animus-learner.log` | Everything the learner prints, including evaluation tables per level band, class/role and arena |
| `<OutputDir>/runs/<stage>/eval.csv`, `eval.jsonl` | Every evaluation, with full tables |
| `<OutputDir>/runs/<stage>/stage.jsonl` | Restart, advance and halt decisions with their gates |
| `forge scenarios` | Every stage's run: finished and why, checkpoints, steps, best score |

Warnings to act on:

- **"learner has not answered"**: it is stalled or doing a very long update. Check the learner log.
- **"step rate dropped"**: another process may be competing for CPU, or an evaluation is running.
- **"entropy under 25%"**: the policy may have collapsed early. Consider raising `mappo.entropy_coef`.
- **"approx KL / clip fraction high"**: updates are too large. Lower the learning rates or the epochs.
- **"best below baseline after 2 evaluations"**: check the reward for that stage, and compare against the fast run.

### Warnings the learner prints without being asked

Two things are checked every update and reported when they happen. Neither changes what the run does; both are
there because the failure they describe is invisible in the ordinary metrics until a run has been wasted on it.

**`reward: <term> earns N an episode, X% of the largest outcome term`**

A shaping term has grown into the objective. Outcome terms -- the kill, the clear, the capture, the arrival --
are what a stage is *for* and may be any size; everything else is a nudge, and a nudge worth more than half a
kill is not a nudge. Only earnings trip it, never charges: a penalty is not farmable, and the largest negative
term in a fight is the death, which is the point of having one.

What to do: read the mix on the same line and decide whether the term is mispriced or exploitable. Three times
in this project it was both. A resurrection offer the core never clears was being accepted every decision and
came to 88% of `druid_dps`'s return; a goal paid for every decision it was held made standing at range the
second largest earner; an order nudge priced per decision reached 23.7% of gross, level with the kill. The
first two were found by hand after runs had already trained on them.

The rule lives in `python/animus/rewards.py`, including which terms count as outcomes. A resurrection is
deliberately not one of them -- standing an ally up is a means, and listing it as an outcome is exactly what
would let a farmable revive read itself as the yardstick.

**`learning has stalled: approx_kl has stayed under ... for N updates`**

The updates have stopped moving the policy. Roughly half the stages measured end their run this way --
`approx_kl` falls eight to elevenfold between the first eighth of a run and the last, with `clip_frac` down to
about 0.01 -- so the final third costs wall clock and buys very little.

What to do: **nothing automatically.** The other half of the stages do not stall at all (`stage1_duel`'s KL
*rises* over 683 updates; travel and flight stay flat), and `stage4_gauntlet` trips this check and then went on
to 916 productive updates. Read it together with the evaluation: if the score is not improving either, the rest
of the run is wall clock and the budget is better spent on the next stage.

### Controlling a run

| Goal | Command |
|---|---|
| Freeze everything, sim and learner | `forge pause`, later `forge resume` |
| Stop and keep the progress | `forge cancel` (saves `latest.pt`), later `forge resume` |
| Give up on the current stage and go to the next | `forge skip` |
| Continue a particular stage from its checkpoint | `forge resume stage4_gauntlet [stage9_companion ...]` |
| Retrain a finished stage | `forge start stage4_gauntlet`, which archives the old run |
| Fine-tune a stage from its own best (after reward or mask changes) | copy its `best.pt` to `runs/_finetune/<stage>/best.pt`, then `forge start <stage>`: the learner seeds from it before the seed chain (`finetune_from`) |

### After changing C++

```bash
docker compose exec ac-dev-server ./acore.sh compiler build     # incremental build into the shared volumes
docker compose restart ac-worldserver                          # the learner saves on stop
```

Or run `./forge.sh --build`, which recreates the container and builds before starting. The restarted sim is idle:

- `forge start` continues with the stages that haven't advanced yet, **from scratch**.
- `forge resume <stage>` continues a run from its `latest.pt`, **if the stage's shapes didn't change**. If a block,
  catalog or the class/role list changed, the learner refuses to resume. Start the stage fresh; it still seeds from its
  ancestors.

If layouts changed for a stage that earlier stages were trained on, those ancestors still seed block by block where
block sizes match. A block whose size changed raises an error during seeding, and the ancestor must be retrained.

## 7.5 When a stage halts below its target

The plan stops with outcome `below target` and the learner exits 3.

1. Read `runs/<stage>/finished.json` (reason, best score, gates) and the last lines of `stage.jsonl`, which list the
   failed gates, for example `layout priest_heal: score 1.2 < baseline 1.5`.
2. Read `eval.jsonl` for per-class/role, per-band and per-arena scores next to the baseline.
3. Decide:
   - **The target is too strict.** The configs' targets are first guesses. Edit `python/configs/<stage>.yaml` (for
     example `target.min_layout_over_baseline: -0.1`), or apply it to every stage with `AnimusForge.Learner.Args =
     "--set target.min_layout_over_baseline=-0.1"`.
   - **The stage needs more training.** Raise `total_env_steps` or `convergence.patience`, or loosen
     `min_improvement`.
   - **The reward or scenario is wrong.** Change `AnimusForge.Curriculum.*` tuning or the code, check it with
     `forge fast <stage>`, then retrain.
4. `forge start <stage>` (and the stages after it) to train again. `best.pt` of the halted run is archived, not deleted.

## 7.6 Exporting and deploying models

1. **Export**, even during training:

   ```
   forge export stage10_party            # best.pt, else latest.pt
   forge export stage10_party latest
   ```

   Output goes to `AnimusForge.ModelDir` (default `modules/mod-animus-forge/models/`) as one `.amdl` and one `.json`
   per class/role, for example `warrior_tank_party.amdl` and `warrior_tank_party.json`. The export log is
   `animus-export.log`. "Export of stage10_party finished" appears in the console.

2. **Copy both files for every class/role** to the realm's `Animus.ModelDir` (default `<DataDir>/animus`).

3. **Configure the realm** (`mod_animus.conf`): set `Animus.Curriculum.Stage` to the stage whose models companions
   should play (`stage10_party`, or `stage22_crossroads` for PvE and PvP), and `Animus.Curriculum.DecisionMs` to the
   training decision interval.

4. **Load.** Models load on first use. On a running realm, `.reload config` resets the model cache.

5. **Verify** in game: `.animus summon human warrior tank`, then `.animus list`. A model that is refused shows the
   reason, and the log says `Animus model <name> not loaded: <reason>`.

The realm's animus-lib must build the same manifests. Use the animus-lib revision the forge trained with, and the same
world database and DBC data, because trainer spells and the spell catalog come from them.

## 7.7 Watching a stage in game

On a stock realm with mod-animus and the models (`.animus stage open` turns GM mode on for you):

```
.animus stage open stage1_duel model         # teleports you; the first episode spawns frozen
.animus stage spawn 6 warlock_dps 70         # a new episode, frozen: tier 6 (elite, +2 levels), a level 70 warlock
.animus stage start                          # play, episode after episode
.animus stage stop                           # freeze where it is
.animus stage status
.animus stage close
```

To see exactly the training conditions, copy the run's `stage.json` `"tuning"` values into `Animus.Curriculum.*`, and
match `Animus.Stage.DecisionMs`, `EpisodeSeconds`, `Level` and `SpawnPoint.*` to the forge settings. To look at one
situation of stage 8: `.animus stage open stage22_crossroads model ambush`. To compare with the baseline:
`.animus stage open stage10_party fight`.

## 7.8 Running the learner by hand

Useful for debugging the learner in an IDE:

1. Set `AnimusForge.Learner.AutoStart = 0` and restart the server.
2. `forge start stage1_duel`. The console prints the exact learner command to run.
3. From `python/`:

   ```bash
   .venv/bin/python -m animus.train --config configs/stage1_duel.yaml --run-name stage1_duel \
       --socket /tmp/animus-forge.sock --runs-dir <OutputDir>/runs --layouts-dir <OutputDir>/layouts
   ```

   A hand-started learner retries until the socket exists. Ctrl+C is safe: the learner saves, and the sim waits for the
   next learner and resets every env when one connects. Point `--runs-dir` and `--layouts-dir` at the sim's output, or
   the learner won't find `stage.json`.

**Tests:**

```bash
docker compose exec -w /azerothcore/modules/mod-animus-forge/python ac-dev-server .venv/bin/python -m pytest
```

**Standalone evaluation of a checkpoint** (the sim must be running the same scenario with no other learner attached):

```bash
python -m animus.evaluate --checkpoint runs/stage1_duel/best.pt --episodes 128 --seed 1000 --baseline fight
python -m animus.evaluate --checkpoint runs/stage18_arena/best.pt --baseline fight --opponent-baseline
```

## 7.9 Extending the curriculum

Most changes alter layout manifests. **Any change to a block, a catalog rule, a stage's blocks or a character-building
rule that affects actions invalidates exported models and blocks `forge resume` of affected runs.** Plan to retrain from
the first affected stage.

Remember that animus-lib must still build on a stock core. Use only public APIs, or add a `CoreHooks` seam. Header names
must stay unique across the three modules.

### Change a reward weight or a chance

Set the key in `mod_animus_forge.conf` (`AnimusForge.Curriculum.Pulls.Interrupt = 0.5`). No rebuild is needed, only a
restart before the stage starts. The value is recorded in the run's `stage.json`. Rewards don't affect the manifest.

### Add a tuning value

1. Add a field with its default and comment to the right group in `CurriculumTuning.h`.
2. Add one `f("Group.Name", tuning.Group.Name);` line to `Visit`.
3. Document it in `animus-forge/conf/mod_animus_forge.conf.dist`, and optionally in `animus/conf/mod_animus.conf.dist`
   under `Animus.Curriculum.`.

### Add a reward term

1. Add it to `RewardTerm` (`Rewards/RewardLedger.h`) and to `RewardTermName` (`Rewards/CombatReward.cpp`).
2. Return it from the paying encounter's `RewardTerms()` and add it in `Reward()` with
   `ledger.Add(RewardTerm::X, value)`. Scale per-decision terms by `_scenario.DecisionScale()`.
3. It appears automatically as `reward_<name>` in episode info, TensorBoard and the learner's summaries.

### Add an observation feature or action to a block

1. Change the block's `Size`, `Observe` (and `Apply` for actions), and `DescribeManifest` if the feature depends on
   lists.
2. If it needs something the world can't provide directly, add a field to `SeatView` and fill it in the encounter's
   `View` (and in mod-animus's `CompanionParty::View` for companions).
3. Update `Baselines.cpp` if it reads a moved index.
4. Retrain every stage that has the block. Seeding treats a block whose size changed as incompatible, so retrain from
   the first stage that has it.

### Add a block

1. Add a `BlockId` (before `Count`) and its name in `BlockName` (`Layout/Layout.cpp`).
2. Implement `Block` in `Blocks/<Name>Block.{h,cpp}`: `Id`, `Size`, `Observe`, and optionally `BeforeApply`, `Apply`,
   `DescribeManifest`.
3. Register it in `Blocks/Blocks.cpp` (`GetBlock`).
4. Add it to the stages that need it. Seeding gives the new block zero input weights and small action weights in those
   stages. Other blocks keep their trained weights.

### Add an encounter

1. Declare it in `Encounters/Encounters.h` and implement it in `Encounters/<Name>Encounter.cpp`, overriding only the
   hooks it needs.
2. Decide what arena field selects it (possibly a new `ArenaDefinition` field and `ArenaProblem` rule), create it in the
   `StageScenario` constructor in the right build order, and add it to the reward order list and to `uses`.
3. Keep per-env state sized at construction. Give it `Deactivate` if it leaves anything in the world, so an arena switch
   removes it.
4. Put its tuning in `CurriculumTuning`. If it spawns scripted players, give their accounts a range in
   `BotAccounts.h`.

### Add a stage

1. Add a `StageDefinition` to `Stages/Stages.cpp`, after the stage it extends. Validation rules are in
   [4.1](04-curriculum.md#41-defining-a-stage).
2. Write `python/configs/<name>.yaml` with `extends: <parent>.yaml` and only what differs: `run_name`, budget, gamma,
   evaluation report, convergence, target, and `distill:` for a merge stage.
3. Leave `InDefaultQueue` true to add it to `forge start`, or false to train it only by name.
4. Check it: `forge run <name> fight 64`, then `forge fast <name>`.
5. For mod-animus, nothing else is needed. `.animus stage list` shows it and companions can use its models.

### Add a standalone scenario

Implement `Animus::Scenario`, create it in `CreateScenario` and list it in `ScenarioNames`
(`src/Scenario/Scenario.cpp`), and write a learner config. The protocol and learner are shape-generic. For more than one
agent per env, provide a real global `State`.

### Change a spec build

Edit `tools/spec_builds/builds.py`, run `validate.py`, then `generate.py` to rewrite `SpecBuilds.cpp`. After a rebuild,
`forge talents <class_role> [spec] [points] [plan]` prints the build a character gets at any point count, under any of
the three talent plans. Talent features change, so models of that class/role must be retrained.

## 7.10 Changing the forge core

Follow [chapter 2's rules](02-forge-core.md#21-rules-the-fork-follows): a `Forge*` replacement called from the top of
the original, no config gates, the upstream body left verbatim, and a header comment explaining what was dropped and
why. Build with `acore.sh compiler build` in the dev container. When a module needs a new core capability, add the API
to the core and a seam in `CoreHooks`, and install it from mod-animus-forge.

## 7.11 Performance

- **Measure it: `forge bench`.** It times the sim at every `AnimusForge.Bench.Threads` x `Envs` pair, then runs the
  fastest few again with the real learner (x `Bench.LearnerTorchThreads`), and reports the env steps per second of
  each. Trials run in `<OutputDir>/bench/` with evaluation, seeding and distillation off, so no real run is touched;
  the results are in `bench/bench.json`, and `forge bench apply` writes the winner in place: `MapUpdate.Threads` into
  `worldserver.conf`, `AnimusForge.Envs` and (when a torch thread count won) `AnimusForge.Learner.TorchThreads` into
  `mod_animus_forge.conf`, backing each file up as `<file>.before-bench`. The thread count takes effect when the
  worldserver restarts, the env count at the next `forge start`. `forge cancel` stops a sweep and restores the
  configured thread count.
- **Know which side is the bottleneck.** `forge status` shows env steps per second and where a decision's wall time
  goes: **world** (the map update, spread over `MapUpdate.Threads`; every env is its own instance), **sim** (this
  module's rewards, observations and actions, on the world thread, linear in envs) and **learner** (blocked on its
  actions and updates). In remote mode every decision is one Python round trip for all envs.
  - If the learner's forward pass dominates (high CPU in the learner process, the world thread idle waiting), fewer,
    larger batches help: raise `AnimusForge.Envs`.
  - If the world thread dominates, more map threads (`MapUpdate.Threads`) and fewer class/roles or simpler arenas help.
  - The learner's torch and the map update threads share the cores: `AnimusForge.Learner.TorchThreads` caps torch,
    and the benchmark sweeps both together.
- **Know which part of the sim.** The `sim parts` row splits that **sim** share into reward, observe (which carries
  the action mask), final observe (the same work without the mask, for an episode that just ended), reset (building
  the next episode's characters) and apply. Observe against final observe, per call, is the cheapest read on what
  mask building costs; reset against the episodes rebuilt per decision says whether episode turnover is worth
  attacking. Measure here before optimising the sim: the answer decides what is worth doing.
- **Envs are also a training setting.** One update is `rollout_length x envs x seats` env steps, so a different env
  count changes the batch PPO trains on, not only the speed. `forge bench` says so when its winner differs from the
  env count you train with.
- **Updates versus rollouts.** `update_seconds` in `metrics.csv` is time spent in PPO updates, which a GPU speeds up.
  Rollouts stay on the CPU on purpose. Serially that time is sim idle time: `env_steps_per_sec` in `metrics.csv` is the
  rollout's own rate, and the rate over the wall clock is lower by the update's share. `overlap_updates` runs the
  update on a worker thread while the sim collects the next rollout and closes most of that gap; the rollout then acts
  on the update before last, and update stats are logged one update late. It is worth having only while an update
  costs much more than a rollout: measure both before turning it on (the curriculum stages train serially, and an
  update there is ~1.1 s against a ~2 s rollout).
- **Evaluation cost.** Every evaluation resets all envs and runs `eval.episodes` seeded episodes plus confirmation
  episodes. Large evaluations every few million steps can take a significant share of wall time. `eval.every_env_steps`
  and `eval.episodes` trade that time against the reliability of convergence decisions -- but the trade is cheap in the
  curriculum stages: an evaluation is seconds of sim time against tens of minutes of training, while its noise sets the
  convergence margin and the per-class/role gates. Too few episodes is the more common mistake.
- **Asset builds** take a few seconds per class/role at the start of each stage (trainer data and item pools). This is
  expected.
- **Memory.** Instances are created once and reused. Bots reuse two GUIDs per slot. Steadily growing memory during a run
  points to a leak worth investigating (a new per-GUID core cache, or instances not unloading).

## 7.12 Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Server exits at once in Docker | The console read end of file. Run through `forge.sh`/Compose, which gives it a TTY. A server without a TTY skips the console and keeps running |
| Every `AnimusForge.*` (or `Animus.*`) key logs "Missing property" | The module's `.conf` doesn't exist: AzerothCore no longer reads a module's `.dist`. Installing creates it when missing (Docker copies it to the config volume on start); for an install that predates that, copy it from the `.dist` |
| "The world ticks N ms, but AnimusForge.DecisionMs is M" | The worldserver was built before "one tick per decision". Run `./forge.sh --build` |
| Configure fails: "mod-animus-forge needs mod-animus-lib, which is disabled" or linkage mismatch | Build both the same way. Set the named variable to `static` or `dynamic` |
| Configure fails: "built dynamic, which needs animus-lib as its own module" | Copy the module's `animus-lib/` bundle to `modules/mod-animus-lib` and build it dynamic too |
| "Learner directory ... does not contain animus/train.py" | The worldserver runs from a baked image, or the module moved. Set `AnimusForge.Learner.WorkDir` |
| "waiting for learner" forever | Auto-start failed (see the server log and `animus-learner.log`), or `AutoStart = 0`. Run the printed command by hand |
| The learner exits right after connecting | Config error (unknown key, wrong type), target validation (a gate on a missing metric), or a resume mismatch. See `animus-learner.log` |
| "cannot resume ...: the scenario's layouts changed" | Shapes changed since the checkpoint. Use `forge start <stage>` instead |
| "trunk.…: the trunk in the checkpoint does not match (hidden sizes must be equal)" | The stage's `mappo.hidden` differs from the ancestor's. Keep `[256, 512, 512]` across stages |
| A stage always starts "from scratch" | Its ancestors have no `best.pt`/`latest.pt` in this `runs/`. Train them first, or move old runs from `modules/mod-animus-forge/python/runs/` to `var/animus-forge/runs/` |
| "Stage X is left out: ..." at startup | A definition broke a validation rule (4.1). Fix `Stages.cpp` |
| Evaluation "stopped after N decisions with k of M episodes" | Episodes are longer than `SPEC.EpisodeSeconds` suggests, or envs are stuck rebuilding. Check for "could not build its episode" errors |
| "env N could not build its episode" repeated | Character or encounter build failures, usually spawn point or map problems, or missing world data. Check `AnimusForge.SpawnPoint.*` and the log lines before it |
| Async query queue or memory keeps growing | A database write on a bot path. Check that the core has sim sessions and groups (2.8) and look at sync-query warnings |
| Realm refuses a model: "its manifest differs" | Different animus-lib revision, world database or DBC data than training. Retrain with the realm's, or align versions |
| Realm: "no layout manifest ... beside the model" | Copy the `.json` next to the `.amdl`. The CMake install step copies only `.amdl` |
| Companion only follows you | Its model is missing or refused (`.animus list`), or it has no target and can't act without one |
| Stage viewer: "you are not in an instance of its map" | `Animus.Stage.SpawnPoint.MapId` isn't instanceable, or the teleport failed within 60 s |
| Not enough detail in the log | Set `Logger.module.animus=1,Console Server` in `worldserver.conf` for debug-level Animus logging (both modules and animus-lib log under `module.animus`) |
