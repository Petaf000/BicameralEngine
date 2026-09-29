#!/usr/bin/env python3
"""archmap — 手で書いた流れ図の定義から、ノードをクリックするとソースの行へ飛べる図のページを作る(T-0006 案 C)。

データの流れ:
  docs/architecture/map.yaml(ノード → ファイル::シンボル、設計書、チケット)
    → このスクリプトがシンボルを探して行番号を解決する
    → out/site/index.html(Mermaid の図。ノードのクリックで GitHub のソース行・設計書へ飛ぶ)

リンク切れ(関数の移動・改名・ファイルの削除)は解決に失敗するので、`--check` は 1 を返して CI を落とす。
こうして図とコードがずれないようにする。依存は PyYAML だけ(pip install pyyaml)。

使い方:
  python tools/archmap/archmap.py --check                       # 解決できるかだけ調べる
  python tools/archmap/archmap.py --out out/site --rev <sha>    # ページを作る(リンクはその版に固定)
"""
from __future__ import annotations

import argparse
import html
import json
import pathlib
import re
import sys

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
MAP_FILE = ROOT / "docs" / "architecture" / "map.yaml"
# done: 実装済み(code 必須)/ planned: 計画 / research: 研究項目 / external: OS やドライバなどエンジンの外
STATUSES = ("done", "planned", "research", "external")

# --- シンボルの解決 ---------------------------------------------------------------------------
# C++ を構文解析はしない。ここのコードの書き方(定義は行頭から型で始まり、宣言は ; で終わる)を前提にした
# 正規表現で「定義の行」を探す。候補が 0 個でも 2 個以上でもエラーにする(曖昧なリンクを作らない)。

_TYPE_PREFIX = r"[\w:<>,\*&\s]*?"          # 戻り値の型・修飾子(const char* など)
_KEYWORDS = {"return", "if", "else", "while", "for", "switch", "case", "do", "new", "delete", "throw"}


def _strip_comment(line: str) -> str:
    return line.split("//", 1)[0]


def _find_candidates(lines: list[str], symbol: str) -> list[int]:
    """symbol の定義らしい行(0 始まり)を返す。関数 → 型 → 変数 の順に探し、最初に見つかった種類を使う。"""
    name = re.escape(symbol)
    function_re = re.compile(rf"^(\s*)({_TYPE_PREFIX})(?<![\w:]){name}\s*\(")
    type_re = re.compile(rf"^\s*(?:template\s*<.*>\s*)?(class|struct|union|enum(?:\s+class)?)\s+{name}\b")
    # 変数は __declspec(dllexport) などの括弧を前に許す(Agility SDK のエクスポート)
    variable_re = re.compile(rf"^\s*[^=;{{}}]*?(?<![\w:]){name}\s*(=|\{{|;)")

    def is_code(line: str) -> bool:
        s = line.strip()
        return bool(s) and not s.startswith(("//", "#", "*", "/*"))

    functions, types, variables = [], [], []
    for i, raw in enumerate(lines):
        if not is_code(raw):
            continue
        line = _strip_comment(raw).rstrip()
        m = function_re.match(line)
        if m:
            words = set(re.findall(r"\w+", m.group(2)))
            # 呼び出し(return Foo(); や if (...) Foo();)と宣言(… ;)を除く
            if not (words & _KEYWORDS) and not line.endswith(";") and "=" not in m.group(2):
                functions.append(i)
            continue
        if type_re.match(line) and not line.endswith(";"):
            types.append(i)
            continue
        if variable_re.match(line) and not (set(re.findall(r"\w+", line.split(symbol)[0])) & _KEYWORDS):
            variables.append(i)
    return functions or types or variables


def _block_end(lines: list[str], start: int) -> int:
    """start 行から始まる定義の閉じ括弧の行を返す。{ が無い(変数など)ならその文の終わり。"""
    depth, opened = 0, False
    for i in range(start, len(lines)):
        code = re.sub(r'"(\\.|[^"\\])*"|\'(\\.|[^\'\\])*\'', "", _strip_comment(lines[i]))
        for ch in code:
            if ch == "{":
                depth, opened = depth + 1, True
            elif ch == "}":
                depth -= 1
                if opened and depth == 0:
                    return i
        if not opened and code.rstrip().endswith(";"):
            return i
    return start


def resolve(ref: str) -> tuple[str, int, int]:
    """'path::Symbol' または 'path::Class::Method' を (path, 開始行, 終了行)(1 始まり)にする。"""
    path, sep, symbol = ref.partition("::")
    if not sep or not symbol:
        raise ValueError(f"'{ref}' は 'ファイル::シンボル' の形で書く")
    file = ROOT / path
    if not file.is_file():
        raise ValueError(f"ファイルがない: {path}")
    lines = file.read_text(encoding="utf-8").splitlines()

    hits = _find_candidates(lines, symbol) if "::" in symbol else []
    if not hits:
        hits = _find_candidates(lines, symbol.rsplit("::", 1)[-1])
    if not hits:
        raise ValueError(f"{path} に '{symbol}' の定義が見つからない(移動・改名された?)")
    if len(hits) > 1:
        where = ", ".join(str(h + 1) for h in hits)
        raise ValueError(f"{path} の '{symbol}' が複数ある(行 {where})。map.yaml で修飾名にする")
    start = hits[0]
    return path, start + 1, _block_end(lines, start) + 1


# --- 地図の読み込みと検証 -----------------------------------------------------------------------

def _ticket_path(ticket: str) -> str:
    found = sorted((ROOT / "docs" / "tickets").glob(f"{ticket}-*.md"))
    if not found:
        raise ValueError(f"チケットがない: {ticket}")
    return found[0].relative_to(ROOT).as_posix()


def load_map() -> tuple[dict, list[str]]:
    """map.yaml を読み、各ノードの code / doc / ticket を解決する。返り値の 2 つ目はエラーの一覧。"""
    data = yaml.safe_load(MAP_FILE.read_text(encoding="utf-8"))
    errors: list[str] = []
    for view in data["views"]:
        ids = set()
        for node in view["nodes"]:
            nid = node["id"]
            if nid in ids:
                errors.append(f"[{view['id']}] ノード id の重複: {nid}")
            ids.add(nid)
            try:
                if "code" in node:
                    node["_code"] = resolve(node["code"])
                if "doc" in node:
                    doc = node["doc"].split("#", 1)[0]
                    if not (ROOT / doc).is_file():
                        raise ValueError(f"設計書がない: {doc}")
                if "ticket" in node:
                    node["_ticket"] = _ticket_path(node["ticket"])
                status = node.get("status", "done")
                if status not in STATUSES:
                    raise ValueError(f"status は {', '.join(STATUSES)} のどれか: {status}")
                if status == "done" and "code" not in node:
                    raise ValueError("status: done のノードには code が要る")
            except ValueError as e:
                errors.append(f"[{view['id']}/{nid}] {e}")
        for edge in view.get("edges", []):
            for end in edge[:2]:
                if end not in ids:
                    errors.append(f"[{view['id']}] 辺の端 '{end}' がノードにない")
    return data, errors


# --- ページの生成 -----------------------------------------------------------------------------

def _node_link(repo: str, rev: str, node: dict) -> str | None:
    base = f"https://github.com/{repo}/blob/{rev}/"
    if "_code" in node:
        path, start, end = node["_code"]
        return f"{base}{path}#L{start}" + (f"-L{end}" if end > start else "")
    if "doc" in node:
        return base + node["doc"]
    if "_ticket" in node:
        return base + node["_ticket"]
    return None


def _mermaid(view: dict, repo: str, rev: str) -> str:
    out = [f"flowchart {view.get('direction', 'TD')}"]
    groups: dict[str, list[dict]] = {}
    for node in view["nodes"]:
        groups.setdefault(node.get("group", ""), []).append(node)
    group_labels = {g["id"]: g["label"] for g in view.get("groups", [])}

    def node_line(n: dict) -> str:
        label = n["label"].replace('"', "#quot;")
        return f'    {n["id"]}["{label}"]:::{n.get("status", "done")}'

    for group, nodes in groups.items():
        if group:
            out.append(f'  subgraph {group}["{group_labels.get(group, group)}"]')
        out.extend(node_line(n) for n in nodes)
        if group:
            out.append("  end")
    for edge in view.get("edges", []):
        arrow = "-.->" if len(edge) > 3 and edge[3] == "async" else "-->"
        label = f'|"{edge[2]}"|' if len(edge) > 2 and edge[2] else ""
        out.append(f"  {edge[0]} {arrow}{label} {edge[1]}")
    for n in view["nodes"]:
        link = _node_link(repo, rev, n)
        if link:
            out.append(f'  click {n["id"]} href "{link}" _blank')
    out.append("  classDef done fill:#dff3e4,stroke:#2f7d45,color:#12351d")
    out.append("  classDef planned fill:#f4f4f5,stroke:#8a8a93,color:#3f3f46,stroke-dasharray:5 4")
    out.append("  classDef research fill:#fdf1dc,stroke:#b7791f,color:#5b3a0a,stroke-dasharray:2 3")
    out.append("  classDef external fill:#e8eefc,stroke:#4a64a8,color:#1b2a52")
    return "\n".join(out)


def _table(view: dict, repo: str, rev: str) -> str:
    status_text = {"done": "動いている", "planned": "計画", "research": "研究", "external": "外部"}
    rows = []
    for n in view["nodes"]:
        link = _node_link(repo, rev, n)
        if "_code" in n:
            path, start, _ = n["_code"]
            where = f"{path}:{start}"
        else:
            where = n.get("doc") or n.get("_ticket") or ""
        name = html.escape(re.sub(r"<br\s*/?>", " — ", n["label"]))
        cell = f'<a href="{html.escape(link)}" target="_blank" rel="noopener">{html.escape(where)}</a>' if link else ""
        st = n.get("status", "done")
        rows.append(f'<tr><td>{name}</td><td><span class="st {st}">{status_text.get(st, st)}</span></td>'
                    f"<td><code>{cell}</code></td><td>{html.escape(n.get('note', ''))}</td></tr>")
    return ("<table><thead><tr><th>ノード</th><th>状態</th><th>どこ</th><th>メモ</th></tr></thead><tbody>"
            + "".join(rows) + "</tbody></table>")


def render(data: dict, rev: str) -> str:
    repo = data["repo"]
    template = (pathlib.Path(__file__).with_name("template.html")).read_text(encoding="utf-8")
    tabs, panels = [], []
    for i, view in enumerate(data["views"]):
        selected = "true" if i == 0 else "false"
        tabs.append(f'<button role="tab" aria-selected="{selected}" data-view="{view["id"]}">'
                    f'{html.escape(view["title"])}</button>')
        panels.append(
            f'<section class="view" id="view-{view["id"]}"{"" if i == 0 else " hidden"}>'
            f'<p class="lead">{html.escape(view.get("description", ""))}</p>'
            f'<pre class="mermaid">{html.escape(_mermaid(view, repo, rev))}</pre>'
            f"{_table(view, repo, rev)}</section>")
    short_rev = rev[:7] if re.fullmatch(r"[0-9a-f]{40}", rev) else rev
    return (template.replace("{{TABS}}", "".join(tabs))
                    .replace("{{PANELS}}", "".join(panels))
                    .replace("{{REPO}}", html.escape(repo))
                    .replace("{{REV}}", html.escape(rev))
                    .replace("{{REV_SHORT}}", html.escape(short_rev))
                    .replace("{{TITLE}}", html.escape(data.get("title", "Architecture map"))))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="解決できるかだけ調べる(失敗なら終了コード 1)")
    ap.add_argument("--out", default="out/site", help="出力フォルダ(index.html を書く)")
    ap.add_argument("--rev", default="main", help="リンク先の版(CI ではコミットの SHA)")
    ap.add_argument("--json", action="store_true", help="解決結果を JSON で表示する")
    args = ap.parse_args()

    data, errors = load_map()
    for e in errors:
        print(f"archmap: エラー: {e}", file=sys.stderr)
    if errors:
        print(f"archmap: {len(errors)} 件のリンク切れ・定義の誤り。docs/architecture/map.yaml を直す", file=sys.stderr)
        return 1
    count = sum(1 for v in data["views"] for n in v["nodes"] if "_code" in n)
    if args.json:
        print(json.dumps({v["id"]: {n["id"]: n.get("_code") for n in v["nodes"]} for v in data["views"]},
                         ensure_ascii=False, indent=1))
    if args.check:
        print(f"archmap: OK(コードへのリンク {count} 個を解決)")
        return 0
    out = ROOT / args.out
    out.mkdir(parents=True, exist_ok=True)
    (out / "index.html").write_text(render(data, args.rev), encoding="utf-8")
    print(f"archmap: {out / 'index.html'} を書いた(コードへのリンク {count} 個、版 {args.rev})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
