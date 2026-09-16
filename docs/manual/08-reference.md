# 8. Reference

## 8.1 Configuration keys

Every key can also be set from the environment: `AC_` plus the key in upper snake case, for example
`AnimusForge.Learner.AutoStart` becomes `AC_ANIMUS_FORGE_LEARNER_AUTO_START`.

### mod-animus-forge (`mod_animus_forge.conf`)

| Key | Default | Meaning |
|---|---|---|
| `AnimusForge.Enable` | `1` | `0` turns the module off (no bots, no socket, hooks return at once) |
| `AnimusForge.OutputDir` | `""` = learner work directory | Where `runs/` and `layouts/` go. Docker: `/azerothcore/var/animus-forge` |
| `AnimusForge.Queue` | `""` = every default-queue stage | Scenarios `forge start` trains when given none |
| `AnimusForge.Queue.SkipFinished` | `1` | `forge start` without names skips stages that already advanced |
| `AnimusForge.Queue.LocalEpisodes` | `0` | With a local policy, episodes per scenario of `forge start` (0 = until cancelled) |
| `AnimusForge.ClassRoles` | `""` = all 18 | Comma-separated class/roles the stages play |
| `AnimusForge.Envs` | `64` | Parallel envs, one instance each (capped at 12500) |
| `AnimusForge.DecisionMs` | `100` | Game time per decision, and the world tick |
| `AnimusForge.EpisodeSeconds` | `60` | Episode length for arenas without their own |
| `AnimusForge.SpawnPoint.MapId` | `560` | Instanceable map every env starts in (Old Hillsbrad Foothills) |
| `AnimusForge.SpawnPoint.X/Y/Z/O` | `2741.9`, `1315.2`, `14.0`, `2.96` | Spawn position |
| `AnimusForge.Policy` | `"remote"` | `remote` (learner), `random`, `greedy`, `fight` |
| `AnimusForge.ReportEpisodes` | `256` | Episode info means are taken over this many episodes |
| `AnimusForge.Socket` | `"/tmp/animus-forge.sock"` | Learner socket |
| `AnimusForge.Learner.AutoStart` | `1` | Start the learner as a child process |
| `AnimusForge.Learner.WorkDir` | `""` = `<module>/python` | Learner working directory |
| `AnimusForge.Learner.Python` | `""` = `<WorkDir>/.venv/bin/python`, else `python3` | Interpreter |
| `AnimusForge.Learner.Config` | `""` = `configs/<scenario>.yaml` | One config for every scenario |
| `AnimusForge.Learner.Args` | `""` | Extra arguments for every learner (`--set key=value ...`) |
| `AnimusForge.Learner.TorchThreads` | `0` | CPU threads for the learner's torch (`--set torch_threads`); 0 = torch's default |
| `AnimusForge.Learner.LogFile` | `""` = `<LogsDir>/animus-learner.log` | Learner output |
| `AnimusForge.Bench.Scenario` | `"stage1_duel"` | What `forge bench` times without a name |
| `AnimusForge.Bench.Policy` | `"fight"` | Local policy the sim-only trials play |
| `AnimusForge.Bench.Threads` | `"4, 8, 12, 16"` | `MapUpdate.Threads` values tried |
| `AnimusForge.Bench.Envs` | `"64, 128, 192"` | `AnimusForge.Envs` values tried |
| `AnimusForge.Bench.MaxEnvs` | `256` | Never try more envs than this |
| `AnimusForge.Bench.WarmupTicks` | `128` | Decisions before a sim-only trial is timed |
| `AnimusForge.Bench.MeasureTicks` | `384` | Decisions timed per sim-only trial |
| `AnimusForge.Bench.MaxMemoryPercent` | `80` | Skip bigger envs once memory is this used |
| `AnimusForge.Bench.LearnerTop` | `2` | Fastest sim trials re-timed with the learner (0 = sim only) |
| `AnimusForge.Bench.LearnerWarmupTicks` | `384` | Decisions before a learner trial is timed |
| `AnimusForge.Bench.LearnerMeasureTicks` | `768` | Decisions timed per learner trial |
| `AnimusForge.Bench.LearnerTorchThreads` | `"0, 8"` | Torch thread counts tried with the learner |
| `AnimusForge.Fast.Queue` | `""` | What `forge fast` trains without names; empty = every curriculum stage in order |
| `AnimusForge.Fast.Envs` | `32` | Fast profile envs |
| `AnimusForge.Fast.Level` | `20` | Fast profile level (0 = random) |
| `AnimusForge.Fast.ClassRoles` | `"warrior_tank, priest_heal, rogue_dps, hunter_dps"` | Fast profile class/roles |
| `AnimusForge.Fast.OutputDir` | `"fast"` | Inside `OutputDir` when relative |
| `AnimusForge.Fast.Learner.Overlay` | `""` = `configs/fast.yaml` | Learner overlay for fast runs |
| `AnimusForge.Fast.Learner.Args` | `""` | Extra arguments for fast learners (after `Learner.Args`) |
| `AnimusForge.ModelDir` | `""` = `<module>/models` | Where `forge export` writes |
| `AnimusForge.Progress.Interval` | `0` | Seconds between periodic reports (0 = off) |
| `AnimusForge.Curriculum.*` | see 8.2 | Curriculum tuning |

Relative path keys are relative to the directory of the loaded `worldserver.conf`, except `Fast.OutputDir`.

`forge bench` writes `<OutputDir>/bench/bench.json`: every trial (`map_threads`, `envs`, `agents`, `learner`,
`torch_threads`, `env_steps_per_second`, `world_ms`, `sim_ms`, `learner_ms`, `memory_mb`) and the winning one as
`best`, which `forge bench apply` reads.

The forge core also relies on these `worldserver.conf` keys: `MapUpdate.Threads` (what `forge bench` tunes),
`Console.Enable`, `LogsDir`, `DataDir`, the database info keys, and `Updates.AutoSetup`.

### mod-animus (`mod_animus.conf`)

| Key | Default | Meaning |
|---|---|---|
| `Animus.Enable` | `1` | `0`: no summons or stages, existing ones removed, no models loaded |
| `Animus.ModelDir` | `"animus"` | Model directory, relative to `DataDir` |
| `Animus.Curriculum.Stage` | `"stage5_party"` | The stage whose models companions play |
| `Animus.Curriculum.DecisionMs` | `100` | Companion decision interval |
| `Animus.Stage.Policy` | `"model"` | Default stage viewer policy |
| `Animus.Stage.DecisionMs` | `100` | Stage viewer decision interval |
| `Animus.Stage.EpisodeSeconds` | `60` | Episode length for arenas without their own |
| `Animus.Stage.ClassRoles` | `""` | Characters that appear (doesn't change layouts) |
| `Animus.Stage.Level` | `0` | Every character's level (0 = random) |
| `Animus.Stage.MaxViewers` | `4` | Stages running at once |
| `Animus.Stage.SpawnPoint.MapId/X/Y/Z/O` | `560`, `2741.9`, `1315.2`, `14.0`, `2.96` | Where stages happen |
| `Animus.Curriculum.<tuning>` | see 8.2 | The stage viewer's curriculum tuning (companions don't use it) |

CMake: `ANIMUS_MODELS_INSTALL_DIR` (default `<install prefix>/data/animus`). Both modules: `ANIMUS_LIB_GIT_URL`
(`https://github.com/Moloch17/animus-lib.git`), `ANIMUS_LIB_GIT_REF` (`master`).

### Docker environment (`docker-compose.yml`)

| Variable | Default | Meaning |
|---|---|---|
| `ANIMUS_TORCH_INDEX_URL` | empty (PyPI) | torch wheel index for the venv (ROCm: `https://download.pytorch.org/whl/rocm6.4`, CPU: `https://download.pytorch.org/whl/cpu`) |
| `ANIMUS_FORGE_OUTPUT_DIR` | `/azerothcore/var/animus-forge` | Becomes `AC_ANIMUS_FORGE_OUTPUT_DIR` |
| `DOCKER_DB_EXTERNAL_PORT` | `13306` | MySQL on `127.0.0.1` |
| `DOCKER_TENSORBOARD_EXTERNAL_PORT` | `16006` | TensorBoard on `127.0.0.1` |
| `DOCKER_DB_ROOT_PASSWORD` | `password` | MySQL root password |
| `CCUSTOMOPTIONS` | (override) | Extra CMake options, for example `-DMODULE_MOD-ANIMUS=disabled` |

## 8.2 Curriculum tuning defaults

Prefix: `AnimusForge.Curriculum.` (forge) or `Animus.Curriculum.` (mod-animus). Per-decision terms are tuned per 50 ms.

| Key | Default | | Key | Default |
|---|---|---|---|---|
| `Characters.HighLevelFirst` | 61 | | `Pulls.LinkedChance` | 70 |
| `Characters.HighLevelChance` | 50 | | `Pulls.EliteChance` | 15 |
| `Characters.NoisyTalentChance` | 30 | | `Pulls.HigherLevelChance` | 25 |
| `Characters.RandomTalentChance` | 10 | | `Pulls.PartyEliteChance` | 50 |
| `Characters.TalentNoisePoints` | 5 | | | |
| `Party.SizeWeight1` | 20 | | `Pulls.HigherLevelChance` | 25 |
| `Party.SizeWeight2` | 20 | | `Pulls.PartyEliteChance` | 50 |
| `Party.SizeWeight3` | 20 | | `Pulls.NextPullMinMs` | 8000 |
| `Party.SizeWeight4` | 40 | | `Pulls.NextPullMaxMs` | 20000 |
| `Party.ClassicChance` | 50 | | `Pulls.OwnerEngageMinMs` | 1500 |
| `Party.RoleTankChance` | 25 | | `Pulls.OwnerEngageMaxMs` | 5000 |
| `Party.RoleHealerChance` | 25 | | `Pulls.PartyOwnerEngageMinMs` | 4000 |
| `Party.TeammateDamageTakenDps` | 0.5 | | `Pulls.PartyOwnerEngageMaxMs` | 7000 |
| `Party.TeammateDamageTakenProtector` | 1.0 | | `Pulls.OwnerPullsMinMs` | 500 |
| `Party.TeammateHealing` | 2.0 | | `Pulls.OwnerPullsMaxMs` | 1500 |
| `Party.TankLoseTeammate` | 0.02 | | `Pulls.OwnerPullsChance` | 30 |
| `Party.TeammateDeath` | 3.0 | | `Pulls.RecoverFraction` | 0.5 |
| `Duel.DamageDealt` | 2.0 | | `Pulls.DamageDealt` | 2.0 |
| `Duel.DamageTaken` | 1.0 | | `Pulls.DamageTaken` | 1.0 |
| `Duel.Approach` | 0.5 | | `Pulls.GauntletDamageTaken` | 1.5 |
| `Duel.StealthOpener` | 0.5 | | `Pulls.Approach` | 0.5 |
| `Duel.StealthUtility` | 0.05 | | `Pulls.StealthOpener` | 0.5 |
| `Duel.StepCost` | 0.0002 | | `Pulls.StealthUtility` | 0.05 |
| `Duel.Kill` | 2.0 | | `Pulls.Interrupt` | 0.3 |
| `Duel.FastKill` | 3.0 | | `Pulls.Kill` | 0.5 |
| `Duel.HealthKept` | 1.0 | | `Pulls.StepCost` | 0.0002 |
| `Duel.Death` | 3.0 | | `Pulls.Clear` | 2.0 |
| `Duel.MeleeRange` | 3.5 | | `Pulls.FastClear` | 3.0 |
| `Duel.RangedRange` | 25.0 | | `Pulls.FastPull` | 2.0 |
| `Casting.TimeWasted` | 0.03 | | `Pulls.HealthKept` | 1.0 |
| `Casting.TimeCompleted` | 0.0 | | `Pulls.PackDeath` | 3.0 |
| `Resurrection.GraceMs` | 20000 | | `Pulls.GauntletDeath` | 5.0 |
| `Resurrection.ReviveAlly` | 1.5 | | `Pulls.OwnerClearScale` | 2.0 |

| Key | Default | | Key | Default |
|---|---|---|---|---|
| `Owner.LevelSpread` | 2 | | `Opponent.LevelSpread` | 1 |
| `Owner.TankChance` | 25 | | `Opponent.EngageMaxMs` | 3000 |
| `Owner.HealerChance` | 25 | | `Opponent.HealerChance` | 20 |
| `Owner.DamageTakenDps` | 1.0 | | `Opponent.TankChance` | 20 |
| `Owner.DamageTakenProtector` | 2.0 | | `Ambush.MinMs` | 20000 |
| `Owner.TankOwnerDamageShare` | 0.25 | | `Ambush.MaxMs` | 120000 |
| `Owner.Healing` | 2.0 | | `Ambush.EngageMaxMs` | 3000 |
| `Owner.TankDamageRefund` | 0.5 | | `Ambush.Kill` | 3.0 |
| `Owner.TankHold` | 0.002 | | `ScriptedPlayers.SpellMinMs` | 2000 |
| `Owner.TankLose` | 0.02 | | `ScriptedPlayers.SpellMaxMs` | 4000 |
| `Owner.PulledThreat` | 0.004 | | `ScriptedPlayers.HealMinMs` | 1500 |
| `Owner.SoloFight` | 0.01 | | `ScriptedPlayers.HealMaxMs` | 2500 |
| `Owner.FollowFar` | 0.002 | | `ScriptedPlayers.WanderMinMs` | 6000 |
| `Owner.FollowNear` | 0.0005 | | `ScriptedPlayers.WanderMaxMs` | 12000 |
| `Owner.FollowFarDistance` | 25.0 | | `ScriptedPlayers.RegenFraction` | 0.04 |
| `Owner.FollowNearDistance` | 12.0 | | `ScriptedPlayers.HealBelow` | 0.85 |
| `Owner.Death` | 6.0 | | `ScriptedPlayers.SelfHealBelow` | 0.6 |
| | | | `ScriptedPlayers.HealerRange` | 30.0 |
| | | | `ScriptedPlayers.TauntRange` | 25.0 |
| | | | `ScriptedPlayers.RangedMin` | 20.0 |
| | | | `ScriptedPlayers.RangedMax` | 30.0 |
| | | | `ScriptedPlayers.StealthChance` | 50 |
| | | | `ScriptedPlayers.TacticsChance` | 75 |
| | | | `ScriptedPlayers.ControlMinMs` | 8000 |
| | | | `ScriptedPlayers.ControlMaxMs` | 15000 |
| | | | `ScriptedPlayers.DefensiveBelow` | 0.35 |
| | | | `ScriptedPlayers.BreakBelow` | 0.6 |

| Key | Default | | Key | Default |
|---|---|---|---|---|
| `Travel.ObjectiveMin` | 60.0 | | `Flag.BaseMin` | 100.0 |
| `Travel.ObjectiveMax` | 320.0 | | `Flag.BaseMax` | 180.0 |
| `Travel.FlyingMin` | 350.0 | | `Flag.CapturesToWin` | 3 |
| `Travel.FlyingMax` | 700.0 | | `Flag.RespawnMs` | 15000 |
| `Travel.Progress` | 1.0 | | `Flag.DroppedReturnMs` | 10000 |
| `Travel.Arrive` | 3.0 | | `Flag.TouchDistance` | 4.0 |
| `Travel.FastArrive` | 3.0 | | `Flag.Capture` | 5.0 |
| `Travel.DamageTaken` | 1.0 | | `Flag.Pickup` | 1.0 |
| `Travel.Death` | 3.0 | | `Flag.Return` | 1.0 |
| `Travel.StepCost` | 0.0002 | | `Flag.CarrierKill` | 1.5 |
| | | | `Flag.Lost` | 3.0 |
| | | | `Flag.Progress` | 0.5 |
| | | | `Flag.Death` | 1.0 |
| | | | `Flag.StepCost` | 0.0002 |

Arena weights: `Arena.<stage>.<arena>.Weight`, defaulting to the definition's weight.

## 8.3 Wire protocol (version 6)

A Unix domain stream socket. The sim is the server and the learner the client. All values are little-endian with no
padding. `src/Bridge/Protocol.h` and `python/animus/protocol.py` must change together, with `PROTOCOL_VERSION` bumped.

Every message is a header followed by `Length` payload bytes:

```
MsgHeader { u32 Type; u32 Length; }                                          8 bytes
Type: 1 HELLO, 2 SPEC, 3 STEP, 4 ACT, 5 CLOSE, 6 MODE, 7 WEIGHTS
```

**Sequence:** `HELLO → SPEC → STEP → (ACT → STEP | MODE → STEP | WEIGHTS)* → CLOSE`

| Message | Direction | Payload |
|---|---|---|
| `HELLO` | learner to sim | `u32 Version` |
| `SPEC` | sim to learner | `SpecMsg` (72 bytes), `u32 LayoutCount`, `LayoutMsg x LayoutCount` (56 bytes each), then the episode info column names as comma-separated ASCII filling the rest (no terminator) |
| `STEP` | sim to learner | See below |
| `ACT` | learner to sim | `i32 actions[E*A]` |
| `MODE` | learner to sim, instead of ACT | `ModeMsg` (48 bytes). The sim resets every env and answers with a fresh STEP |
| `WEIGHTS` | learner to sim, instead of ACT | `u32 Count`, `f32 Weight[Count]`: how often training episodes draw each layout, in SPEC layout order. The sim applies them as envs reset and answers nothing; the ACT follows |
| `CLOSE` | learner to sim, instead of ACT | Empty. The sim drops the client and waits for a new one |

```
SpecMsg   { u32 Version, NumEnvs, AgentsPerEnv, ObsDim, StateDim, NumActions, EpisodeInfoDim,
            TickMs, DecisionTicks, EpisodeSeconds; char Scenario[32]; }
LayoutMsg { u32 ObsDim, NumActions; char Name[48]; }
ModeMsg   { u32 Mode;          // 0 training, 1 evaluation
            u32 SeedBase;
            u32 Episodes;      // seeded evaluation episodes
            u32 Flags;         // 1 = MODE_FLAG_SCRIPTED_OPPONENTS
            char Baseline[32]; // scripted policy to run instead of the learner; empty = learner
          }
```

`ObsDim` and `NumActions` in `SpecMsg` are the largest layout's. The forge sends `TickMs = DecisionMs`,
`DecisionTicks = 1`, and `EpisodeSeconds` = the longest episode of any arena.

**STEP** payload, with E envs, A agents per env, O obs dim, S state dim, N actions, K episode info dim:

| Field | Type | Size | Meaning |
|---|---|---|---|
| decision | u64 | 1 | Decision counter |
| obs | f32 | E·A·O | Observation after any auto-reset. A layout fills only its first obs-dim features |
| state | f32 | E·S | Critic state after any auto-reset |
| mask | u8 | E·A·N | 1 = allowed. A layout fills only its first action-count entries |
| layout | u16 | E·A | Layout index into SPEC's layouts, constant per episode |
| present | u8 | E·A | 0 = an empty seat (only the no-op, reward 0, not a sample) |
| reward | f32 | E·A | Reward of the transition that just ended |
| done | u8 | E | The episode ended on this transition |
| terminated | u8 | E | Ended in a terminal state (no bootstrap). Done without terminated = truncation |
| final_obs | f32 | E·A·O | Last observation of the ended episode (valid if done) |
| final_state | f32 | E·S | Last state of the ended episode (valid if done) |
| episode_info | f32 | E·A·K | Per-agent totals of the ended episode (valid if done) |
| episode_seed | u32 | E | Evaluation seed index of the ended episode, `0xFFFFFFFF` for training |

The first STEP after SPEC, and the STEP answering a MODE, carry freshly reset envs with zero rewards and dones. Neither
is a transition. Every new session starts in training mode.

Evaluation episodes ignore `WEIGHTS`: seed index *i* plays layout *i % (layout count)*, so every class/role is scored
on an equal share of the seeds.

## 8.4 File formats

### `.amdl` (version 1)

```
char[4] "AMDL" | u32 version | u16 name_len | name | u32 obs_dim | u32 num_agents | u32 num_actions | u32 layer_count
per layer: u32 in_dim | u32 out_dim | f32 weight[out*in] (row-major) | f32 bias[out]
```

Input is the observation followed by a one-hot agent id (`num_agents` = 1 for exported class/role models). tanh follows
every layer but the last. The policy is the argmax of the logits over allowed actions.

### Layout manifest `<model>.json` (format 3)

Compact JSON: `format`, `model`, `stage`, `class_role`, `class`, `role`, `obs_dim`, `num_actions`, `specs` (talent tabs),
`blocks[]` each with `name`, `obs: [first, count]`, `actions: [first, count]` and block-specific entries (core:
`action_features`, `catalog[]` with `kind`, `first_rank` and `next_swing`, plus talents; duel: stable slots; pack:
tactical spells; gauntlet: sustain spells; companion: ally heals and revives; party: member slots). A consumer must build
a byte-identical manifest (trailing whitespace ignored).

### `stage.json` (format 2)

Written to `<OutputDir>/layouts/<stage>/stage.json` and copied into each run:

```json
{
  "format": 2, "stage": "stage4_companion", "suffix": "_companion", "extends": "stage3_gauntlet",
  "summary": "...", "seats": 1,
  "blocks": ["core", "duel", "pack", "gauntlet", "companion"],
  "arenas": [{"name": "companion", "weight": 1, "seats": 1, "episode_seconds": 60, "pvp": false, "ambushers": 0}],
  "seed_chain": ["stage3_gauntlet", "stage2_pack", "stage1_duel"],
  "merges": [],
  "state": {"arena_first": 13, "arena_count": 8},
  "models": {"warrior_tank": "warrior_tank_companion", "...": "..."},
  "layouts": {"warrior_tank": {"obs_dim": 519, "num_actions": 76,
              "blocks": [{"name": "core", "obs": [0, 374], "actions": [0, 45]},
                         {"name": "duel", "obs": [374, 30], "actions": [45, 15]}, "..."]}},
  "episode_info": ["damage", "dps", "..."],
  "tuning": {"Characters.HighLevelFirst": 61, "...": "..."}
}
```

### `finished.json`

`reason` (`converged`, `total_env_steps`, `below_target`, `budget_below_target`), `advanced`, `env_steps`, `update`,
`best_score`, `best_env_steps`, `restarts`, `judged` (`best`, `confirm`, or empty), `gates` (`passed`, `failures`,
`checks[]` with gate, value, required and passed, and `skipped`).

### `progress.json`

A flat object rewritten after every update and evaluation. Fields include:

- run: `run_name`, `scenario`, `total_env_steps`, `started_at`, `resumed_update`, `resumed_env_steps`
- evaluation settings: `eval_every`, `patience`, `min_env_steps`, `baseline`, `target`, `max_restarts`
- state: `phase` (`training`, `evaluating`, `finished`, `stopped`), `update`, `env_steps`, `updated_at`,
  `finish_reason`, `advanced`
- latest training metrics
- evaluation: `evals`, `last_eval_env_steps`, `last_eval_score`, `baseline_score`, `best_score`, `best_env_steps`,
  `evals_since_best`, `restarts`, `segment_env_steps`
- `nonfinite`: names of metrics written as null

## 8.5 Run directory

`<OutputDir>/runs/<scenario>/`:

| File | Written | Content |
|---|---|---|
| `config.yaml` | Start | The fully resolved learner config |
| `spec.json` | Connect | The SPEC |
| `stage.json` | Connect | The stage description (curriculum stages) |
| `metrics.csv` | Every `log_every` updates | Update, env steps, rates, reward per decision, episode count, `episode_<info>` means, losses, entropy, entropy coefficient, clip fraction, approx KL, `update_compute_seconds` (the update's own cost, which `update_seconds` stops measuring once `overlap_updates` is on), `explained_variance` (how much of the returns' spread the critic accounts for), actor and critic gradient norms before clipping, `epochs_run` (fewer than `mappo.epochs` when `target_kl` stopped the update), `allowed_actions` (mean legal actions per decision, which is what entropy has to be read against), `elapsed_seconds`, distillation stats |
| `tb/` | Same | TensorBoard events, if installed |
| `progress.json` | Every update and evaluation | For the console |
| `eval.csv` | Every evaluation | update, env_steps, policy, episodes, score, stderr, margin, best, evals_since_best, restarts, seconds |
| `eval.jsonl` | Every evaluation | The same plus the full summary (bands, layouts, arenas) |
| `eval_episodes.jsonl` | Every evaluation | One row per scored episode: update, env_steps, policy, seed, layout, return and the reported episode info |
| `eval_baseline.json` | Once per run | The baseline summary and its cache key |
| `eval_baseline_<seed>_<episodes>.json` | Confirmation | Baseline on the confirmation seeds |
| `stage.jsonl` | Each advance, restart or halt | Decision, reason, gates |
| `checkpoint_<update>.pt` | Every `checkpoint_every` | Newest `keep_checkpoints` kept |
| `latest.pt` | Checkpoints and finish | Resume point |
| `best.pt` | Each new best evaluation | Seed for later stages, export default |
| `finished.json` | When the stage is decided | See 8.4 |

Other locations:

- `<OutputDir>/runs/_archive/<scenario>-<time>/`: earlier runs moved aside by a fresh start
- `<OutputDir>/layouts/<stage>/`: manifests and `stage.json`
- `<OutputDir>/fast/{runs,layouts,models}/`: fast test runs
- `<LogsDir>/animus-learner.log`, `<LogsDir>/animus-export.log`

## 8.6 Bot account and name ranges

| Bot | Account id | Name |
|---|---|---|
| Forge seat | `0x7F000000 + env*8 + seat*2 + session` | `Forge<env>s<seat><a\|b>` |
| Owner | `0x7F000000 + 100000 + env*2 + session` | encounter-defined |
| Scripted opponent | `0x7F000000 + 300000 + env*2 + session` | encounter-defined |
| Ambusher | `0x7F000000 + 500000 + env*4 + ambusher*2 + session` | encounter-defined |
| Spell probe | `0x7F000000 - 1 - race` | |
| mod-animus companion | `0x7E000000 + n` | `Animus<n>` |

`env` is `Env::Id` (`StageSettings::FirstEnvId` + index). Names contain digits so they never collide with player names.

## 8.7 Exit codes

| Process | Code | Meaning |
|---|---|---|
| worldserver | 0 | Normal shutdown |
| worldserver | 1 | Error (config, databases) |
| worldserver | 2 | Restart requested |
| learner | 0 | Stage advanced (plan moves on) |
| learner | 3 | Below target (plan halts) |
| learner | other | Crash (plan holds, `forge resume` restarts it) |

## 8.8 Glossary

| Term | Meaning |
|---|---|
| **Arena** | One situation a stage's episodes can be, drawn by weight each episode |
| **Baseline** | A scripted policy (`greedy`, `fight`) scored on the evaluation seeds as a reference |
| **Block** | A group of observation features and actions (`core`, `duel`, ...) placed into layouts |
| **Class/role** | One trained model: a class in one role, over the specs that play it (`druid_tank`) |
| **Confirmation** | Re-scoring `best.pt` on held-out seeds before a stage advances |
| **CoreHooks** | animus-lib's function-pointer seams for forge-only core APIs |
| **Critic state** | The class-agnostic global env description the centralised critic sees |
| **Decision** | One environment step: score, reset, observe, act. On the forge, one world tick |
| **Distillation** | A decaying KL term pulling a merge stage's policy towards its parents on their arenas |
| **Encounter** | One part of an env besides the seats: creature, pulls, owner, party group, opponent, ambush |
| **Env** | One instance holding one copy of a training situation |
| **Env pool** | Every env of a scenario plus the flat buffers a host exchanges |
| **Episode info** | Per-seat totals reported when an episode ends |
| **Fast run** | `forge fast`: a quick, easier training run in a separate output directory |
| **Forge core** | The `forge` branch of AzerothCore: a fixed-tick, headless simulator |
| **Layout** | A class/role's exact observation and action layout at a stage |
| **Lock-step** | The sim sends observations and waits for actions every decision |
| **Manifest** | JSON describing a layout's meaning, exported beside its model |
| **Merge stage** | A stage that seeds blocks from further parents and distils their arenas |
| **Owner** | The scripted (training) or real (play) player that companions fight for |
| **Plan** | The forge module's list of scenarios to run one after another |
| **Seat** | One learned agent in an env, rebuilt as a new character every episode |
| **Seed chain** | A stage's ancestors, closest first, used to find what to seed from |
| **Segment** | The part of a run since its start or last restart, for convergence |
| **Sim group / sim session** | Core groups and sessions that exist only in memory |
| **Stage** | A curriculum scenario, extending an earlier stage |
| **Target** | The gates a stage's best networks must pass to advance |
| **Trunk** | The hidden layers shared by every layout in the actor and critic |
