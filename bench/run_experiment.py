"""Scheduler experiment: run pipeline_test N times for each scheduler mode and CPU count.

For every (CPUS, SCHED_MODE) pair it does `make clean`, builds xv6 with that mode,
boots one disposable QEMU guest with -smp CPUS, runs
`pipeline_test <stages> <work> <deadline>` N times, and parses the [METRICS] line.

    python3 run_experiment.py --deadline 8                 # full experiment, 20 runs each
    python3 run_experiment.py --deadline 1000 --runs 5     # quick look at execution times
    python3 run_experiment.py --from-csv results/runs.csv  # redo table and graphs only

Writes results/runs.csv (every run), results/summary.md (the table) and the graphs.
"""
import argparse
import csv
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "agent"))
import config  # noqa: E402
from qemu_bridge import QemuBridge  # noqa: E402

SCHEDULERS = {0: "RR", 1: "EDF", 2: "EDF + Donation"}
METRICS = re.compile(r"\[METRICS\] Start=(\d+) \| End=(\d+) \| Target=(\d+) \| Elapsed=(\d+) \| Missed=([01])")
FIELDS = ["cpus", "scheduler", "run", "start", "end", "target", "elapsed", "missed"]


def qemu_args(cpus):
    # QEMU's newer machine types (pc-i440fx-6.2 and later) give xv6 a BIOS MP table
    # that lists only one CPU, so xv6 silently runs on one CPU whatever -smp says.
    # pc-i440fx-6.1 is the newest machine type where xv6 starts every CPU.
    args = list(config.QEMU_ARGS) + ["-machine", "pc-i440fx-6.1"]
    args[args.index("-smp") + 1] = str(cpus)
    return args


def run_config(cpus, mode, opts):
    subprocess.run(["make", "clean"], cwd=config.XV6_DIR, check=True, capture_output=True)
    subprocess.run(["make", f"SCHED_MODE={mode}", "xv6.img", "fs.img"], cwd=config.XV6_DIR,
                   check=True, capture_output=True)
    bridge = QemuBridge(args=qemu_args(cpus), command_timeout=opts.timeout)
    bridge.connect()
    started = len(re.findall(r"^cpu\d+: starting", bridge.boot_log, re.M))
    if started != cpus:
        bridge.close()
        raise RuntimeError(f"asked for {cpus} CPUs but xv6 started {started}:\n{bridge.boot_log}")
    rows = []
    try:
        for run in range(1, opts.runs + 1):
            out = bridge.execute(["pipeline_test", str(opts.stages), str(opts.work), str(opts.deadline)])["output"]
            match = METRICS.search(out)
            if not match:  # report it, never drop a run silently
                raise RuntimeError(f"CPUS={cpus} mode={mode} run {run}: no [METRICS] line in:\n{out}")
            start, end, target, elapsed, missed = map(int, match.groups())
            rows.append(dict(cpus=cpus, scheduler=SCHEDULERS[mode], run=run, start=start, end=end,
                             target=target, elapsed=elapsed, missed=missed))
            print(f"  CPUS={cpus} {SCHEDULERS[mode]:<15} run {run:2}: elapsed={elapsed} missed={missed}")
    finally:
        bridge.close()
    return rows


def summarize(rows):
    table = []
    for cpus in sorted({r["cpus"] for r in rows}):
        for name in SCHEDULERS.values():
            runs = [r for r in rows if r["cpus"] == cpus and r["scheduler"] == name]
            if not runs:
                continue
            times = [r["elapsed"] for r in runs]
            misses = sum(r["missed"] for r in runs)
            table.append(dict(scheduler=name, cpus=cpus, runs=len(runs), misses=misses,
                              miss_rate=100 * misses / len(runs), avg=statistics.mean(times),
                              std=statistics.pstdev(times), min=min(times), max=max(times)))
    return table


def write_summary(table, path, deadline):
    lines = [f"Deadline: {deadline} ticks after the pipeline starts. Times are in xv6 timer ticks.", "",
             "| Scheduler | CPUs | Runs | Misses | Miss rate | Average execution time | Std dev | Min | Max |",
             "|---|---|---|---|---|---|---|---|---|"]
    lines += [f"| {t['scheduler']} | {t['cpus']} | {t['runs']} | {t['misses']} | {t['miss_rate']:.0f}% "
              f"| {t['avg']:.2f} | {t['std']:.2f} | {t['min']} | {t['max']} |" for t in table]
    path.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


def plot(table, outdir, deadline):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    colors = {"RR": "#2a78d6", "EDF": "#eb6834", "EDF + Donation": "#1baf7a"}  # validated, fixed order
    ink, muted, grid = "#0b0b0b", "#898781", "#e1e0d9"
    cpu_counts = sorted({t["cpus"] for t in table})
    width = 0.26

    for metric, ylabel, fname, fmt, title in [
        ("miss_rate", "Deadline miss rate (%)", "miss_rate.png", "{:.0f}%", "Deadline misses"),
        ("avg", "Average execution time (ticks)", "exec_time.png", "{:.1f}", "Pipeline execution time, mean ± 1 std dev"),
    ]:
        fig, ax = plt.subplots(figsize=(7, 4.2), dpi=150)
        fig.patch.set_facecolor("#fcfcfb")
        ax.set_facecolor("#fcfcfb")
        for i, name in enumerate(SCHEDULERS.values()):
            xs, ys, errs = [], [], []
            for j, cpus in enumerate(cpu_counts):
                row = next((t for t in table if t["cpus"] == cpus and t["scheduler"] == name), None)
                if row:
                    xs.append(j + (i - 1) * (width + 0.02))
                    ys.append(row[metric])
                    errs.append(row["std"] if metric == "avg" else 0)
            bars = ax.bar(xs, ys, width, color=colors[name], label=name, edgecolor="#fcfcfb", linewidth=2,
                          yerr=errs if metric == "avg" else None, error_kw=dict(ecolor=muted, lw=1, capsize=3))
            for bar, y, err in zip(bars, ys, errs):
                ax.annotate(fmt.format(y), (bar.get_x() + bar.get_width() / 2, y + err), xytext=(0, 3),
                            textcoords="offset points", ha="center", va="bottom", fontsize=8, color=ink)
        ax.set_xticks(range(len(cpu_counts)), [f"{c} CPU" + ("s" if c > 1 else "") for c in cpu_counts])
        ax.set_ylabel(ylabel, color=ink)
        ax.set_ylim(0, 115 if metric == "miss_rate" else 1.3 * max(t["avg"] + t["std"] for t in table))
        ax.set_title(f"{title} (deadline {deadline} ticks, {table[0]['runs']} runs each)", color=ink,
                     fontsize=11, loc="left")
        ax.grid(axis="y", color=grid, linewidth=0.6)
        ax.set_axisbelow(True)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color("#c3c2b7")
        ax.tick_params(colors=muted)
        ax.legend(frameon=False, fontsize=9, labelcolor=ink, ncols=3, loc="upper right")
        fig.tight_layout()
        fig.savefig(outdir / fname)
        plt.close(fig)
        print(f"wrote {outdir / fname}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--runs", type=int, default=20)
    p.add_argument("--stages", type=int, default=4)
    p.add_argument("--work", type=int, default=5000000, help="CPU work per stage")
    p.add_argument("--deadline", type=int, required=False, help="relative deadline in ticks")
    p.add_argument("--cpus", type=int, nargs="+", default=[1, 3])
    p.add_argument("--modes", type=int, nargs="+", default=[0, 1, 2], choices=[0, 1, 2])
    p.add_argument("--timeout", type=float, default=300, help="seconds allowed per pipeline_test run")
    p.add_argument("--out", type=Path, default=Path(__file__).resolve().parent / "results")
    p.add_argument("--from-csv", type=Path, help="skip running; rebuild table and graphs from this CSV")
    opts = p.parse_args()
    opts.out.mkdir(parents=True, exist_ok=True)

    if opts.from_csv:
        with open(opts.from_csv) as f:
            rows = [{k: (v if k == "scheduler" else int(v)) for k, v in r.items()} for r in csv.DictReader(f)]
        deadline = rows[0]["target"] - rows[0]["start"]
    else:
        if opts.deadline is None:
            p.error("--deadline is required unless --from-csv is given")
        rows = []
        for cpus in opts.cpus:
            for mode in opts.modes:
                print(f"== CPUS={cpus} SCHED_MODE={mode} ({SCHEDULERS[mode]})")
                rows += run_config(cpus, mode, opts)
        with open(opts.out / "runs.csv", "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=FIELDS)
            writer.writeheader()
            writer.writerows(rows)
        deadline = opts.deadline

    table = summarize(rows)
    write_summary(table, opts.out / "summary.md", deadline)
    plot(table, opts.out, deadline)


if __name__ == "__main__":
    main()
