import csv
import os
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXP = os.path.join(BASE, "data", "experiments.csv")
IMG = os.path.join(BASE, "report", "img")
os.makedirs(IMG, exist_ok=True)

with open(EXP, encoding="utf-8") as f:
    rows = list(csv.DictReader(f))

groups = defaultdict(list)
for r in rows:
    groups[(r["mode"], int(r["requests"]))].append(r)

reqs = sorted({int(r["requests"]) for r in rows})


def avg(mode, n, field):
    vals = [float(x[field]) for x in groups[(mode, n)]]
    return sum(vals) / len(vals)


COLORS = {"single": "#1f77b4", "batch": "#d62728"}

fig, ax1 = plt.subplots(figsize=(8, 4.5), dpi=150)
for mode, mk in [("single", "o"), ("batch", "s")]:
    wall = [avg(mode, n, "wall_ms") for n in reqs]
    ax1.plot(reqs, wall, "-" + mk, color=COLORS[mode], label=f"{mode}: время выполнения, мс")
ax1.set_xlabel("Количество заявок")
ax1.set_ylabel("Время выполнения, мс")
ax2 = ax1.twinx()
for mode, mk in [("single", "^"), ("batch", "v")]:
    thr = [avg(mode, n, "completed") / avg(mode, n, "wall_ms") * 1000.0 for n in reqs]
    ax2.plot(reqs, thr, "--" + mk, color=COLORS[mode], alpha=0.55,
             label=f"{mode}: пропускная способность, заяв/с")
ax2.set_ylabel("Пропускная способность, заяв/с")
h1, l1 = ax1.get_legend_handles_labels()
h2, l2 = ax2.get_legend_handles_labels()
ax1.legend(h1 + h2, l1 + l2, loc="upper left", fontsize=8)
ax1.set_title("Рисунок 1 — Время выполнения и пропускная способность\n"
              "от количества заявок (stages=3, workers=4, batch=4, seed=8081)", fontsize=10)
ax1.grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig(os.path.join(IMG, "graph1_time_throughput.png"))
plt.close(fig)

fig, ax1 = plt.subplots(figsize=(8, 4.5), dpi=150)
for mode, mk in [("single", "o"), ("batch", "s")]:
    wait = [avg(mode, n, "mean_wait_ms") for n in reqs]
    ax1.plot(reqs, wait, "-" + mk, color=COLORS[mode], label=f"{mode}: среднее время ожидания, мс")
ax1.set_xlabel("Количество заявок")
ax1.set_ylabel("Среднее время ожидания заявки, мс")
ax2 = ax1.twinx()
bar = [avg("batch", n, "barrier_wait_total_ms") for n in reqs]
ax2.plot(reqs, bar, "--D", color="#7f7f7f", alpha=0.8,
         label="batch: суммарное ожидание на барьерах, мс")
ax2.set_ylabel("Суммарное время на барьерах, мс")
h1, l1 = ax1.get_legend_handles_labels()
h2, l2 = ax2.get_legend_handles_labels()
ax1.legend(h1 + h2, l1 + l2, loc="upper left", fontsize=8)
ax1.set_title("Рисунок 2 — Время ожидания заявок и суммарное время\n"
              "ожидания на барьерах от количества заявок", fontsize=10)
ax1.grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig(os.path.join(IMG, "graph2_wait_barrier.png"))
plt.close(fig)

print("OK ->", IMG)
