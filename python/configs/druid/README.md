# The druid's own floors

A run of a single class (`AnimusForge.Classes = "druid"`) takes its learner config from
`configs/druid/<stage>.yaml` where one exists, and from the shared `configs/<stage>.yaml` otherwise
(`ForgeConfig::LearnerConfigFor`). Only the stages whose floors are genuinely druid-shaped have a file here; the
rest inherit.

**Why a class needs its own floors at all.** A shared config cannot name a build, and a build is now the grain the
gates work at (`target.spec_metrics`). "Every tank" used to be one line; it cannot be, because a build gate names
builds and the shared file does not know which classes the run plays. That is a real loss of brevity and a gain in
precision: a feral cat and a balance druid were one role and one gate, and they are not equally hard to win with.

**The druid is four builds across what used to be three roles:**

| build | tree | what the floors are about |
|---|---|---|
| `balance` | 0 | ranged caster; the easiest of the four to win a duel with |
| `feral_cat` | 1 | melee damage, and the only one that can open from Prowl |
| `feral_bear` | 1 | holds the pull; kills slowly, so the clock is its risk |
| `restoration` | 2 | heals; the weakest solo and it still has to pass |

`feral_cat` and `feral_bear` share a talent tree, which is exactly the case a role could not separate and the
reason the ladder and the weights are keyed on the build.

**The floors here are first-pass.** A failed gate halts the queue, so they ask "did this stage learn its lesson at
all", not "is it good". Raise them once a full pass has produced real numbers.
