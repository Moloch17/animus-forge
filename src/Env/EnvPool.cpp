/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "EnvPool.h"
#include "ForgeConfig.h"
#include "Protocol.h"
#include "Common.h"
#include "Log.h"
#include "Random.h"
#include "RandomSeed.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "Unit.h"
#include <algorithm>

AnimusForge::EnvPool::EnvPool(Scenario& scenario, ForgeConfig const& config)
    : _scenario(scenario), _spec(scenario.Spec()), _episodeLengthMs(config.EpisodeSeconds * IN_MILLISECONDS),
    _reportEpisodes(config.ReportEpisodes)
{
    uint32 const envs = config.Envs;
    uint32 const agents = envs * _spec.AgentsPerEnv;

    if (_spec.Layouts.empty())
        _spec.Layouts.push_back(LayoutSpec{ scenario.Name(), _spec.ObsDim, _spec.NumActions });

    _envs.resize(envs);
    for (uint32 i = 0; i < envs; ++i)
    {
        _envs[i].Index = i;
        _envs[i].EpisodeLengthMs = _episodeLengthMs;
        _envs[i].StepStats.resize(_spec.AgentsPerEnv);
        _envs[i].EpisodeStats.resize(_spec.AgentsPerEnv);
    }

    Obs.assign(agents * _spec.ObsDim, 0.0f);
    State.assign(envs * _spec.StateDim, 0.0f);
    Mask.assign(agents * _spec.NumActions, 0);
    Rewards.assign(agents, 0.0f);
    Done.assign(envs, 0);
    Terminated.assign(envs, 0);
    FinalObs.assign(agents * _spec.ObsDim, 0.0f);
    FinalState.assign(envs * _spec.StateDim, 0.0f);
    EpisodeInfo.assign(agents * _spec.EpisodeInfoDim, 0.0f);
    Layout.assign(agents, 0);
    EpisodeSeed.assign(envs, NO_EPISODE_SEED);
    _envSeed.assign(envs, NO_EPISODE_SEED);
    Actions.assign(agents, 0);

    _scratchMask.assign(_spec.AgentsPerEnv * _spec.NumActions, 0);
    _reportInfoSum.assign(_spec.EpisodeInfoDim, 0.0);
}

bool AnimusForge::EnvPool::Setup()
{
    for (Env& env : _envs)
    {
        if (!_scenario.Setup(env) || env.Bots.size() != _spec.AgentsPerEnv)
        {
            LOG_ERROR("module.animus", "Scenario {} failed to set up env {}", _scenario.Name(), env.Index);
            return false;
        }

        for (uint32 agent = 0; agent < env.Bots.size(); ++agent)
            _agents[env.Bots[agent]] = AgentSlot{ env.Index, agent };

        for (uint32 ally = 0; ally < env.Allies.size() && ally < MAX_ALLIES; ++ally)
            _allies[env.Allies[ally]] = AgentSlot{ env.Index, ally };
    }

    LOG_INFO("module.animus", "Scenario {}: {} envs x {} agents, obs {}, state {}, actions {}", _scenario.Name(),
        _envs.size(), _spec.AgentsPerEnv, _spec.ObsDim, _spec.StateDim, _spec.NumActions);

    return true;
}

void AnimusForge::EnvPool::Teardown()
{
    _agents.clear();
    _allies.clear();

    for (Env& env : _envs)
        _scenario.Teardown(env);
}

uint64 AnimusForge::EnvPool::CompletedEpisodes() const
{
    uint64 episodes = 0;
    for (Env const& env : _envs)
        episodes += env.EpisodesCompleted;

    return episodes;
}

void AnimusForge::EnvPool::AdvanceClock(uint32 diff)
{
    for (Env& env : _envs)
        env.EpisodeElapsedMs += diff;
}

void AnimusForge::EnvPool::ResetAll()
{
    for (Env& env : _envs)
    {
        ResetEnv(env);

        uint32 const e = env.Index;
        _scenario.Observe(env, &Obs[e * _spec.AgentsPerEnv * _spec.ObsDim], &State[e * _spec.StateDim],
            &Mask[e * _spec.AgentsPerEnv * _spec.NumActions]);
        _scenario.AgentLayouts(env, &Layout[e * _spec.AgentsPerEnv]);
    }

    std::fill(Rewards.begin(), Rewards.end(), 0.0f);
    std::fill(Done.begin(), Done.end(), 0);
    std::fill(Terminated.begin(), Terminated.end(), 0);
}

void AnimusForge::EnvPool::Collect()
{
    uint32 const agentsPerEnv = _spec.AgentsPerEnv;

    for (Env& env : _envs)
    {
        uint32 const e = env.Index;

        _scenario.Reward(env, &Rewards[e * agentsPerEnv]);

        for (uint32 agent = 0; agent < agentsPerEnv; ++agent)
        {
            env.EpisodeStats[agent].Add(env.StepStats[agent]);
            env.StepStats[agent] = AgentStats();
        }

        bool const terminal = _scenario.IsTerminal(env);
        bool const done = terminal || env.EpisodeElapsedMs >= env.EpisodeLengthMs;
        Done[e] = done ? 1 : 0;
        Terminated[e] = terminal ? 1 : 0;

        if (done)
        {
            _scenario.Observe(env, &FinalObs[e * agentsPerEnv * _spec.ObsDim], &FinalState[e * _spec.StateDim],
                _scratchMask.data());
            _scenario.EpisodeInfo(env, &EpisodeInfo[e * agentsPerEnv * _spec.EpisodeInfoDim]);
            EpisodeSeed[e] = _envSeed[e];

            ++env.EpisodesCompleted;
            ReportEpisode(e);
            ResetEnv(env);
        }

        _scenario.Observe(env, &Obs[e * agentsPerEnv * _spec.ObsDim], &State[e * _spec.StateDim],
            &Mask[e * agentsPerEnv * _spec.NumActions]);
        _scenario.AgentLayouts(env, &Layout[e * agentsPerEnv]);
    }
}

bool AnimusForge::EnvPool::ChooseLocalActions(std::string const& policy)
{
    uint32 const agents = NumEnvs() * _spec.AgentsPerEnv;
    uint32 const numActions = _spec.NumActions;

    for (uint32 i = 0; i < agents; ++i)
    {
        float const* obs = &Obs[i * _spec.ObsDim];
        uint8 const* mask = &Mask[i * numActions];

        if (policy == "random")
        {
            uint32 allowed = 0;
            for (uint32 a = 0; a < numActions; ++a)
                allowed += mask[a];

            // Uniform over unmasked actions; action 0 if the scenario masked everything.
            int32 chosen = 0;
            if (allowed)
            {
                uint32 pick = urand(0, allowed - 1);
                for (uint32 a = 0; a < numActions; ++a)
                {
                    if (!mask[a])
                        continue;

                    if (pick-- == 0)
                    {
                        chosen = static_cast<int32>(a);
                        break;
                    }
                }
            }

            Actions[i] = chosen;
        }
        else if (!_scenario.ScriptedAction(policy, obs, mask, Layout[i], Actions[i]))
            return false;
    }

    return true;
}

void AnimusForge::EnvPool::SetEvaluation(bool enabled, uint32 seedBase, uint32 episodes, std::string const& baseline)
{
    _evaluating = enabled;
    _evalSeedBase = seedBase;
    _evalEpisodes = enabled ? episodes : 0;
    _evalNextSeed = 0;
    _evalBaseline = enabled ? baseline : std::string();
}

void AnimusForge::EnvPool::ApplyActions()
{
    for (Env& env : _envs)
        _scenario.ApplyActions(env, &Actions[env.Index * _spec.AgentsPerEnv]);
}

void AnimusForge::EnvPool::RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type)
{
    if (!attacker || !victim || !damage || (type != DIRECT_DAMAGE && type != SPELL_DIRECT_DAMAGE && type != DOT))
        return;

    // Damage an agent takes. The victim is the agent itself (pets absorb their own damage).
    if (auto const hit = _agents.find(victim->GetGUID()); hit != _agents.end())
        _envs[hit->second.Env].StepStats[hit->second.Agent].DamageTaken += damage;

    // Damage an ally takes counts against every agent of its env.
    if (auto const ally = _allies.find(victim->GetGUID()); ally != _allies.end())
    {
        for (AgentStats& stats : _envs[ally->second.Env].StepStats)
        {
            stats.AllyDamageTaken += damage;
            stats.AllyDamageTakenBy[ally->second.Agent] += damage;
        }
    }

    // Pets, guardians and totems deal damage for their owner.
    auto const itr = _agents.find(attacker->GetCharmerOrOwnerOrOwnGUID());
    if (itr == _agents.end())
        return;

    // Damage counts on the env's targets, and on its other agents (self-play).
    Env& env = _envs[itr->second.Env];
    auto const victimAgent = _agents.find(victim->GetGUID());
    bool const onOtherAgent = victimAgent != _agents.end() && victimAgent->second.Env == itr->second.Env
        && victimAgent->second.Agent != itr->second.Agent;
    if (!onOtherAgent && std::find(env.Targets.begin(), env.Targets.end(), victim->GetGUID()) == env.Targets.end())
        return;

    AgentStats& stats = env.StepStats[itr->second.Agent];
    stats.Damage += damage;

    if (type == DIRECT_DAMAGE)
    {
        stats.WhiteDamage += damage;
        ++stats.WhiteHits;
    }
    else
    {
        stats.SpecialDamage += damage;
        ++stats.SpecialHits;
    }
}

void AnimusForge::EnvPool::ResetEnv(Env& env)
{
    env.EpisodeElapsedMs = 0;

    std::vector<ObjectGuid> const previousBots = env.Bots;
    std::vector<ObjectGuid> const previousAllies = env.Allies;

    // An evaluation episode is built from its seed: the world thread's random numbers restart from it for
    // the reset (race, level, spec, talents, gear, opponents, spawn points) and go back to entropy afterwards.
    // Resets run on the world thread, and everything a scenario rolls there comes from those numbers.
    _envSeed[env.Index] = NO_EPISODE_SEED;
    if (_evaluating && _evalNextSeed < _evalEpisodes)
    {
        uint32 const index = _evalNextSeed++;
        uint32 seed = (_evalSeedBase + 1) * 2654435761u ^ (index + 1) * 2246822519u;
        rand_seed(seed ? seed : 1);
        _envSeed[env.Index] = index;
    }

    _scenario.Reset(env);

    if (_envSeed[env.Index] != NO_EPISODE_SEED)
        rand_seed(0);

    // After the reset: tearing down the old character (a cast cut short, its pet's last hit) still reports
    // to the hooks, and none of that belongs to the new episode.
    for (uint32 agent = 0; agent < _spec.AgentsPerEnv; ++agent)
    {
        env.StepStats[agent] = AgentStats();
        env.EpisodeStats[agent] = AgentStats();
    }

    // A scenario may rebuild its bots and allies on reset. Safe to update here: resets run on the world
    // thread while no map is updating, so no damage or heal hook is reading the maps.
    if (env.Bots != previousBots)
    {
        for (ObjectGuid const& guid : previousBots)
            _agents.erase(guid);

        for (uint32 agent = 0; agent < env.Bots.size(); ++agent)
            _agents[env.Bots[agent]] = AgentSlot{ env.Index, agent };
    }

    if (env.Allies != previousAllies)
    {
        for (ObjectGuid const& guid : previousAllies)
            _allies.erase(guid);

        for (uint32 ally = 0; ally < env.Allies.size() && ally < MAX_ALLIES; ++ally)
            _allies[env.Allies[ally]] = AgentSlot{ env.Index, ally };
    }
}

void AnimusForge::EnvPool::RecordHeal(Unit const* healer, Unit const* receiver, uint32 gain)
{
    if (!healer || !receiver || !gain)
        return;

    // Healing on another agent of the same env (a teammate).
    if (auto const patient = _agents.find(receiver->GetGUID()); patient != _agents.end())
    {
        auto const agent = _agents.find(healer->GetCharmerOrOwnerOrOwnGUID());
        if (agent != _agents.end() && agent->second.Env == patient->second.Env
            && agent->second.Agent != patient->second.Agent && patient->second.Agent < MAX_AGENTS)
            _envs[agent->second.Env].StepStats[agent->second.Agent].AgentHealingBy[patient->second.Agent] += gain;
        return;
    }

    auto const ally = _allies.find(receiver->GetGUID());
    if (ally == _allies.end())
        return;

    // Pets and totems heal for their owner.
    auto const agent = _agents.find(healer->GetCharmerOrOwnerOrOwnGUID());
    if (agent == _agents.end() || agent->second.Env != ally->second.Env)
        return;

    AgentStats& stats = _envs[agent->second.Env].StepStats[agent->second.Agent];
    stats.AllyHealing += gain;
    stats.AllyHealingBy[ally->second.Agent] += gain;
}

void AnimusForge::EnvPool::RecordCastCompleted(Unit const* caster, Spell* spell)
{
    if (!caster || !spell || spell->IsTriggered() || spell->GetCastTime() <= 0 || spell->m_spellInfo->IsChanneled())
        return;

    auto const agent = _agents.find(caster->GetGUID());
    if (agent == _agents.end())
        return;

    AgentStats& stats = _envs[agent->second.Env].StepStats[agent->second.Agent];
    ++stats.CastsCompleted;
    stats.CastMsCompleted += uint32(spell->GetCastTime());
}

void AnimusForge::EnvPool::RecordCastCancelled(Unit const* caster, Spell* spell)
{
    // Only a cast still in its cast time: a cancelled channel has already paid out its ticks.
    if (!caster || !spell || spell->getState() != SPELL_STATE_PREPARING || spell->IsTriggered()
        || spell->GetCastTime() <= 0)
        return;

    auto const agent = _agents.find(caster->GetGUID());
    if (agent == _agents.end())
        return;

    // Pushback adds to the time left, so clamp what was spent to [0, cast time].
    int32 const spent = std::clamp(spell->GetCastTime() - spell->GetCastTimeRemaining(), 0, spell->GetCastTime());

    AgentStats& stats = _envs[agent->second.Env].StepStats[agent->second.Agent];
    ++stats.CastsCancelled;
    stats.CastMsWasted += uint32(spent);
}

void AnimusForge::EnvPool::ReportEpisode(uint32 envIndex)
{
    // Every agent's episode counts as one.
    for (uint32 agent = 0; agent < _spec.AgentsPerEnv; ++agent)
    {
        float const* info = &EpisodeInfo[(envIndex * _spec.AgentsPerEnv + agent) * _spec.EpisodeInfoDim];
        for (uint32 i = 0; i < _spec.EpisodeInfoDim; ++i)
            _reportInfoSum[i] += info[i];
    }

    _reportedEpisodes += _spec.AgentsPerEnv;
    if (_reportedEpisodes < _reportEpisodes)
        return;

    std::vector<std::string> const names = _scenario.EpisodeInfoNames();

    std::string line;
    for (uint32 i = 0; i < _spec.EpisodeInfoDim; ++i)
    {
        if (i)
            line += ", ";

        std::string const name = i < names.size() ? names[i] : "info";
        line += Acore::StringFormat("{} {:.2f}", name, _reportInfoSum[i] / _reportedEpisodes);
    }

    LOG_INFO("module.animus", "Episodes {} (mean): {}", _reportedEpisodes, line);

    std::fill(_reportInfoSum.begin(), _reportInfoSum.end(), 0.0);
    _reportedEpisodes = 0;
}
