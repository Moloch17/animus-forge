"""Stage controller decisions (move on, restart, halt) and the trainer hooks restarts use."""

import pytest
import torch

from animus.config import TrainConfig
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.stage import ADVANCE, CONTINUE, HALT, RESTART, StageController


def make_config(**target) -> TrainConfig:
    config = TrainConfig()
    config.eval.every_env_steps = 10
    config.eval.baseline = "fight"
    config.convergence.patience = 1
    config.convergence.min_improvement = 0.0
    config.convergence.min_improvement_abs = 0.5
    config.target.confirm_episodes = 0
    for key, value in target.items():
        setattr(config.target, key, value)
    return config


def evaluate(controller: StageController, score: float, env_steps: int, baseline: float = 10.0):
    controller.baseline_summary = {"score": baseline, "layouts": {}}
    controller.observe({"score": score, "episodes": 64, "layouts": {}}, env_steps)
    return controller.after_eval(env_steps, confirm=lambda: pytest.fail("no confirmation expected"))


def test_no_target_moves_on_once_converged():
    controller = StageController(make_config())
    assert evaluate(controller, 5.0, 0).action == CONTINUE
    outcome = evaluate(controller, 5.1, 10)
    assert (outcome.action, outcome.reason) == (ADVANCE, "converged")


def test_a_resumed_controller_judges_its_best_evaluation():
    controller = StageController(make_config(min_over_baseline=0.2))
    evaluate(controller, 13.0, 0)
    controller.record_restart(5)

    resumed = StageController(make_config(min_over_baseline=0.2))
    resumed.tracker.load_state_dict(controller.tracker.state_dict())
    resumed.load_state_dict(controller.state_dict())
    assert (resumed.restarts, resumed.restart_env_steps) == (1, 5)
    assert resumed.entropy_coef(5) == controller.entropy_coef(5)

    outcome = evaluate(resumed, 12.9, 20)
    assert (outcome.action, outcome.stage) == (ADVANCE, "best")


def test_converged_above_target_moves_on():
    controller = StageController(make_config(min_over_baseline=0.2))
    evaluate(controller, 13.0, 0)
    outcome = evaluate(controller, 12.9, 10)
    assert (outcome.action, outcome.stage) == (ADVANCE, "best")  # judged on the best evaluation, not the latest


def test_below_target_restarts_then_halts():
    config = make_config(min_over_baseline=0.2)
    config.restarts.max_restarts = 2
    controller = StageController(config)
    evaluate(controller, 11.0, 0)
    outcome = evaluate(controller, 10.9, 10)
    assert outcome.action == RESTART and outcome.gates.failures == ["score 11 (needs 12)"]

    controller.record_restart(10)
    assert controller.tracker.evals_since_best == 0 and controller.tracker.best == 11.0
    assert evaluate(controller, 10.0, 20).action == RESTART  # converged again in the new segment
    controller.record_restart(20)
    outcome = evaluate(controller, 10.0, 30)
    assert (outcome.action, outcome.reason) == (HALT, "below_target")


def test_restart_that_escapes_moves_on():
    config = make_config(min_over_baseline=0.2)
    controller = StageController(config)
    evaluate(controller, 11.0, 0)
    assert evaluate(controller, 10.9, 10).action == RESTART
    controller.record_restart(10)
    assert evaluate(controller, 12.5, 20).action == CONTINUE  # new best: not converged
    assert evaluate(controller, 12.4, 30).action == ADVANCE


def test_confirmation_on_held_out_seeds():
    config = make_config(min_over_baseline=0.2)
    config.target.confirm_episodes = 256
    controller = StageController(config)
    controller.baseline_summary = {"score": 10.0}
    controller.observe({"score": 13.0}, 0)
    controller.observe({"score": 12.0}, 10)

    calls = []

    def lucky_best():
        calls.append(1)
        return {"score": 11.0}, {"score": 10.0}

    outcome = controller.after_eval(10, lucky_best)
    assert calls and (outcome.action, outcome.stage) == (RESTART, "confirm")

    outcome = controller.at_budget(lambda: ({"score": 12.5}, {"score": 10.0}))
    assert (outcome.action, outcome.reason, outcome.stage) == (ADVANCE, "total_env_steps", "confirm")


def test_budget_below_target_halts_without_restarting():
    controller = StageController(make_config(min_over_baseline=0.2))
    controller.baseline_summary = {"score": 10.0}
    controller.observe({"score": 11.0}, 0)
    outcome = controller.at_budget(lambda: pytest.fail("gates failed on the best: no confirmation"))
    assert (outcome.action, outcome.reason) == (HALT, "budget_below_target")

    no_target = StageController(make_config())
    assert no_target.at_budget(lambda: pytest.fail("no target")).action == ADVANCE


def test_entropy_boost_decays_after_restart():
    config = make_config()
    config.mappo.entropy_coef = 0.01
    config.restarts.entropy_boost = 3.0
    config.restarts.entropy_half_life_env_steps = 100
    controller = StageController(config)
    assert controller.entropy_coef(50) == pytest.approx(0.01)
    controller.record_restart(1000)
    assert controller.entropy_coef(1000) == pytest.approx(0.03)
    assert controller.entropy_coef(1100) == pytest.approx(0.02)
    assert controller.entropy_coef(10_000) == pytest.approx(0.01, abs=1e-6)


def test_trainer_restart_hooks():
    torch.manual_seed(0)
    trainer = MappoTrainer([(3, 2), (4, 3)], 5, MappoConfig(hidden=(8,), entropy_coef=0.02))
    assert trainer.entropy_coef == 0.02

    before = [p.clone() for p in trainer.actor.parameters()]
    trainer.shrink_perturb(1.0, 0.0)
    assert all(torch.equal(a, b) for a, b in zip(before, trainer.actor.parameters()))
    trainer.shrink_perturb(0.5, 0.1)
    assert any(not torch.allclose(a * 0.5, b) for a, b in zip(before, trainer.actor.parameters()))
    rollout = dict(trainer._rollout_actor.named_parameters())
    assert all(torch.equal(p, rollout[name]) for name, p in trainer.actor.named_parameters())

    old_opt = trainer.actor_opt
    trainer.reset_optimizers()
    assert trainer.actor_opt is not old_opt and not trainer.actor_opt.state
