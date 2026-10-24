import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, Rectangle

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(BASE, "report", "img")
os.makedirs(IMG, exist_ok=True)

fig, ax = plt.subplots(figsize=(12.5, 5.2), dpi=150)
ax.set_xlim(0, 130)
ax.set_ylim(0, 44)
ax.axis("off")


def box(x, y, w, h, text, fs=8, bold=False):
    ax.add_patch(Rectangle((x, y), w, h, fc="white", ec="black", lw=1.2))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center", fontsize=fs,
            fontweight="bold" if bold else "normal")


def arr(x1, y1, x2, y2, num=None, nx=None, ny=None):
    ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle="-|>",
                                 mutation_scale=11, color="black", lw=1.1))
    if num:
        ax.text(nx if nx is not None else (x1 + x2) / 2,
                ny if ny is not None else (y1 + y2) / 2 + 1.0,
                num, ha="center", va="center", fontsize=7.5, fontweight="bold")


box(1, 24, 12, 8, "Generator\n(1 поток)")
box(117, 24, 12, 8, "Collector\n(1 поток)")

for qi, qx in enumerate([15, 47, 79, 111]):
    box(qx, 25, 4, 6, f"Q{qi}\ncap\n16", fs=6)

CX = [4.0, 9.6, 15.2, 20.8]
for k in range(3):
    x0 = 21 + k * 32
    ax.add_patch(Rectangle((x0, 8), 24, 30, fc="white", ec="black", lw=1.4))
    ax.text(x0 + 12, 36, f"Stage {k + 1} (4 потока)", ha="center", fontsize=9,
            fontweight="bold")
    for i in range(4):
        box(x0 + 1.5 + i * 5.6, 24, 5, 6, f"W{i}", fs=7.5)
    for i in range(1, 4):
        ax.plot([x0 + CX[0], x0 + CX[0], x0 + CX[i]], [30, 31.4, 31.4],
                color="black", lw=1.0)
        ax.add_patch(FancyArrowPatch((x0 + CX[i], 31.4), (x0 + CX[i], 30),
                                     arrowstyle="-|>", mutation_scale=9,
                                     color="black", lw=1.0))
    ax.text(x0 + CX[0] - 1.6, 31.4, "3", fontsize=7.5, fontweight="bold",
            ha="center", va="center")
    ax.add_patch(Rectangle((x0 + 1.5, 12.5), 21.8, 1.6, fc="black", ec="black"))
    ax.text(x0 + 12, 10.0, f"Barrier {k + 1}: SYNCHRONIZATION_BARRIER\n"
                           "TRUE — последнему; он пересылает партию (5, 6)",
            ha="center", fontsize=6.5)
    for i in range(4):
        arr(x0 + CX[i], 24, x0 + CX[i], 14.1)
    ax.text(x0 + CX[0] - 1.6, 19, "4", fontsize=7.5, fontweight="bold",
            ha="center", va="center")
    arr(x0 + 23.3, 13.3, x0 + 26, 25, "5,6", x0 + 26.4, 18.5)

arr(13, 28, 15, 28, "1", 14, 30)
arr(19, 28, 22.5, 27, "2", 20.6, 30)
arr(115, 28, 117, 28, "7", 116, 30)

legend = [
    "1 — Generator формирует партию из 4 заявок и кладёт её в Q0 (push с таймаутом);",
    "2 — W0 каждой ступени (fetcher) забирает партию из входной очереди (pop) и публикует раунд;",
    "3 — W0 будит W1–W3 (WakeAllConditionVariable по cvBatch); каждый поток берёт свою заявку через InterlockedIncrement(claim);",
    "4 — обработав заявку, все 4 потока входят в EnterSynchronizationBarrier своей ступени;",
    "5 — барьер освобождает потоки, только когда вошли все 4; TRUE получает последний прибывший;",
    "6 — последний покинувший барьер поток проверяет инвариант (processed == items) и пересылает партию в следующую очередь;",
    "7 — Collector забирает партии из Q3 и проверяет, что каждая заявка прошла все ступени ровно один раз.",
]
for i, line in enumerate(legend):
    ax.text(1, 6.6 - i * 1.0, line, fontsize=6.8, va="center")

ax.set_title("Схема взаимодействия потоков и очередей (режим batch, вариант 19)",
             fontsize=11, fontweight="bold")
fig.tight_layout()
fig.savefig(os.path.join(IMG, "scheme.png"), facecolor="white")
print("OK ->", os.path.join(IMG, "scheme.png"))
