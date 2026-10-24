import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(BASE, "report", "img")
os.makedirs(IMG, exist_ok=True)

fig, ax = plt.subplots(figsize=(11, 3.6), dpi=150)
ax.set_xlim(0, 110)
ax.set_ylim(0, 36)
ax.axis("off")


def box(x, y, w, h, text, fc="#dbe9f6", ec="#31688e", fs=9, bold=False):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.4",
                                fc=fc, ec=ec, lw=1.2))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center", fontsize=fs,
            fontweight="bold" if bold else "normal")


def arrow(x1, x2, y, label):
    ax.add_patch(FancyArrowPatch((x1, y), (x2, y), arrowstyle="-|>",
                                 mutation_scale=12, color="#444444", lw=1.2))
    ax.text((x1 + x2) / 2, y + 1.6, label, ha="center", fontsize=7.5, color="#444444")


box(1, 12, 12, 12, "Generator\n1 поток\nSEED=8081", fc="#fde9d9", ec="#b35c00")
xs = 21
for i in range(3):
    arrow(xs - 7, xs, 18, f"Q{i}\ncap=16")
    box(xs, 4, 20, 28, "", fc="#eef5ee", ec="#3a7d44")
    ax.text(xs + 10, 29.5, f"Stage {i + 1}  (4 потока)", ha="center",
            fontsize=9, fontweight="bold")
    for w in range(4):
        box(xs + 1.6 + (w % 2) * 9.4, 18.5 - (w // 2) * 6.2, 8, 5, f"W{w}",
            fc="#ffffff", ec="#3a7d44", fs=8)
    ax.plot([xs + 2, xs + 18], [11.2, 11.2], color="#c0392b", lw=2)
    ax.text(xs + 10, 8.2, "Synchronization Barrier\n(партия из 4 заявок)",
            ha="center", fontsize=7.5, color="#c0392b")
    ax.text(xs + 10, 5.2, "batch: партия уходит после\nвсех 4 потоков", ha="center",
            fontsize=7, color="#555555")
    xs += 27
arrow(xs - 7, xs, 18, "Q3\ncap=16")
box(xs, 12, 14, 12, "Collector\n1 поток", fc="#f3e1f7", ec="#7d3c98")

ax.set_title("Архитектура программы: Generator → 3 ступени × 4 потока → Collector\n"
             "ограниченные очереди между ступенями; в режиме batch ступень синхронизируется барьером",
             fontsize=10)
fig.tight_layout()
fig.savefig(os.path.join(IMG, "scheme.png"))
print("OK ->", os.path.join(IMG, "scheme.png"))
