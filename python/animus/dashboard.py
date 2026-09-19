"""A local dashboard for a forge run: the config it is running under, and what it is doing right now.

The worldserver container starts it (apps/docker/forge-worldserver.sh) and publishes it on
http://localhost:18800, next to TensorBoard. To run one by hand, in the container or on the host:

    python3 modules/mod-animus-forge/python/animus/dashboard.py [--port 8800] [--host 0.0.0.0] [--runs DIR]

Reads the files the sim and the learner already write (runs/<stage>/progress.json, eval.csv, metrics.csv,
eval_episodes.jsonl, finished.json) and the worldserver's own conf. It never writes anything and never talks to the
sim, so it is safe to start, stop and restart under a live run. Standard library only -- it runs on the host's python
without the learner's venv, and binds to the loopback address only.

TensorBoard (http://localhost:16006) plots the same scalars in more depth; this page answers the questions it cannot:
what config is this run using, which stage of the plan is live, and how is each class/role doing in the last
evaluation.
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import os
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]          # <repo>/modules/mod-animus-forge/python/animus/dashboard.py
DEFAULT_RUNS = ROOT / "var" / "animus-forge" / "runs"
DEFAULT_CONF = ROOT / "env" / "dist" / "etc" / "modules" / "mod_animus_forge.conf"
DIST_CONF = ROOT / "modules" / "mod-animus-forge" / "conf" / "mod_animus_forge.conf.dist"

# The training-metric series the page plots, and what each is called on it.
SERIES = [
    ("episode_killed", "killed"),
    ("episode_died", "died"),
    ("episode_timed_out", "timed out"),
    ("episode_difficulty", "difficulty"),
    ("entropy", "entropy"),
    ("reward_per_decision", "reward/decision"),
]
# Per class/role, from the last evaluation's episodes.
LAYOUT_FIELDS = ["killed", "died", "timed_out", "options_started", "option_seconds", "preparation_seconds",
                 "in_melee_share", "repeated_presses", "health_left", "interrupts", "control_seconds"]


def _read_json(path: Path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


class Cache:
    """Parsed files, keyed by path and invalidated by (mtime, size): a poll every few seconds must not re-read a
    100 MB episode log that has not changed."""

    def __init__(self):
        self._entries: dict[Path, tuple[tuple[float, int], object]] = {}

    def get(self, path: Path, parse):
        try:
            stat = path.stat()
        except OSError:
            return None
        stamp = (stat.st_mtime, stat.st_size)
        hit = self._entries.get(path)
        if hit and hit[0] == stamp:
            return hit[1]
        try:
            value = parse(path)
        except (OSError, ValueError):
            return None
        self._entries[path] = (stamp, value)
        return value


CACHE = Cache()


def parse_conf(path: Path) -> dict[str, str]:
    """`Key = value` lines of a worldserver conf, comments and blanks dropped."""
    values: dict[str, str] = {}
    for line in path.read_text(errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip().strip('"')
    return values


def parse_eval(path: Path) -> list[dict]:
    rows = []
    for row in csv.DictReader(io.StringIO(path.read_text(errors="replace"))):
        try:
            rows.append({
                "update": int(float(row["update"])),
                "env_steps": int(float(row["env_steps"])),
                "policy": row["policy"],
                "score": float(row["score"]),
                "stderr": float(row.get("stderr") or 0.0),
                "best": float(row["best"]) if row.get("best") else None,
            })
        except (KeyError, TypeError, ValueError):
            continue
    return rows


def parse_metrics(path: Path, points: int = 240) -> dict[str, list]:
    """The plotted series, thinned to `points` samples: an hour of training is thousands of updates and the page only
    has a few hundred pixels to draw them in."""
    rows = list(csv.DictReader(io.StringIO(path.read_text(errors="replace"))))
    if not rows:
        return {}
    step = max(1, len(rows) // points)
    kept = rows[::step] + ([rows[-1]] if len(rows) > 1 else [])
    series: dict[str, list] = {"env_steps": []}
    for name, _ in SERIES:
        series[name] = []
    for row in kept:
        try:
            series["env_steps"].append(float(row["env_steps"]))
        except (KeyError, TypeError, ValueError):
            continue
        for name, _ in SERIES:
            try:
                series[name].append(float(row[name]))
            except (KeyError, TypeError, ValueError):
                series[name].append(None)
    return series


def parse_live_layouts(path: Path) -> dict:
    """The newest update's per class/role training rows (layouts.csv). Training episodes, not an evaluation: sampled
    actions at each class/role's own ladder difficulty, which is what makes them live -- they arrive every update
    rather than every eval.every_env_steps."""
    rows = list(csv.DictReader(io.StringIO(path.read_text(errors="replace"))))
    if not rows:
        return {}

    newest = rows[-1].get("update")
    latest = [row for row in rows if row.get("update") == newest]
    layouts = []
    for row in latest:
        entry = {"layout": row.get("layout", "?")}
        for key, value in row.items():
            if key.startswith("episode_") or key in ("episodes", "env_steps"):
                try:
                    entry[key.replace("episode_", "")] = float(value)
                except (TypeError, ValueError):
                    continue
        layouts.append(entry)

    layouts.sort(key=lambda entry: entry.get("killed", 0.0))
    return {"update": newest, "env_steps": layouts[0].get("env_steps", 0) if layouts else 0, "layouts": layouts}


def parse_layouts(path: Path) -> dict:
    """Per class/role means over the last evaluation in the episode log (the learner appends every evaluation)."""
    last_steps = None
    totals: dict[str, dict[str, float]] = {}
    with path.open(errors="replace") as handle:
        for line in handle:
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if row.get("policy") == "fight":
                continue
            steps = row.get("env_steps", 0)
            if last_steps is None or steps > last_steps:
                last_steps, totals = steps, {}
            if steps != last_steps:
                continue
            entry = totals.setdefault(row.get("layout", "?"), {"n": 0.0})
            entry["n"] += 1
            for field in LAYOUT_FIELDS:
                entry[field] = entry.get(field, 0.0) + (row.get(field) or 0.0)
    layouts = []
    for name in sorted(totals):
        entry = totals[name]
        count = entry["n"]
        means = {field: entry.get(field, 0.0) / count for field in LAYOUT_FIELDS}
        started = means["options_started"]
        means["seconds_per_option"] = means["option_seconds"] / started if started else 0.0
        means["episodes"] = count
        means["layout"] = name
        layouts.append(means)
    return {"env_steps": last_steps or 0, "layouts": layouts}


def run_state(run_dir: Path) -> dict | None:
    progress = CACHE.get(run_dir / "progress.json", _read_json)
    finished = CACHE.get(run_dir / "finished.json", _read_json)
    if not progress and not finished:
        return None
    progress = progress or {}
    evals = CACHE.get(run_dir / "eval.csv", parse_eval) or []
    return {
        "name": run_dir.name,
        "progress": progress,
        "finished": finished,
        "evals": evals,
        "updated_at": progress.get("updated_at", 0),
        "live": bool(progress) and not finished and time.time() - progress.get("updated_at", 0) < 120,
    }


def collect(runs_dir: Path, conf_path: Path) -> dict:
    runs = []
    if runs_dir.is_dir():
        for child in sorted(runs_dir.iterdir()):
            if not child.is_dir() or child.name.startswith("_"):
                continue
            state = run_state(child)
            if state:
                runs.append(state)

    runs.sort(key=lambda r: r["updated_at"], reverse=True)
    current = next((r for r in runs if r["live"]), runs[0] if runs else None)

    metrics: dict = {}
    layouts: dict = {}
    if current:
        run_dir = runs_dir / current["name"]
        metrics = CACHE.get(run_dir / "metrics.csv", parse_metrics) or {}
        layouts = CACHE.get(run_dir / "eval_episodes.jsonl", parse_layouts) or {}
        live = CACHE.get(run_dir / "layouts.csv", parse_live_layouts) or {}

    live = CACHE.get(conf_path, parse_conf) or {}
    dist = CACHE.get(DIST_CONF, parse_conf) or {}
    config = [{"key": key, "value": value, "default": dist.get(key), "changed": key in dist and dist[key] != value}
              for key, value in sorted(live.items())]
    # Keys the conf leaves out take the module's built-in default, which the dist file documents.
    config += [{"key": key, "value": value, "default": value, "changed": False, "from_dist": True}
               for key, value in sorted(dist.items()) if key not in live]

    return {
        "now": time.time(),
        "runs_dir": str(runs_dir),
        "conf": str(conf_path),
        "runs": [{k: v for k, v in run.items() if k != "progress"} | {"progress": run["progress"]} for run in runs],
        "current": current["name"] if current else None,
        "series": [{"key": key, "label": label} for key, label in SERIES],
        "metrics": metrics,
        "layouts": layouts,
        "live": live,
        "config": config,
    }


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>Animus Forge</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  :root {
    --bg: #0f1115; --panel: #171a21; --line: #262b36; --text: #e6e9ef; --dim: #939cad;
    --good: #5bd6a0; --warn: #e5c07b; --bad: #e88388; --accent: #7aa2f7;
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--text);
         font: 14px/1.5 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif; }
  header { display: flex; align-items: baseline; gap: 16px; flex-wrap: wrap;
           padding: 14px 20px; border-bottom: 1px solid var(--line); }
  h1 { font-size: 16px; margin: 0; font-weight: 650; letter-spacing: .01em; }
  .muted { color: var(--dim); }
  main { padding: 20px; display: grid; gap: 16px; max-width: 1400px; }
  .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; }
  .card { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 12px 14px; }
  .card .k { color: var(--dim); font-size: 11px; text-transform: uppercase; letter-spacing: .06em; }
  .card .v { font-size: 20px; font-variant-numeric: tabular-nums; margin-top: 2px; }
  .panel { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 14px 16px; }
  .panel h2 { font-size: 13px; margin: 0 0 10px; color: var(--dim); text-transform: uppercase;
              letter-spacing: .06em; font-weight: 600; }
  table { border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; }
  th, td { text-align: right; padding: 4px 8px; border-bottom: 1px solid var(--line); white-space: nowrap; }
  th:first-child, td:first-child { text-align: left; }
  th { color: var(--dim); font-weight: 600; font-size: 12px; position: sticky; top: 0; background: var(--panel); }
  tbody tr:hover { background: #1d222c; }
  .charts { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 14px; }
  .chart { height: 150px; width: 100%; }
  .bar { height: 6px; background: var(--line); border-radius: 3px; overflow: hidden; margin-top: 6px; }
  .bar > div { height: 100%; background: var(--accent); }
  input[type=search] { background: #11141a; color: var(--text); border: 1px solid var(--line);
                       border-radius: 8px; padding: 6px 10px; width: 260px; }
  label { color: var(--dim); font-size: 12px; margin-left: 12px; }
  .scroll { max-height: 420px; overflow: auto; }
  .good { color: var(--good); } .warn { color: var(--warn); } .bad { color: var(--bad); }
  .dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; margin-right: 6px; }
  .changed td:first-child::after { content: " changed"; color: var(--warn); font-size: 11px; }
</style></head>
<body>
<header>
  <h1>Animus Forge</h1>
  <span id="stage" class="muted"></span>
  <span style="flex:1"></span>
  <span id="clock" class="muted"></span>
</header>
<main>
  <div class="cards" id="cards"></div>
  <div class="panel"><h2>Training</h2><div class="charts" id="charts"></div></div>
  <div class="panel"><h2>Evaluations</h2><div id="evals"></div></div>
  <div class="panel"><h2>Class and role, right now <span id="livesteps" class="muted"></span></h2>
    <div class="scroll"><table id="live"></table></div></div>
  <div class="panel"><h2>Class and role, last evaluation <span id="layoutsteps" class="muted"></span></h2>
    <div class="scroll"><table id="layouts"></table></div></div>
  <div class="panel"><h2>Runs</h2><table id="runs"></table></div>
  <div class="panel"><h2>Config
      <input type="search" id="filter" placeholder="filter keys and values">
      <label><input type="checkbox" id="changedonly"> changed from the dist default only</label></h2>
    <div class="scroll"><table id="config"></table></div></div>
</main>
<script>
const fmt = (v, d = 2) => v === null || v === undefined || Number.isNaN(v) ? "-" : Number(v).toFixed(d);
const steps = v => v >= 1e6 ? (v / 1e6).toFixed(1) + "M" : v >= 1e3 ? (v / 1e3).toFixed(0) + "k" : String(v ?? 0);
const dur = s => { if (!s || s < 0) return "-"; const h = Math.floor(s / 3600), m = Math.round(s % 3600 / 60);
                   return h ? `${h}h ${m}m` : `${m}m`; };

function card(k, v, cls) { return `<div class="card"><div class="k">${k}</div><div class="v ${cls || ""}">${v}</div></div>`; }

function chart(series, xs, ys, label) {
  const w = 320, h = 150, pad = 26;
  const pts = xs.map((x, i) => [x, ys[i]]).filter(p => p[1] !== null && p[1] !== undefined);
  if (pts.length < 2) return `<svg class="chart" viewBox="0 0 ${w} ${h}"><text x="8" y="20" fill="#939cad">${label}</text></svg>`;
  const xmin = Math.min(...pts.map(p => p[0])), xmax = Math.max(...pts.map(p => p[0]));
  const ymin = Math.min(...pts.map(p => p[1])), ymax = Math.max(...pts.map(p => p[1]));
  const sx = x => pad + (w - pad - 6) * (xmax === xmin ? 1 : (x - xmin) / (xmax - xmin));
  const sy = y => h - 18 - (h - 34) * (ymax === ymin ? 0.5 : (y - ymin) / (ymax - ymin));
  const d = pts.map((p, i) => `${i ? "L" : "M"}${sx(p[0]).toFixed(1)},${sy(p[1]).toFixed(1)}`).join("");
  const last = pts[pts.length - 1][1];
  return `<svg class="chart" viewBox="0 0 ${w} ${h}">
    <line x1="${pad}" y1="${h - 18}" x2="${w - 6}" y2="${h - 18}" stroke="#262b36"/>
    <path d="${d}" fill="none" stroke="#7aa2f7" stroke-width="1.6"/>
    <text x="4" y="14" fill="#939cad" font-size="11">${label}</text>
    <text x="${w - 6}" y="14" fill="#e6e9ef" font-size="11" text-anchor="end">${fmt(last, 3)}</text>
    <text x="4" y="${h - 4}" fill="#939cad" font-size="10">${fmt(ymin, 2)}</text>
    <text x="${w - 6}" y="${h - 4}" fill="#939cad" font-size="10" text-anchor="end">${steps(xmax)} steps</text>
  </svg>`;
}

let state = null;

function render() {
  if (!state) return;
  const run = (state.runs || []).find(r => r.name === state.current);
  const p = run ? run.progress || {} : {};
  document.getElementById("stage").textContent = run
    ? `${run.name} - ${run.finished ? "finished: " + (run.finished.reason || "") : p.phase || "?"}` : "no runs yet";
  document.getElementById("clock").textContent = new Date(state.now * 1000).toLocaleTimeString();

  const total = p.total_env_steps || 0, done = p.env_steps || 0;
  const eta = p.env_steps_per_sec && total > done ? (total - done) / p.env_steps_per_sec : null;
  const score = p.last_eval_score, base = p.baseline_score;
  const cls = score === undefined || base === undefined ? "" : score > base ? "good" : "bad";
  document.getElementById("cards").innerHTML = [
    card("steps", `${steps(done)}<div class="bar"><div style="width:${total ? Math.min(100, 100 * done / total) : 0}%"></div></div>`),
    card("steps / s", fmt(p.env_steps_per_sec, 0)),
    card("update", p.update ?? "-"),
    card("elapsed", dur(p.elapsed_seconds)),
    card("eta", dur(eta)),
    card("last eval", fmt(score, 2), cls),
    card("baseline", fmt(base, 2)),
    card("best", `${fmt(p.best_score, 2)} <span class="muted" style="font-size:12px">@${steps(p.best_env_steps || 0)}</span>`),
    card("entropy", fmt(p.entropy, 3)),
    card("approx kl", fmt(p.approx_kl, 4)),
    card("clip frac", fmt(p.clip_frac, 3)),
    card("reward / decision", fmt(p.reward_per_decision, 4)),
  ].join("");

  const m = state.metrics || {};
  document.getElementById("charts").innerHTML = (state.series || [])
    .map(s => chart(s.key, m.env_steps || [], m[s.key] || [], s.label)).join("");

  const evals = (run && run.evals || []).slice().reverse();
  document.getElementById("evals").innerHTML = evals.length ? `<table>
    <thead><tr><th>steps</th><th>policy</th><th>score</th><th>+/-</th><th>best</th></tr></thead><tbody>
    ${evals.map(e => `<tr><td>${steps(e.env_steps)}</td><td>${e.policy}</td><td>${fmt(e.score, 3)}</td>
      <td class="muted">${fmt(e.stderr, 3)}</td><td>${e.best === null ? "-" : fmt(e.best, 3)}</td></tr>`).join("")}
    </tbody></table>` : `<span class="muted">none yet</span>`;

  // What each class/role is doing in the training episodes of the newest update, rather than at the last evaluation.
  const LV = state.live || {};
  document.getElementById("livesteps").textContent = LV.update !== undefined
    ? `(update ${LV.update}, ${steps(LV.env_steps || 0)} steps, sampled actions)` : "(waiting for the first update)";
  const livecols = [["episodes", "eps", 0], ["killed", "kill", 2], ["died", "die", 2], ["timed_out", "t/o", 2],
                    ["difficulty", "diff", 1], ["in_melee_share", "melee", 2], ["power_left", "power", 2],
                    ["healing_per_mana", "heal/mana", 2], ["hot_healing_share", "hot", 2],
                    ["repeated_presses", "repeats", 1], ["hazard_seconds", "haz s", 1],
                    ["interruptible_casts_seen", "seen", 1], ["reward_interrupt", "r_int", 3]];
  document.getElementById("live").innerHTML = `
    <thead><tr><th>layout</th>${livecols.map(c => `<th>${c[1]}</th>`).join("")}</tr></thead><tbody>
    ${(LV.layouts || []).map(r => `<tr><td>${r.layout}</td>${livecols.map(c => {
        const v = r[c[0]];
        let k = "";
        if (c[0] === "killed") k = v >= 0.9 ? "good" : v < 0.7 ? "bad" : "warn";
        if (c[0] === "died" || c[0] === "timed_out") k = v <= 0.05 ? "good" : v > 0.2 ? "bad" : "warn";
        return `<td class="${k}">${v === undefined ? "-" : fmt(v, c[2])}</td>`;
      }).join("")}</tr>`).join("")}</tbody>`;

  const L = state.layouts || {};
  document.getElementById("layoutsteps").textContent = L.env_steps ? `(${steps(L.env_steps)} steps)` : "";
  const cols = [["killed", "kill", 2], ["died", "die", 2], ["timed_out", "t/o", 2],
                ["options_started", "opts", 1], ["option_seconds", "opt s", 1], ["seconds_per_option", "s/opt", 2],
                ["preparation_seconds", "prep", 1], ["in_melee_share", "melee", 2],
                ["repeated_presses", "repeats", 1], ["interrupts", "intr", 2],
                ["control_seconds", "control", 1], ["health_left", "hp", 2]];
  document.getElementById("layouts").innerHTML = `
    <thead><tr><th>layout</th>${cols.map(c => `<th>${c[1]}</th>`).join("")}</tr></thead><tbody>
    ${(L.layouts || []).map(r => `<tr><td>${r.layout}</td>${cols.map(c => {
        const v = r[c[0]];
        let k = "";
        if (c[0] === "killed") k = v >= 0.9 ? "good" : v < 0.7 ? "bad" : "warn";
        if (c[0] === "died" || c[0] === "timed_out") k = v <= 0.05 ? "good" : v > 0.2 ? "bad" : "warn";
        return `<td class="${k}">${fmt(v, c[2])}</td>`;
      }).join("")}</tr>`).join("")}</tbody>`;

  document.getElementById("runs").innerHTML = `
    <thead><tr><th>run</th><th>state</th><th>steps</th><th>best</th><th>baseline</th><th>updated</th></tr></thead><tbody>
    ${(state.runs || []).map(r => { const q = r.progress || {}; return `<tr>
      <td><span class="dot" style="background:${r.live ? "#5bd6a0" : "#3a4150"}"></span>${r.name}</td>
      <td>${r.finished ? (r.finished.reason || "finished") + (r.finished.advanced ? ", advanced" : "") : q.phase || "-"}</td>
      <td>${steps(q.env_steps || 0)}</td><td>${fmt(q.best_score, 2)}</td><td>${fmt(q.baseline_score, 2)}</td>
      <td class="muted">${q.updated_at ? new Date(q.updated_at * 1000).toLocaleTimeString() : "-"}</td></tr>`; }).join("")}
    </tbody>`;

  renderConfig();
}

function renderConfig() {
  const needle = document.getElementById("filter").value.toLowerCase();
  const only = document.getElementById("changedonly").checked;
  const rows = (state.config || []).filter(c =>
    (!only || c.changed) &&
    (!needle || c.key.toLowerCase().includes(needle) || String(c.value).toLowerCase().includes(needle)));
  document.getElementById("config").innerHTML = `
    <thead><tr><th>key</th><th>value</th><th>dist default</th></tr></thead><tbody>
    ${rows.map(c => `<tr class="${c.changed ? "changed" : ""}"><td>${c.key}</td><td>${c.value}</td>
      <td class="muted">${c.default === undefined || c.default === null ? "-" : c.default}</td></tr>`).join("")}
    </tbody>`;
}

async function poll() {
  try {
    const r = await fetch("api/state");
    state = await r.json();
    render();
  } catch (e) { document.getElementById("clock").textContent = "disconnected"; }
}
document.getElementById("filter").addEventListener("input", renderConfig);
document.getElementById("changedonly").addEventListener("change", renderConfig);
poll();
setInterval(poll, 5000);
</script>
</body></html>
"""


class Handler(BaseHTTPRequestHandler):
    runs_dir = DEFAULT_RUNS
    conf_path = DEFAULT_CONF

    def _send(self, body: bytes, content_type: str):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler's name
        path = self.path.split("?")[0].rstrip("/") or "/"
        if path in ("/", "/index.html"):
            self._send(PAGE.encode(), "text/html; charset=utf-8")
        elif path == "/api/state":
            payload = json.dumps(collect(self.runs_dir, self.conf_path)).encode()
            self._send(payload, "application/json")
        else:
            self.send_error(404)

    def log_message(self, *_args):
        pass        # a poll every 5 s would fill the terminal it runs in


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8800)
    parser.add_argument("--host", default="127.0.0.1", help="loopback by default: the page is not authenticated")
    parser.add_argument("--runs", type=Path, default=DEFAULT_RUNS, help="AnimusForge.OutputDir's runs directory")
    parser.add_argument("--conf", type=Path, default=DEFAULT_CONF, help="the worldserver's mod_animus_forge.conf")
    args = parser.parse_args()

    Handler.runs_dir = args.runs
    Handler.conf_path = args.conf
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Animus Forge dashboard on http://{args.host}:{args.port}  (runs {args.runs})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
