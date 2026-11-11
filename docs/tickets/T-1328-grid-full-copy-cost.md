# T-1328 仮の世界の放射の写しの段の全部写す刻みを main 以下に(T-1228 段 1 の RcSources の一覧の続き)

- Status: Doing
- 種類: 工学
- PC: 必須(GPU の測り)
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列(docs/plan/ROADMAP.md の T-1228 の続き)
- 設計: ADR-0500 決定 4 / T-1228 段 1(docs/tickets/T-1228-changed-mark-source-term.md「RcSources の一覧の作り」)

## 目的(1〜2 行)
T-1228 段 1(RcSources の放つセルの一覧)の後、全部写す刻み(作った直後・保存点へ戻す・写しの早送り・比べる試験の「いつも全部」)の
RadiationGrid が main 73〜76 → 76〜85 µs に重く測れた件を、結果を変えずに main 以下へ。

## 完了条件(チェックできる形で)
- [ ] 結果が変わらない: gpu_radiation_cost(一覧の有無・写しは毎刻み全部と刻みごとのハッシュ一致・飛ばした刻みの数が同じ)・gpu_probe_sim・gpu_probe_rewind・gpu_world_copy・probe_radiation
- [ ] 費用: 同じ回で main・T-1228 の後(直す前)・直した後を交互に 2 回ずつ(240 刻み)。全部写す刻みの Grid が main 以下か(perf.md)
- [ ] release のビルド(警告 0)・archmap --check

## メモ・参考
- 見立て(2026-11-12 wt4。t-1228-rc-sources 5d88e46c の測りの log 20261111-195152-run を読んだ): 一覧の bit を直すのは写しの段の
  「値が変わり・放つかが変わった」セルだけ(1 刻みに数十〜数百)で、原子操作の数は小さい。しかも **0 K の空(1 側。一覧の道を通らず、
  写しの段のコードは main と同じ)でも全部写す刻みが 80.0〜80.2 µs**(main 73〜76)。だから +2〜6 µs の多くは回ごとの揺れ
  (別の時間に測った main との比べ)か、当てる段の 0 戻し(ClearRadiationSide)の書き込みが次の刻みに残る分と見る(未確認)。
  → 同じ GPU の順番待ちの中で main・直す前・直した後を交互に流して比べるのが先(下の runner/t1328-ab.ps1)。
- 試した案 1(WIP・未ビルド): UpdateEmitBits = 波の中で同じ語のセルをまとめ(WaveActiveMin で語を 1 つずつ・WaveActiveBitOr で立てる/下ろす bit)、
  1 語に 1 回ずつ原子操作。呼ぶ所を分かれ道の外(波で揃った所 = WaveActiveSum の前)へ移した。bit は重ならないので結果は同じ見込み。

## 引き継ぎ(2026-11-12 wt4 2 本目。入れ物の再起動で区切り)
- どこまで: ブランチ t-1328-grid-emit-bits(t-1228-rc-sources 5d88e46c の上。rebase main はまだ)に案 1 とこのチケットを WIP でコミット。**ビルドも試験もまだ**。
- 用意: wt4 の runner/ab_main/ = main と同じ放射のコードの gpu_radiation_cost_test(t-1273 の release から写した。exe・shaders・D3D12・data)。
  runner/t1328-ab.ps1 = 1 回の GPU の順番待ちで ab_main・ab_v0・今のビルドを 2 周 `--measure --ticks 240`(raw で投げる。ab_v0 はまだ無い)。
- 次の一手: (1) t-1228-rc-sources のまま release → bin の 5 つを runner/ab_v0/ へ写す(直す前)。(2) このブランチで release(警告 0)。
  (3) raw runner/t1328-ab.ps1 → 「写しは毎刻み全部」の Grid を 3 つで比べる。直す前と main が同じ回で同じなら、+2〜6 は揺れとして閉じる案を書く。
  (4) 案 1 を残すなら gpu_radiation_cost・gpu_probe_sim・gpu_probe_rewind・gpu_world_copy・probe_radiation → rebase main → archmap → perf.md → 印。
  案 1 で変わらなければ戻す(結果は同じなので、どちらでもよい)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-12(wt4 2 本目): チケットを作った。案 1 を WIP で入れた(未ビルド)。上の引き継ぎ。
