# T-0038 ストリーミング(DirectStorage)と追い出し・主メモリへの退避

- Status: Todo(2026-10-09 に司令塔が事前調査を置いた。着手はまだ)
- 設計: 05 §3・§7・§8・17・15・ADR-0015・ADR-0016・ADR-0018。関係: T-0037(生成器)・T-0039(粗い層の成長・天候)

## 事前調査(2026-10-09、調べ役。読むだけ。未確認の数値は「推定」「未確認」)

### 今の実装の要点
- 木は 8³ ブロック。見出し(MrBlock 約 120B)とセルの頁(512 セル)は別のプールで、頁なし = 一様。RxCell 112B なので 1 頁は 1 世代 56KiB、2 世代と熱で約 125KiB。枠と頁の番号は累積和で決まり決定的(ADR-0016)。HashWholeNest は頁の番号まで含み、HashRealLeaves は論理のセルだけ。
- メモリを減らす手段はもう 3 つ: 一様な頁を畳む(T-0103)・許容差の中で畳む(T-0104・D-435)・静かな葉を粗くする(T-0101・D-430)。ストリーミング・主メモリへの退避・DirectStorage のコードは無い。生成器(T-0037)も未着手。
- **頁が足りない時の今の扱い**: そのブロックだけその刻みを刻まない(MR_COUNTER_PAGE_SHORTAGE)・要求も後回し(MR_COUNTER_NO_SPACE)。どちらも容量で世界の結果が変わり、VRAM が違う機械では結果がずれる(ADR-0008・D-202 に反する)。
- 眠っているブロックは wakeTick までは「全部を計算しても何も変わらない」(ADR-0018)→ **追い出した間の時間を進める必要は無い**。
- 05 §8 は「静かなブロックだけ追い出す」「活性が VRAM を超えたら主メモリに退避して分割で計算」と書くだけ。粗い 8m の層は常に VRAM(D-312)。

### 結論(推奨)
1. **原則は「どこに置かれているかは世界に漏らさない」**。合否の基準: 小さいプール + 追い出しと十分大きいプールで、論理の要約が毎刻み一致。最初に直す 2 つ(工学):
   - 頁の不足と要求の後回しを「ブロックごとに刻まない」から「**世界全体のその刻みを始めない**(D-202 の遅れ)」に。
   - 要約を論理(世界)と物理(頁の番号)に分ける。
2. **今のプールの上にソフトウェアで頁を出し入れ**: 追い出す時は見出しを VRAM に残し page = EVICTED と退避先の番号。頁のバイトは刻みの境界で GPU が CopyBufferRegion で主メモリのバッファへ(静かなブロックは 2 世代が同じなので 1 世代だけ)。**CPU はバイトを解釈しない**(退避先の確保・ディスクへの書き出し〔セーブ I/O の枠〕・DirectStorage の発行だけ)。
3. **読み込みの決定性**: 刻み t に要るブロックの集合は刻み t の前に GPU が決める(起こす刻みが来た所・つつかれた所・活性の周りの余白・観察の枠)。全部が載るまで刻み t を始めない。先読み: 起こす刻みは前から正確に分かるので L 刻み前に読み始める。I/O の終わる時刻は刻みの境界でしか世界に見えないので、遅い時間を待つだけで結果は変わらない。
4. **追い出す順番**(性能だけに効くので距離を使ってよい): ① 生成器の出力と同じブロックは捨てて作り直す(T-0037 の後)② 起こす刻みが遠い・最後に変わったのが古い・活性から遠い・観測者から遠い順。追い出さない: 活性・余白・観察の影(D-403)・処理中の要求。畳む・粗くするのが第一の手段で、追い出しはそれで減らせない細部(D-430)のため。

### 方式の比較
- ソフトウェアの出し入れ(推奨): 今の page の引き当てがそのまま使える・粒度 56KiB・GPU が決め WARP でも試験できる。優先度と転送の段取りは自前。
- Reserved / Tiled Resources: 64KiB のタイルに頁がほぼ収まるが、割り当ては CPU の UpdateTileMappings で固定費が高いという指摘(数値未確認)。使うならプールを伸び縮みさせる用途に限る。
- WDDM に任せる: コード不要だが止まり方が読めず粒度も粗い → 最後の逃げ道。
- 圧縮: 転送前に GPU で詰める(空いた成分の枠を落とす)のが安い。ディスク用は GDeflate(1.1 から安定)か Zstd(1.4 はプレビュー・GPU 解凍のドライバ最適化は 2026 年後半予定)。
- 事例: Minecraft・World Partition は範囲外を止める(観測者で世界が変わる。D-208 と合わない)。Nanite は要る分だけ読み使っていないものを追い出す(根に近い粗い部分を残す = D-312 と同じ形)。GVDB は仮想メモリ式のアトラス、SPGrid は OS の仮想メモリの頁。NanoVDB は形が変わらない前提で合わない。Factorio 型(全部を毎刻み)が D-202 に近い。Teardown は未確認。

### 試す順番(D-411: 成功の基準と代案の順)
1. 工学: 不足を世界全体で止める・論理と物理の要約を分ける・小さいプールと大きいプールで毎刻み一致する試験(I/O 無し)。
2. 工学: 主メモリへの退避と復帰。追い出す順を「状態 + 容量 + コマンド」の決定的な関数にし、CPU リファレンスも同じ判断で番号までビット一致。繰り返しても保存量一致。
3. 工学: 要る集合を前もって決め、余白と先読み。場面ごとに「止まった刻みの割合」と転送量を測る。
4. 工学(T-0037 の後): 64m の地域ごとのファイル・DirectStorage(まず無圧縮)・作り直しの層。
5. 計測: GPU で詰める → GDeflate か Zstd。
6. 研究: 活性が VRAM を超える時に分けて計算(05 §8)。基準は「無制限の場合とビット一致」。代案の順: 地域 + 余白で分けて刻む → 刻みをやり直せる形 → WDDM に任せる。
- 次の代案へ移る条件: 1 刻みの中の伝播の距離が抑えられず余白が決まらない → 「足りなければ書く側の世代を捨てて刻み直す」/ 追い出しの判断を CPU で同じに再現するのが重い → 非決定的な置き場所 + 論理の要約で比べる(ADR-0016 が想定済み)/ 帯域が足りない → 圧縮を前倒し。

### ユーザーに聞く点 → QUESTIONS Q24

### リスクと未確認
- 主メモリとの GPU コピーの実効帯域(PCIe 4.0 で 10〜25GB/s と見込むが未確認。1 頁 56KiB なら 1ms に数百頁・未確認)。DirectStorage の小さい読み込みの遅延・WARP で動くかも未確認。
- 世界全体の見出しと索引の大きさ(TotK 程度で粗い層の見出しが数百 MB になりうる・未確認)。見出しも追い出しの対象になるかもしれない。
- 頁を戻すのは生成器(T-0037)に依存。T-0037 を先にするか、1〜3 だけ先に進めるかを決める必要がある。

### 出典
- DirectStorage 1.1: https://devblogs.microsoft.com/directx/directstorage-1-1-coming-soon/ / 1.4(Zstd): https://devblogs.microsoft.com/directx/directstorage-1-4-release-adds-support-for-zstandard/
- GDeflate: https://developer.nvidia.com/blog/accelerating-load-times-for-directx-games-and-apps-with-gdeflate-for-directstorage
- タイル割り当ての費用: https://github.com/gpuweb/gpuweb/issues/455 / https://learn.microsoft.com/en-us/windows/win32/direct3d12/volume-tiled-resources
- Nanite: https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf / GVDB: https://diglib.eg.org/handle/10.2312/hpg20161197 / SPGrid: https://pages.cs.wisc.edu/~sifakis/project_pages/SPGrid.html
- Minecraft: https://minecraft.wiki/w/C-tick / GPU Upload Heaps: https://github.com/microsoft/DirectX-Specs/blob/master/d3d/D3D12GPUUploadHeaps.md
