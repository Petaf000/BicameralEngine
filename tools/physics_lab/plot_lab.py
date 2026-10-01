"""plot_lab.py — physics_lab の書き出し(<場面>_bodies.csv・<場面>_stats.csv)を画像にする(T-0016)。

使い方: python3 plot_lab.py <フォルダ> <場面> [<比べるフォルダ>]
  <フォルダ>/<場面>_snapshots.png: 何枚かの時刻の箱(斜めから見た図)
  <フォルダ>/<場面>_stats.png: 食い込み・最大の速さ・力学的エネルギーの時間変化(比べるフォルダがあれば重ねる)
画像はリポジトリに入れない(out/ の下に出す)。
"""
import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Poly3DCollection  # noqa: E402

TICKS_PER_SECOND = 60
FACES = [(0, 1, 3, 2), (4, 5, 7, 6), (0, 1, 5, 4), (2, 3, 7, 6), (0, 2, 6, 4), (1, 3, 7, 5)]


def rotation_matrix(qx, qy, qz, qw):
    return np.array([
        [1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qw * qz), 2 * (qx * qz + qw * qy)],
        [2 * (qx * qy + qw * qz), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qw * qx)],
        [2 * (qx * qz - qw * qy), 2 * (qy * qz + qw * qx), 1 - 2 * (qx * qx + qy * qy)],
    ])


def box_corners(row):
    half = np.array([row["hx"], row["hy"], row["hz"]])
    signs = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)])
    local = signs * half
    world = local @ rotation_matrix(row["qx"], row["qy"], row["qz"], row["qw"]).T + np.array([row["px"], row["py"], row["pz"]])
    # matplotlib の z を上にする(シミュの y が上)
    return world[:, [0, 2, 1]]


def load_bodies(path):
    frames = defaultdict(list)
    with open(path, encoding="utf-8") as file:
        for raw in csv.DictReader(file):
            row = {key: float(value) for key, value in raw.items()}
            if row["active"] == 1:
                frames[int(row["tick"])].append(row)
    return frames


def plot_snapshots(folder, scene):
    frames = load_bodies(folder / f"{scene}_bodies.csv")
    ticks = sorted(frames)
    chosen = [ticks[int(i * (len(ticks) - 1) / 5)] for i in range(6)]
    figure = plt.figure(figsize=(18, 10))
    for index, tick in enumerate(chosen):
        axes = figure.add_subplot(2, 3, index + 1, projection="3d")
        for row in frames[tick]:
            if row["hx"] > 20:  # 地面は描かない
                continue
            corners = box_corners(row)
            static = row["hy"] >= 1.4 or row["hx"] >= 1.6 or row["hz"] >= 1.6
            color = (0.7, 0.7, 0.7, 0.15) if static else (0.85, 0.55, 0.3, 0.8)
            axes.add_collection3d(Poly3DCollection([[corners[i] for i in face] for face in FACES], facecolor=color,
                                                   edgecolor=(0.2, 0.2, 0.2, 0.6), linewidth=0.3))
        extent = 5 if scene == "pile" else 3
        height = 10 if scene == "stack" else (8 if scene == "pile" else 3)
        axes.set_xlim(-extent, extent)
        axes.set_ylim(-extent, extent)
        axes.set_zlim(0, height)
        axes.set_box_aspect((2 * extent, 2 * extent, height))
        axes.view_init(elev=18, azim=-60)
        axes.set_title(f"{scene}  t = {tick / TICKS_PER_SECOND:.1f} s")
    figure.tight_layout()
    figure.savefig(folder / f"{scene}_snapshots.png", dpi=80)
    plt.close(figure)


def load_stats(path):
    with open(path, encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    return {key: np.array([float(r[key]) for r in rows]) for key in rows[0]}


def plot_stats(folder, scene, compare):
    series = [("integer" if "int" in folder.name else "this", load_stats(folder / f"{scene}_stats.csv"))]
    if compare is not None:
        series.append(("double" if "int" in folder.name else "compare", load_stats(compare / f"{scene}_stats.csv")))
    figure, axes = plt.subplots(3, 1, figsize=(12, 9), sharex=True)
    for label, stats in series:
        seconds = stats["tick"] / TICKS_PER_SECOND
        axes[0].plot(seconds, stats["max_penetration"] * 1000, label=label, linewidth=0.8)
        axes[1].plot(seconds, stats["max_speed"], label=label, linewidth=0.8)
        axes[2].plot(seconds, stats["energy"] - stats["spawned_energy"], label=label, linewidth=0.8)
    axes[0].set_ylabel("max penetration (mm)")
    axes[1].set_ylabel("max speed (m/s)")
    axes[1].set_yscale("symlog", linthresh=0.01)
    axes[2].set_ylabel("energy - spawned PE (J)")
    axes[2].set_xlabel("time (s)")
    for a in axes:
        a.grid(alpha=0.3)
        a.legend()
    figure.tight_layout()
    figure.savefig(folder / f"{scene}_stats.png", dpi=80)
    plt.close(figure)


def main():
    folder = Path(sys.argv[1])
    scene = sys.argv[2]
    compare = Path(sys.argv[3]) if len(sys.argv) > 3 else None
    plot_snapshots(folder, scene)
    plot_stats(folder, scene, compare)


if __name__ == "__main__":
    main()
