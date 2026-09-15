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

#include "Progress.h"
#include "ForgeConfig.h"
#include "StringFormat.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>

namespace
{
    /// Weight of the newest interval in the step rate average.
    constexpr double RATE_EMA_ALPHA = 0.3;

    /// Warn when the learner has not answered a STEP for this long.
    constexpr double STALL_SECONDS = 120.0;

    /// Warn when the step rate of the last interval falls this far below the run's average.
    constexpr double RATE_DROP_FRACTION = 0.30;

    /// Warn when entropy falls under this fraction of its first reported value.
    constexpr double ENTROPY_COLLAPSE_FRACTION = 0.25;

    /// Warn when the policy moves this much per update.
    constexpr double APPROX_KL_LIMIT = 0.05;
    constexpr double CLIP_FRACTION_LIMIT = 0.30;

    /// Clock slack when telling this run's progress.json from an earlier run's, in seconds.
    constexpr double STALE_SLACK = 5.0;

    /// Warn when the best score is still under the baseline after this many evaluations.
    constexpr double BELOW_BASELINE_EVALS = 2.0;

    /// Episode means shown in the report, when the scenario has them, in this order.
    constexpr char const* EPISODE_COLUMNS[] =
    {
        "dps", "killed", "died", "time_to_kill", "damage_taken", "kills", "pulls_cleared", "wipes", "owner_deaths",
        "healing", "owner_healing",
    };

    double UnixNow()
    {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /// "~14:32" today, "~Tue 14:32" within a week, "~Sep 21 14:32" after that.
    std::string ClockAfter(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0)
            return "";

        std::time_t const at = std::time(nullptr) + std::time_t(seconds);
        std::tm local{};
        localtime_r(&at, &local);

        char buffer[32];
        char const* format = seconds < 20 * 3600 ? "~%H:%M" : seconds < 6 * 86400 ? "~%a %H:%M" : "~%b %d %H:%M";
        std::strftime(buffer, sizeof(buffer), format, &local);
        return buffer;
    }

    /// "+4.0%" change from `before` to `now`, or "" when either is missing.
    std::string Change(std::optional<double> now, std::optional<double> before)
    {
        if (!now || !before || !std::isfinite(*now) || !std::isfinite(*before) || std::abs(*before) < 1e-12)
            return "";

        return AnimusForge::Format::Percent((*now - *before) / std::abs(*before), true) + " vs last";
    }

    void SkipSpace(std::string const& text, std::size_t& at)
    {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
            ++at;
    }

    bool ParseString(std::string const& text, std::size_t& at, std::string& out)
    {
        if (at >= text.size() || text[at] != '"')
            return false;

        ++at;
        out.clear();
        while (at < text.size() && text[at] != '"')
        {
            char c = text[at++];
            if (c == '\\' && at < text.size())
            {
                char const escaped = text[at++];
                switch (escaped)
                {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u':
                        // Scenario and run names are ASCII; anything else is replaced.
                        at = std::min(text.size(), at + 4);
                        c = '?';
                        break;
                    default: c = escaped; break;
                }
            }
            out += c;
        }

        if (at >= text.size())
            return false;

        ++at;
        return true;
    }
}

bool AnimusForge::ProgressFile::Load(std::filesystem::path const& path)
{
    std::ifstream file(path);
    if (!file)
        return false;

    std::stringstream buffer;
    buffer << file.rdbuf();
    return Parse(buffer.str());
}

bool AnimusForge::ProgressFile::Parse(std::string const& text)
{
    _numbers.clear();
    _strings.clear();

    std::size_t at = 0;
    SkipSpace(text, at);
    if (at >= text.size() || text[at] != '{')
        return false;

    ++at;
    SkipSpace(text, at);
    if (at < text.size() && text[at] == '}')
        return true;

    while (at < text.size())
    {
        std::string key;
        SkipSpace(text, at);
        if (!ParseString(text, at, key))
            return false;

        SkipSpace(text, at);
        if (at >= text.size() || text[at] != ':')
            return false;

        ++at;
        SkipSpace(text, at);
        if (at >= text.size())
            return false;

        if (text[at] == '"')
        {
            std::string value;
            if (!ParseString(text, at, value))
                return false;

            _strings[key] = value;
        }
        else if (text.compare(at, 4, "null") == 0)
            at += 4;
        else if (text.compare(at, 4, "true") == 0)
        {
            _numbers[key] = 1.0;
            at += 4;
        }
        else if (text.compare(at, 5, "false") == 0)
        {
            _numbers[key] = 0.0;
            at += 5;
        }
        else
        {
            char* end = nullptr;
            double const value = std::strtod(text.c_str() + at, &end);
            if (end == text.c_str() + at)
                return false;

            _numbers[key] = value;
            at = std::size_t(end - text.c_str());
        }

        SkipSpace(text, at);
        if (at < text.size() && text[at] == ',')
        {
            ++at;
            continue;
        }

        return at < text.size() && text[at] == '}';
    }

    return false;
}

std::optional<double> AnimusForge::ProgressFile::Number(std::string const& key) const
{
    auto const itr = _numbers.find(key);
    if (itr == _numbers.end())
        return std::nullopt;

    return itr->second;
}

std::string AnimusForge::ProgressFile::Text(std::string const& key) const
{
    auto const itr = _strings.find(key);
    return itr == _strings.end() ? std::string() : itr->second;
}

std::filesystem::path AnimusForge::ProgressPath(ForgeConfig const& config, std::string const& scenario)
{
    return config.RunsDir() / scenario / "progress.json";
}

std::optional<uint64> AnimusForge::ConfiguredTotalEnvSteps(ForgeConfig const& config, std::string const& scenario)
{
    std::optional<uint64> total;

    std::ifstream file(config.LearnerConfigFor(scenario));
    std::regex const key(R"(^total_env_steps:\s*([0-9_]+))");
    for (std::string line; std::getline(file, line);)
    {
        std::smatch match;
        if (std::regex_search(line, match, key))
        {
            std::string digits = match[1].str();
            digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
            total = std::strtoull(digits.c_str(), nullptr, 10);
        }
    }

    // `--set total_env_steps=N` in AnimusForge.Learner.Args wins, as it does in the learner.
    for (std::size_t i = 0; i + 1 < config.LearnerArgs.size(); ++i)
        if (config.LearnerArgs[i] == "--set" && config.LearnerArgs[i + 1].starts_with("total_env_steps="))
            total = std::strtoull(config.LearnerArgs[i + 1].c_str() + sizeof("total_env_steps=") - 1, nullptr, 10);

    return total;
}

void AnimusForge::ProgressMonitor::Begin(std::string const& scenario)
{
    _scenario = scenario;
    _lastSample.reset();
    _rateEma.reset();
    _peakRate.reset();
    _firstEntropy.reset();
    _previous = {};
}

std::optional<double> AnimusForge::ProgressMonitor::StepRate(ProgressFile const& progress) const
{
    if (_rateEma)
        return _rateEma;

    // Before two samples: the average since the learner started, evaluations and updates included.
    std::optional<double> const steps = progress.Number("env_steps");
    std::optional<double> const resumed = progress.Number("resumed_env_steps");
    std::optional<double> const started = progress.Number("started_at");
    std::optional<double> const updated = progress.Number("updated_at");
    if (steps && started && updated && *updated - *started > 1.0 && *steps > resumed.value_or(0.0))
        return (*steps - resumed.value_or(0.0)) / (*updated - *started);

    return progress.Number("env_steps_per_sec");
}

void AnimusForge::ProgressMonitor::Advance(ProgressFile const& progress)
{
    std::optional<double> const steps = progress.Number("env_steps");
    std::optional<double> const updated = progress.Number("updated_at");
    if (!steps || !updated)
        return;

    if (_lastSample && *updated > _lastSample->UnixTime && *steps > _lastSample->EnvSteps)
    {
        double const rate = (*steps - _lastSample->EnvSteps) / (*updated - _lastSample->UnixTime);
        _rateEma = _rateEma ? RATE_EMA_ALPHA * rate + (1.0 - RATE_EMA_ALPHA) * *_rateEma : rate;
        _peakRate = std::max(_peakRate.value_or(0.0), *_rateEma);
    }

    if (!_lastSample || *steps != _lastSample->EnvSteps)
        _lastSample = Sample{ *updated, *steps };

    if (!_firstEntropy)
        _firstEntropy = progress.Number("entropy");

    _previous.Reward = progress.Number("reward_per_decision");
    _previous.Entropy = progress.Number("entropy");
    _previous.Rate = StepRate(progress);
    _previous.Score = progress.Number("last_eval_score");
}

void AnimusForge::ProgressMonitor::Report(ForgeConfig const& config, SimSnapshot const& sim,
    std::vector<PlanRow> const& plan, LineSink const& info, LineSink const& warn, bool periodic)
{
    if (sim.Scenario != _scenario)
        Begin(sim.Scenario);

    // A progress.json older than this scenario is the previous run's, left until the new learner archives it (or
    // until a learner started by hand connects): not this run's progress.
    ProgressFile progress;
    bool haveProgress = sim.Remote && progress.Load(ProgressPath(config, sim.Scenario));
    if (haveProgress && progress.Number("started_at").value_or(0.0) < UnixNow() - sim.ScenarioSeconds - STALE_SLACK)
        haveProgress = false;

    std::vector<std::string> warnings;

    if (sim.Remote)
    {
        // A run's rate warnings compare the newest interval with the average before it, so look before Advance.
        if (periodic && haveProgress && _rateEma && _lastSample)
        {
            std::optional<double> const steps = progress.Number("env_steps");
            std::optional<double> const updated = progress.Number("updated_at");
            if (steps && updated && *updated > _lastSample->UnixTime && *steps > _lastSample->EnvSteps)
            {
                double const rate = (*steps - _lastSample->EnvSteps) / (*updated - _lastSample->UnixTime);
                if (rate < (1.0 - RATE_DROP_FRACTION) * *_rateEma)
                    warnings.push_back(Acore::StringFormat("Step rate dropped to {} steps/s, {} below the run's "
                        "average of {}", Format::Count(uint64(rate)), Format::Percent(1.0 - rate / *_rateEma),
                        Format::Count(uint64(*_rateEma))));
            }
        }

        ReportTraining(config, sim, haveProgress ? &progress : nullptr, info, warnings);
    }
    else
        ReportLocal(sim, info);

    if (plan.size() > 1)
        ReportPlan(config, plan, haveProgress ? &progress : nullptr, info);

    for (std::string const& warning : warnings)
        warn("Warning: " + warning);

    if (periodic && haveProgress)
        Advance(progress);

    if (periodic)
        _previous.TicksPerSecond = sim.TicksPerSecond;
}

void AnimusForge::ProgressMonitor::ReportTraining(ForgeConfig const& config, SimSnapshot const& sim,
    ProgressFile const* progress, LineSink const& info, std::vector<std::string>& warnings) const
{
    std::string header = Acore::StringFormat("Forge: {}", sim.Scenario);
    if (sim.PlanSize > 1)
        header += Acore::StringFormat(" ({} of {})", sim.PlanPosition, sim.PlanSize);
    header += " | " + sim.State;
    if (progress)
    {
        std::string const phase = progress->Text("phase");
        if (!phase.empty() && phase != "training")
            header += " | learner " + phase;
        if (std::optional<double> const update = progress->Number("update"))
            header += Acore::StringFormat(" | update {}", Format::Count(uint64(*update)));
    }
    header += " | " + Format::Duration(sim.ScenarioSeconds);
    info(header);

    TextTable table({ { "Metric" }, { "Value", TextTable::Align::Right }, { "Note" } });

    std::string learner = sim.LearnerConnected ? "connected" : sim.LearnerRunning ? "starting" : "not connected";
    std::string learnerNote;
    if (sim.LearnerPid > 0)
        learnerNote = Acore::StringFormat("pid {}", sim.LearnerPid);
    if (sim.LearnerConnected && sim.SecondsSinceAct >= 0)
        learnerNote += Acore::StringFormat("{}last answer {} ago", learnerNote.empty() ? "" : ", ",
            Format::Duration(sim.SecondsSinceAct));
    if (!sim.LearnerRunning && !sim.LearnerConnected && !config.LearnerAutoStart)
        learnerNote = "start it by hand (AnimusForge.Learner.AutoStart = 0)";
    table.AddRow({ "learner", learner, learnerNote });

    if (sim.LearnerConnected && sim.SecondsSinceAct > STALL_SECONDS)
        warnings.push_back(Acore::StringFormat("The learner has not answered for {} (stalled, or a very long update "
            "or evaluation); see {}", Format::Duration(sim.SecondsSinceAct), config.LearnerLogFile));

    if (sim.LearnerFailed && !sim.LearnerConnected)
        warnings.push_back(Acore::StringFormat("The learner exited unexpectedly; see {}. `forge resume` continues from "
            "its last checkpoint, `forge cancel` stops the plan", config.LearnerLogFile));

    table.AddRow({ "sim", Acore::StringFormat("{} ticks/s", Format::Count(uint64(sim.TicksPerSecond))),
        Acore::StringFormat("{} envs x {} agents, {} decisions", sim.Envs, sim.AgentsPerEnv,
            Format::Count(sim.Decisions)) });

    if (!progress)
    {
        table.AddRow({ "progress", "-", "no progress.json yet (the learner writes it after connecting)" });
        table.Write(info, "  ");
        return;
    }

    std::optional<double> const steps = progress->Number("env_steps");
    std::optional<double> const total = progress->Number("total_env_steps");
    std::optional<double> const rate = StepRate(*progress);

    if (steps && total && *total > 0)
        table.AddRow({ "env steps", Format::Compact(*steps) + " / " + Format::Compact(*total),
            Format::Percent(std::min(1.0, *steps / *total)) });

    if (rate)
        table.AddRow({ "step rate", Format::Count(uint64(*rate)) + " steps/s",
            _rateEma ? Change(rate, _previous.Rate) : "average since start" });

    std::optional<double> etaSteps;
    if (steps && total && rate && *rate > 0)
    {
        etaSteps = std::max(0.0, *total - *steps) / *rate;
        table.AddRow({ "ETA (step limit)", Format::Duration(*etaSteps), ClockAfter(*etaSteps) });
    }

    std::optional<double> const patience = progress->Number("patience");
    std::optional<double> const every = progress->Number("eval_every");
    std::optional<double> const sinceBest = progress->Number("evals_since_best");
    std::optional<double> const evals = progress->Number("evals");
    if (steps && total && rate && *rate > 0 && patience && *patience > 0 && every && *every > 0)
    {
        double const needed = std::max(1.0, *patience - sinceBest.value_or(0.0));
        double stopAt = progress->Number("last_eval_env_steps").value_or(0.0) + needed * *every;
        while (stopAt < progress->Number("min_env_steps").value_or(0.0))
            stopAt += *every;

        if (stopAt < *total)
        {
            double const eta = std::max(0.0, stopAt - *steps) / *rate;
            table.AddRow({ "ETA (plateau, earliest)", Format::Duration(eta),
                Acore::StringFormat("at {} if {} more eval{} bring{} no improvement", Format::Compact(stopAt),
                    uint32(needed), needed == 1 ? "" : "s", needed == 1 ? "s" : "") });
        }

        if (sinceBest && *sinceBest > 0 && *patience - *sinceBest <= 1 && evals && *evals > 0)
            warnings.push_back(Acore::StringFormat("Plateau: {} of {} evaluations without improvement; the next one "
                "stops the run unless the score improves", uint32(*sinceBest), uint32(*patience)));
    }

    std::optional<double> const score = progress->Number("last_eval_score");
    std::optional<double> const best = progress->Number("best_score");
    std::optional<double> const baselineScore = progress->Number("baseline_score");
    if (evals && *evals > 0)
    {
        std::string note = Acore::StringFormat("{} evals", uint32(*evals));
        if (best)
            note += Acore::StringFormat(", best {} at {}", Format::Metric(*best),
                Format::Compact(progress->Number("best_env_steps").value_or(0.0)));
        if (sinceBest && patience && *patience > 0)
            note += Acore::StringFormat(", {}/{} without improvement", uint32(*sinceBest), uint32(*patience));
        table.AddRow({ "eval score", Format::OrDash(score, Format::Metric), note });

        if (baselineScore && best && std::abs(*baselineScore) > 1e-9)
        {
            table.AddRow({ "best vs baseline", Format::Percent((*best - *baselineScore) / std::abs(*baselineScore),
                true), Acore::StringFormat("{} {}", progress->Text("baseline"), Format::Metric(*baselineScore)) });

            if (*evals >= BELOW_BASELINE_EVALS && *best < *baselineScore)
                warnings.push_back(Acore::StringFormat("Best evaluation score {} is still below the {} baseline's {} "
                    "after {} evaluations", Format::Metric(*best), progress->Text("baseline"),
                    Format::Metric(*baselineScore), uint32(*evals)));
        }
    }
    else if (every && *every > 0 && steps)
        table.AddRow({ "eval score", "-", Acore::StringFormat("first evaluation at {}",
            Format::Compact(std::ceil((*steps + 1) / *every) * *every)) });

    std::optional<double> const reward = progress->Number("reward_per_decision");
    if (reward)
        table.AddRow({ "reward/decision", Format::Metric(*reward), Change(reward, _previous.Reward) });

    std::optional<double> const entropy = progress->Number("entropy");
    if (entropy)
    {
        std::string note = Change(entropy, _previous.Entropy);
        if (_firstEntropy && *_firstEntropy > 0)
        {
            note += Acore::StringFormat("{}{} of start", note.empty() ? "" : ", ",
                Format::Percent(*entropy / *_firstEntropy));

            if (*entropy < ENTROPY_COLLAPSE_FRACTION * *_firstEntropy)
                warnings.push_back(Acore::StringFormat("Entropy {} is under {} of its starting {}: the policy may "
                    "have collapsed", Format::Metric(*entropy), Format::Percent(ENTROPY_COLLAPSE_FRACTION),
                    Format::Metric(*_firstEntropy)));
        }
        table.AddRow({ "entropy", Format::Metric(*entropy), note });
    }

    if (std::optional<double> const valueLoss = progress->Number("value_loss"))
        table.AddRow({ "value loss", Format::Metric(*valueLoss), "" });

    std::optional<double> const kl = progress->Number("approx_kl");
    std::optional<double> const clip = progress->Number("clip_frac");
    if (kl || clip)
        table.AddRow({ "approx KL / clip", Format::OrDash(kl, Format::Metric) + " / " + Format::OrDash(clip,
            [](double v) { return Format::Percent(v); }), "" });

    if ((kl && *kl > APPROX_KL_LIMIT) || (clip && *clip > CLIP_FRACTION_LIMIT))
        warnings.push_back(Acore::StringFormat("Updates are large (approx KL {}, clip fraction {}): consider a lower "
            "learning rate", Format::OrDash(kl, Format::Metric), Format::OrDash(clip,
                [](double v) { return Format::Percent(v); })));

    if (std::optional<double> const episodes = progress->Number("episodes"))
        table.AddRow({ "episodes/update", Format::Count(uint64(*episodes)), "" });

    for (char const* column : EPISODE_COLUMNS)
        if (std::optional<double> const value = progress->Number(std::string("episode_") + column))
            table.AddRow({ std::string("  ") + column, Format::Metric(*value), "mean of the last update's episodes" });

    std::string const nonfinite = progress->Text("nonfinite");
    if (!nonfinite.empty())
        warnings.push_back("Non-finite values from the learner: " + nonfinite);

    table.Write(info, "  ");
}

void AnimusForge::ProgressMonitor::ReportLocal(SimSnapshot const& sim, LineSink const& info) const
{
    std::string header = Acore::StringFormat("Forge: {}", sim.Scenario);
    if (sim.PlanSize > 1)
        header += Acore::StringFormat(" ({} of {})", sim.PlanPosition, sim.PlanSize);
    info(header + " | " + sim.State + " | " + Format::Duration(sim.ScenarioSeconds));

    TextTable table({ { "Metric" }, { "Value", TextTable::Align::Right }, { "Note" } });
    table.AddRow({ "sim", Acore::StringFormat("{} ticks/s", Format::Count(uint64(sim.TicksPerSecond))),
        Acore::StringFormat("{} envs x {} agents, {} decisions", sim.Envs, sim.AgentsPerEnv,
            Format::Count(sim.Decisions)) });

    std::string episodes = Format::Count(sim.Episodes);
    std::string note = Acore::StringFormat("{:.1f}/s", sim.EpisodesPerSecond);
    if (sim.EpisodeLimit)
    {
        episodes += " / " + Format::Count(sim.EpisodeLimit);
        if (sim.EpisodesPerSecond > 0 && sim.Episodes < sim.EpisodeLimit)
        {
            double const eta = double(sim.EpisodeLimit - sim.Episodes) / sim.EpisodesPerSecond;
            note += Acore::StringFormat(", done in {} {}", Format::Duration(eta), ClockAfter(eta));
        }
    }
    table.AddRow({ "episodes", episodes, note });

    for (auto const& [name, value] : sim.EpisodeMeans)
        table.AddRow({ "  " + name, Format::Metric(value),
            Acore::StringFormat("mean of the last {} episodes", Format::Count(sim.EpisodeMeansCount)) });

    table.Write(info, "  ");
}

void AnimusForge::ProgressMonitor::ReportPlan(ForgeConfig const& config, std::vector<PlanRow> const& plan,
    ProgressFile const* current, LineSink const& info) const
{
    TextTable table({ { "#", TextTable::Align::Right }, { "Scenario" }, { "Status" },
        { "Env steps", TextTable::Align::Right }, { "Best score", TextTable::Align::Right },
        { "ETA", TextTable::Align::Right } });

    std::optional<double> const rate = current ? StepRate(*current) : std::nullopt;
    double remaining = 0.0;
    bool estimated = false;

    for (std::size_t i = 0; i < plan.size(); ++i)
    {
        PlanRow const& row = plan[i];
        std::string steps = "-";
        std::string best = "-";
        std::string eta = "";

        ProgressFile file;
        ProgressFile const* progress = row.Current ? current : nullptr;
        if (!row.Current && row.Status != "pending" && file.Load(ProgressPath(config, row.Scenario)))
            progress = &file;

        std::optional<uint64> const configured = ConfiguredTotalEnvSteps(config, row.Scenario);

        if (progress)
        {
            std::optional<double> const done = progress->Number("env_steps");
            std::optional<double> const total = progress->Number("total_env_steps");
            steps = Format::OrDash(done, Format::Compact) + " / " + Format::OrDash(total, Format::Compact);
            best = Format::OrDash(progress->Number("best_score"), Format::Metric);

            if (row.Current && done && total && rate && *rate > 0)
            {
                double const seconds = std::max(0.0, *total - *done) / *rate;
                remaining += seconds;
                eta = Format::Duration(seconds);
            }
        }
        else if (row.Status == "pending" && configured)
        {
            steps = Format::Compact(double(*configured));
            if (rate && *rate > 0)
            {
                double const seconds = double(*configured) / *rate;
                remaining += seconds;
                estimated = true;
                eta = "~" + Format::Duration(seconds);
            }
        }

        table.AddRow({ std::to_string(i + 1), row.Scenario, row.Status + (row.Resume ? " (resume)" : ""), steps, best,
            eta });
    }

    info("  Plan:");
    table.Write(info, "  ");

    if (remaining > 0)
        info(Acore::StringFormat("  Plan ETA {}{} {}{}", estimated ? "~" : "", Format::Duration(remaining),
            ClockAfter(remaining), estimated ? " (~: full step limit at the current rate; plateaus end sooner)" : ""));
}
