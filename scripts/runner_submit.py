#!/usr/bin/env python3
"""
runner_submit.py — Claude 側(device_bash の Linux 環境)から Windows のジョブランナーへジョブを投げる。

使い方:
  python3 ~/job.py build                     # scripts/jobs/build.ps1 を実行(引数は後ろに続ける)
  python3 ~/job.py build -Preset release
  python3 ~/job.py test
  python3 ~/job.py run -- --caps             # エンジン実行ファイルに --caps を渡す
  python3 ~/job.py git status
  python3 ~/job.py raw path/to/script.ps1    # 任意の PowerShell を実行(最小限に)
  python3 ~/job.py check                     # ランナーの生存と鍵 ID の一致だけ確認

出力はコンテキストを食わないように要約する: 終了コード・時間・エラー行・末尾 N 行・成果物パス。
ログ全文が要るときだけ runner/logs/<job>.log を grep / 範囲指定で読む。

鍵: PC の %USERPROFILE%\.bicameral-runner\key.txt(固定)。チャットの最初にそのフォルダの接続を求めて読む。
    探す順: 環境変数 BICAMERAL_KEYFILE → ~/.bicameral-runner-key → ~/mnt/.bicameral-runner/key.txt
    鍵をリポジトリ・メモリ・文書に書き写さない。
ルート: 環境変数 BICAMERAL_ROOT(既定 ~/mnt/BicameralEngine)
"""
import hashlib, hmac, json, os, sys, time, datetime, re, pathlib

ROOT = pathlib.Path(os.environ.get("BICAMERAL_ROOT", os.path.expanduser("~/mnt/BicameralEngine")))
RUNNER = ROOT / "runner"
KEY_CANDIDATES = [p for p in [
    os.environ.get("BICAMERAL_KEYFILE"),
    os.path.expanduser("~/.bicameral-runner-key"),
    os.path.expanduser("~/mnt/.bicameral-runner/key.txt"),
] if p]
JOBS = {"build", "test", "run", "git", "env"}   # scripts/jobs/<name>.ps1


def die(msg, code=2):
    print(f"[job] {msg}")
    sys.exit(code)


def load_key():
    found = next((pathlib.Path(p) for p in KEY_CANDIDATES if pathlib.Path(p).exists()), None)
    if not found:
        die("鍵が見つかりません。PC の %USERPROFILE%\\.bicameral-runner フォルダの接続を依頼し、"
            "マウント先の key.txt を BICAMERAL_KEYFILE で指定してください(ランナーを一度も起動していなければ key.txt はまだ無い)。")
    k = found.read_text(encoding="utf-8-sig").strip().lower()
    if not re.fullmatch(r"[0-9a-f]{64}", k):
        die("鍵の形式が不正です(64 桁の16進数)。")
    return k


def key_id(k):
    return hashlib.sha256(k.encode("ascii")).hexdigest()[:8]


def check_runner(k):
    hb = RUNNER / "heartbeat.json"
    if not hb.exists():
        die("heartbeat.json がありません。ランナーが一度も起動していません。ユーザーに起動を頼んでください。")
    try:
        d = json.loads(hb.read_text(encoding="utf-8-sig"))
    except Exception as e:
        die(f"heartbeat.json を読めません: {e}")
    # PowerShell の ToString('o') は小数 7 桁 + オフセット。6 桁に丸めてタイムゾーン込みで比べる
    ts = re.sub(r"(\.\d{6})\d+", r"\1", str(d["time"])).replace("Z", "+00:00")
    t = datetime.datetime.fromisoformat(ts)
    if t.tzinfo is None:
        t = t.astimezone()
    age = (datetime.datetime.now(datetime.timezone.utc) - t).total_seconds()
    if d.get("state") == "stopped":
        die("ランナーは停止しています。ユーザーにランナーの起動を頼んでください。")
    if d.get("keyId") != key_id(k):
        die(f"鍵 ID が一致しません(ランナー {d.get('keyId')} / 手元 {key_id(k)})。鍵が作り直された(-RotateKey)可能性があります。key.txt を読み直してください。")
    # PC 側の時計とタイムゾーンがずれている可能性があるので、古さは警告にとどめる
    if not str(d.get("state", "")).startswith("running") and abs(age) > 120:
        print(f"[job] 注意: heartbeat が {int(age)} 秒前(時計ずれ or ランナー停止の可能性)")
    return d


def ps_quote(s):
    return "'" + s.replace("'", "''") + "'"


def build_body(argv):
    kind = argv[0]
    rest = argv[1:]
    timeout = None
    if "--timeout" in rest:
        i = rest.index("--timeout"); timeout = int(rest[i + 1]); del rest[i:i + 2]
    if kind == "raw":
        body = pathlib.Path(rest[0]).read_text(encoding="utf-8")
        name = "raw-" + pathlib.Path(rest[0]).stem
    elif kind in JOBS:
        if kind == "run" and "--" in rest:
            i = rest.index("--")
            args = ", ".join(ps_quote(a) for a in rest[i + 1:])
            pre = " ".join(a if a.startswith("-") else ps_quote(a) for a in rest[:i])
            call = f"& .\\scripts\\jobs\\run.ps1 {pre} -AppArgs @({args})"
        elif kind == "git":
            # git の -m などを PowerShell のパラメータと誤認させないよう、全部クォートして渡す
            call = "& .\\scripts\\jobs\\git.ps1 " + " ".join(ps_quote(a) for a in rest)
        else:
            parts = [a if a.startswith("-") else ps_quote(a) for a in rest]
            call = f"& .\\scripts\\jobs\\{kind}.ps1 " + " ".join(parts)
        body = call + "\nexit $LASTEXITCODE\n"
        name = kind + ("-" + re.sub(r"[^A-Za-z0-9_-]", "", rest[0])[:20] if rest and not rest[0].startswith("-") else "")
        if timeout is None:
            # ジョブスクリプト自身の "# timeout: N" を既定値として使う
            src = ROOT / "scripts" / "jobs" / f"{kind}.ps1"
            if src.exists():
                m = re.search(r"(?m)^#\s*timeout:\s*(\d+)", src.read_text(encoding="utf-8-sig"))
                if m: timeout = int(m.group(1))
    else:
        die(f"不明なジョブ種別: {kind}(build/test/run/git/env/raw/check)")
    if timeout:
        body = f"# timeout: {timeout}\n" + body
    return name, body


def submit(k, name, body):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    job = f"{stamp}-{name}"
    # 同じ内容のジョブ(build を 2 回など)も署名が変わるように nonce を入れる。ランナーは同じ署名を 2 回実行しない
    body = f"# nonce: {job}-{os.urandom(6).hex()}\n" + body
    data = body.replace("\r\n", "\n").encode("utf-8")
    sig = hmac.new(bytes.fromhex(k), data, hashlib.sha256).hexdigest()
    q = RUNNER / "queue"
    q.mkdir(parents=True, exist_ok=True)
    tmp = q / f"{job}.part"
    tmp.write_bytes(f"# sig: {sig}\n".encode("ascii") + data)
    tmp.rename(q / f"{job}.ps1")   # 書き終わってから .ps1 にする(ランナーは *.ps1 しか拾わない)
    return job


def wait(job, limit):
    res = RUNNER / "logs" / f"{job}.result.json"
    t0 = time.time()
    while not res.exists():
        if time.time() - t0 > limit:
            die(f"{job}: {limit} 秒待っても結果が出ません。ランナーの状態を heartbeat.json で確認してください。", 3)
        time.sleep(2)
    time.sleep(0.3)
    return json.loads(res.read_text(encoding="utf-8-sig"))


def _line(e):
    # 旧ランナーは行を PowerShell のオブジェクトのまま JSON にしていた({"value": ...})
    if isinstance(e, dict):
        e = e.get("value", "")
    return str(e)


def show(r, tail):
    print(f"[job] {r['job']}  exit={r['exit']}  {r.get('seconds', '?')}s  log={r.get('lines', '?')} 行")
    if r.get("reason"):
        print(f"[job] 理由: {r['reason']}")
    errs = r.get("errors") or []
    if errs:
        print(f"--- エラー候補 {len(errs)} 行 ---")
        for e in errs[:25]:
            print("  " + _line(e)[:300])
    t = (r.get("tail") or [])[-tail:]
    if t:
        print(f"--- 末尾 {len(t)} 行 ---")
        for e in t:
            print("  " + _line(e)[:300])
    if r.get("outputs"):
        print("--- 成果物 (runner/ 以下) ---")
        for o in r["outputs"][:20]:
            print("  " + o)


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__); return
    tail = 25
    if "--tail" in argv:
        i = argv.index("--tail"); tail = int(argv[i + 1]); del argv[i:i + 2]
    k = load_key()
    hb = check_runner(k)
    if argv[0] == "check":
        print(f"[job] OK  keyId={hb['keyId']}  state={hb['state']}  project={hb.get('project')}"); return
    name, body = build_body(argv)
    job = submit(k, name, body)
    print(f"[job] 投入: {job}")
    limit = 3700
    m = re.search(r"(?m)^#\s*timeout:\s*(\d+)", body)
    if m: limit = int(m.group(1)) + 120
    r = wait(job, limit)
    show(r, tail)
    sys.exit(0 if str(r["exit"]) == "0" else 1)


if __name__ == "__main__":
    main()
