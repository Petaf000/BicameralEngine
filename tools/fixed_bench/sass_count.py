"""sass_count.py — マイクロベンチの演算ごとの NVIDIA の機械語(SASS)の命令数を、Nsight Graphics の GPU Trace で実測する(T-0093、04 §6)。

SASS を直接読む手段(ドライバのキャッシュ GetCachedBlob は圧縮されていて読めない)が無いので、
実行した命令の数(ハードウェアのカウンタ sm__inst_executed)を数え、1 スレッドの 1 歩(演算 + 混ぜる)あたりに割る。
静的な命令数ではなく**動的な**命令数(ループを回った分も数える)。ループの出入りの命令も少し入る。

1. `python3 tools/fixed_bench/sass_count.py ps1 <出力フォルダ(リポジトリの根からの Windows のパス。例 out\\nsight\\bench)> <演算...> > ~/w/b.ps1` → `python3 ~/job.py raw ~/w/b.ps1`
   (1 演算 7 秒前後。device_bash の 180 秒に収まるよう 6 個ずつ)
   gpu_fixed_bench を `--only <演算> --iterations 16` で走らせ、3 回目の投入(調整なしの 5 回の計測の 2 回目)だけを記録して指標を書き出す。
   性能カウンタは NVIDIA コントロールパネルの「すべてのユーザーに GPU パフォーマンスカウンタへのアクセスを許可」が要る。
2. `python3 tools/fixed_bench/sass_count.py report <出力フォルダ(Linux のパス)> [演算...]` → Markdown の表
"""

import sys
from pathlib import Path

ORDER = ("base32 base64 add32 mul32 div32 add64 mul64 div64 fadd fmul fdiv mulshift32 mulshift64 mulfull128 divs64 "
         "divshift64 div128 recip32 recip64 recips64 makerecip64 sqrt32 sqrt64 exp2 log2 exp ln sincos hash64 solve6").split()
WIDTH_32 = {"base32", "add32", "mul32", "div32", "mulshift32", "recip32", "sqrt32", "sincos"}
FLOAT = {"fadd", "fmul", "fdiv"}

# gpu_fixed_bench.cpp の既定(4096 グループ × 256 スレッド)・BENCH_UNROLL・ps1 で渡す反復回数
THREADS = 4096 * 256
ITERATIONS = 16
UNROLL = 4

# ランナーの raw はリポジトリの根で走る。Nsight Graphics の版が変わったら NGFX を直す
NGFX = r"$env:ProgramFiles\NVIDIA Corporation\Nsight Graphics 2025.2.0\host\windows-desktop-nomad-x64\ngfx.exe"


def write_ps1(output_root: str, operations: list[str]) -> None:
    print(f'$ng = "{NGFX}"')
    print('$bin = (Resolve-Path "out\\build\\release\\bin").Path')
    for name in operations:
        print(f'$out = Join-Path (Get-Location) "{output_root}\\{name}"')
        print("New-Item -ItemType Directory -Force $out | Out-Null")
        print('& $ng --activity "GPU Trace Profiler" --platform Windows --exe "$bin\\gpu_fixed_bench.exe" --dir $bin '
              f'--args "--only {name} --iterations {ITERATIONS}" --output-dir $out --start-after-submits 2 '
              '--limit-to-submits 1 --architecture "Ampere GA10x" --metric-set-id 1 --auto-export '
              '--collect-screenshot 0 *> "$out\\ngfx_log.txt"')
        print(f'"{name} exit=$LASTEXITCODE"')


def load_metrics(folder: Path) -> dict[str, float] | None:
    files = list(folder.glob("BASE/GPUTRACE_FRAME.xls"))
    if not files:
        return None

    metrics = {}
    for line in files[0].read_text(encoding="latin-1").splitlines():
        name, _, value = line.partition("\t")
        try:
            metrics[name] = float(value)
        except ValueError:
            pass

    return metrics


def metric(metrics: dict[str, float], suffix: str) -> float:
    return next(value for name, value in metrics.items() if name.endswith(suffix))


def report(output_root: Path, operations: list[str]) -> None:
    warp_steps = THREADS * ITERATIONS * UNROLL / 32
    per_step = {}
    for name in operations:
        metrics = load_metrics(output_root / name)
        if metrics is None:
            continue

        ratio = metric(metrics, "smsp__thread_inst_executed_per_inst_executed.ratio") / 32
        per_step[name] = {
            "all": metric(metrics, "sm__inst_executed_realtime.sum") * ratio / warp_steps,
            "alu": metric(metrics, "sm__inst_executed_pipe_alu_realtime.sum") * ratio / warp_steps,
            "fma": metric(metrics, "smsp__inst_executed_pipe_fma.sum") * ratio / warp_steps,
            "xu": metric(metrics, "sm__inst_executed_pipe_xu_realtime.sum") * ratio / warp_steps,
        }

    print("| 演算 | SASS 命令/歩 | 混ぜる分を引いた SASS 命令/回 | うち ALU / FMA / XU |")
    print("|---|---|---|---|")
    for name, counts in per_step.items():
        base_name = None if name in FLOAT or name.startswith("base") else "base32" if name in WIDTH_32 else "base64"
        base = per_step.get(base_name, {"all": 0, "alu": 0, "fma": 0, "xu": 0}) if base_name else None
        net = "—" if base is None else f"{counts['all'] - base['all']:.1f}"
        pipes = "/".join(f"{counts[p] - (base[p] if base else 0):.1f}" for p in ("alu", "fma", "xu"))
        print(f"| {name} | {counts['all']:.1f} | {net} | {pipes} |")


def main() -> None:
    if len(sys.argv) < 3 or sys.argv[1] not in ("ps1", "report"):
        print(__doc__)
        sys.exit(2)

    operations = sys.argv[3:] or ORDER
    if sys.argv[1] == "ps1":
        write_ps1(sys.argv[2], operations)
    else:
        report(Path(sys.argv[2]), operations)


if __name__ == "__main__":
    main()
