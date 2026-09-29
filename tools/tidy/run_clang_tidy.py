#!/usr/bin/env python3
"""リポジトリの C++ に clang-tidy(.clang-tidy の規則、docs/style.md)をかけ、警告が 1 つでもあれば失敗する。

入力はビルドフォルダの compile_commands.json(CMakePresets.json で出力している)。engine/ と tests/ のソースだけを見て、
警告もリポジトリのファイル(out/ と vcpkg を除く)のものだけを数える。同じヘッダの同じ警告は 1 回にまとめる。
使う所: PC では `job.py tidy`(scripts/jobs/tidy.ps1)、CI では build ジョブ(.github/workflows/ci.yml)。

MSVC(cl.exe)の compile_commands を clang-tidy に読ませるので、プリコンパイルヘッダの指定(/Yu)は /Y- で打ち消す。
MSVC が作った .pch を clang は読めないため。ヘッダ自体は /FI で読まれるので、解析の中身は変わらない。
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
SOURCE_DIRS = ("engine", "tests")
DIAGNOSTIC = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<column>\d+): (?P<level>warning|error): (?P<message>.*)$")
# HLSL と C++ で共通のヘッダ(shaders/**/*.hlsli)では、HLSL に無い書き方(auto・指示付き初期化子)を勧める検査を外す。
# 検査の設定は解析の起点の .cpp ごとに決まり、ヘッダの場所では変えられないので、ここで除く。
HLSL_UNSUPPORTED_CHECKS = ("[modernize-use-auto]", "[modernize-use-designated-initializers]")


def IsRepoFile(path: pathlib.Path) -> bool:
    try:
        relative = path.resolve().relative_to(REPO)
    except ValueError:
        return False
    return relative.parts[:1] != ("out",) and "vcpkg_installed" not in relative.parts


def SelectSources(buildDir: pathlib.Path) -> list[tuple[pathlib.Path, bool]]:
    """(ソース, cl.exe でコンパイルされるか) の一覧。"""
    entries = json.loads((buildDir / "compile_commands.json").read_text(encoding="utf-8"))
    selected = {}
    for entry in entries:
        source = pathlib.Path(entry["directory"], entry["file"]).resolve()
        if not IsRepoFile(source):
            continue
        if source.relative_to(REPO).parts[0] not in SOURCE_DIRS:
            continue
        command = entry.get("command") or " ".join(entry.get("arguments", []))
        selected[source] = "cl.exe" in command.lower()
    return sorted(selected.items())


def RunOne(clangTidy: str, buildDir: pathlib.Path, source: pathlib.Path, isMsvc: bool) -> str:
    command = [clangTidy, "-p", str(buildDir), "--quiet", "--header-filter=.*"]
    if isMsvc:
        command.append("--extra-arg=/Y-")
    command.append(str(source))
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return result.stdout + result.stderr


def Main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--clang-tidy", default="clang-tidy")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    sources = SelectSources(args.build_dir.resolve())
    if not sources:
        print("run_clang_tidy: compile_commands.json に engine/・tests/ のソースが無い")
        return 1
    version = subprocess.run([args.clang_tidy, "--version"], capture_output=True, text=True).stdout
    versionLine = next((line.strip() for line in version.splitlines() if "version" in line), "?")
    print(f"run_clang_tidy: {len(sources)} ファイル / {versionLine}")

    diagnostics = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        outputs = pool.map(lambda item: RunOne(args.clang_tidy, args.build_dir, *item), sources)
        for output in outputs:
            for line in output.splitlines():
                match = DIAGNOSTIC.match(line)
                if not match or not IsRepoFile(pathlib.Path(match["file"])):
                    continue
                relative = pathlib.Path(match["file"]).resolve().relative_to(REPO).as_posix()
                if relative.endswith(".hlsli") and match["message"].endswith(HLSL_UNSUPPORTED_CHECKS):
                    continue
                key = (relative, int(match["line"]), int(match["column"]), match["message"])
                diagnostics[key] = match["level"]

    for (file, line, column, message), level in sorted(diagnostics.items()):
        print(f"{file}:{line}:{column}: {level}: {message}")
    if diagnostics:
        print(f"run_clang_tidy: {len(diagnostics)} 件(.clang-tidy・docs/style.md)")
        return 1
    print("run_clang_tidy: 警告なし")
    return 0


if __name__ == "__main__":
    sys.exit(Main())
