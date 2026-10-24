import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, Rectangle

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(BASE, "report", "img")
os.makedirs(IMG, exist_ok=True)

PARTS = [("Q", "Очередь Qk", 8), ("W0", "W0 (fetcher)", 26), ("W1", "W1", 42),
         ("W2", "W2", 58), ("W3", "W3", 74), ("QN", "Очередь Qk+1", 92)]
X = {k: x for k, _, x in PARTS}

EVENTS = [
    ("self", "W0", "ждёт current == nullptr\n(раунд завершён)"),
    ("msg", "W0", "Q", "pop()"),
    ("ret", "Q", "W0", "партия из 4 заявок"),
    ("self", "W0", "current = batch;\nround++; claim = 0;\ndepart = 0"),
    ("msg", "W0", "W1", "WakeAllConditionVariable(cvBatch)"),
    ("msg", "W0", "W2", "WakeAllConditionVariable(cvBatch)"),
    ("msg", "W0", "W3", "WakeAllConditionVariable(cvBatch)"),
    ("par", [("W0", "slot 0: обработка REQ a"), ("W1", "slot 1: обработка REQ b"),
             ("W2", "slot 2: обработка REQ c"), ("W3", "slot 3: обработка REQ d")]),
    ("par", [("W0", "EnterSynchronizationBarrier"), ("W1", "EnterSynchronizationBarrier"),
             ("W2", "EnterSynchronizationBarrier"), ("W3", "EnterSynchronizationBarrier")]),
    ("note", "W0", "W3", "Барьер освобождает все 4 потока только когда\n"
                         "каждый из них вошёл в него (конец раунда).\n"
                         "TRUE получает только последний прибывший поток."),
    ("self", "W3", "depart == 4:\nprocessed == items"),
    ("msg", "W3", "QN", "push(партия целиком)"),
    ("self", "W3", "current = nullptr;\nWakeAllConditionVariable"),
    ("msg", "W0", "Q", "pop() следующей партии"),
]

height = 5.0
for ev in EVENTS:
    if ev[0] == "note":
        height += 4.6
    elif ev[0] == "par":
        height += 2.8
    else:
        height += 2.2

fig, ax = plt.subplots(figsize=(12, height * 0.27), dpi=150)
ax.set_xlim(0, 100)
ax.set_ylim(-height, 4)
ax.axis("off")

ax.text(50, 3.2, "Обработка одной партии на ступени (режим batch, вариант 19)",
        ha="center", va="center", fontsize=12, fontweight="bold")

for key, name, x in PARTS:
    ax.add_patch(Rectangle((x - 8, 0.4), 16, 2.2, fc="white", ec="black", lw=1.2))
    ax.text(x, 1.5, name, ha="center", va="center", fontsize=9, fontweight="bold")
    ax.plot([x, x], [0.4, -height + 0.6], color="#666666", lw=0.8, ls=(0, (4, 3)))


def self_loop(x, y):
    ax.plot([x, x + 4.5], [y, y], color="black", lw=1.0)
    ax.plot([x + 4.5, x + 4.5], [y, y - 1.1], color="black", lw=1.0)
    ax.add_patch(FancyArrowPatch((x + 4.5, y - 1.1), (x, y - 1.1), arrowstyle="-|>",
                                 mutation_scale=10, color="black", lw=1.0))


y = -1.4
for ev in EVENTS:
    kind = ev[0]
    if kind == "self":
        _, p, label = ev
        self_loop(X[p], y)
        ax.text(X[p] + 5.6, y - 0.55, label, ha="left", va="center", fontsize=7.5)
        y -= 2.2
    elif kind in ("msg", "ret"):
        _, a, b, label = ev
        style = "-" if kind == "msg" else (0, (4, 3))
        ax.add_patch(FancyArrowPatch((X[a], y), (X[b], y), arrowstyle="-|>",
                                     mutation_scale=11, color="black", lw=1.1,
                                     linestyle=style))
        ax.text((X[a] + X[b]) / 2, y + 0.35, label, ha="center", va="bottom", fontsize=7.5)
        y -= 2.2
    elif kind == "par":
        for p, label in ev[1]:
            self_loop(X[p], y)
            ax.text(X[p] + 2.25, y + 0.35, label, ha="center", va="bottom", fontsize=7.5)
        y -= 2.8
    elif kind == "note":
        _, a, b, label = ev
        x0, x1 = X[a] - 6, X[b] + 6
        ax.add_patch(Rectangle((x0, y - 3.8), x1 - x0, 3.4, fc="#f0f0f0", ec="black", lw=1.0))
        ax.text((x0 + x1) / 2, y - 2.1, label, ha="center", va="center", fontsize=8)
        y -= 4.6

fig.tight_layout()
out = os.path.join(IMG, "sequence.png")
fig.savefig(out, facecolor="white")
print("OK ->", out)
