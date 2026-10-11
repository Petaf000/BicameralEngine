# T-0329 GPU の粗くする和集合が溢れを読む・TestCoarsenFullWide の GPU 版(T-0311 から分けた)

- Status: Todo(T-0311 の後)
- 種類: 工学
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052(追記 6・7)。関係: T-0187・T-0237・T-0311・T-0312・T-0272

## 今(T-0311 の後)
- GPU は端数の枠の溢れを持てる(頁と同じ形の見出し・2 面・同じ塊の置き場。ADR-0052 追記 7)。CPU の EnableWideCells の世界を写す・読み戻す・
  細かくする(子の端数の枠へ写す・親を覆う)・粗くする時に返す子の端数の枠を空にする、まで。
- 粗くする要求(multires_graph.hlsl の CoarsenRequest)はインラインの核(MrCoarsenCell)のまま: 子に溢れ(頁・端数)があると溢れを落とす(保存が崩れる)。
  和集合が 8 種を超えると TreeResolve が MR_STATUS_SPECIES_FULL で断る(CPU の溢れの世界は断らない)。
- 畳む時に端数を帳簿へ返す(multires_tree.hlsl の ReturnFractionCell)もインラインだけ(溢れのある端数の枠を畳むと溢れの成分を返さない)。

## やること
1. 粗くする和集合: 子のセル・端数を溢れまで読み(上限の無い形を 8 個レジスタに持つと 1 スレッドが数 KiB になり、HW で止まる疑い〔ADR-0052 追記 5〕。
   子の溢れを置き場から直接引く読み手で MrCoarsenCellOf と同じ順に足すか、作業場に置く)、結果(RX_GPU_WIDE_SPECIES まで)を親の頁と親の端数の枠の
   書く面へ(八分の一の 64 セルが変わる。ほかのセルの溢れは写す)書いて面を入れ替える。要求 1 件の費用を測る。
2. TreeResolve は溢れの世界では MR_STATUS_SPECIES_FULL を使わない(木の管理の段の溢れの変種が要る。今は TreeWideReserve だけ)。
3. 塊: 親の頁の書く面と親の端数の枠の書く面の塊を TreeWideReserve で要求の順に配る。要る数は粗くするまで分からないので、上限
   (セルごとに min(24, 子の成分 + 端数の成分 − 8))で取って余りを返すか、数える段を先に回すかを測って選ぶ。
4. 畳む時に端数の溢れも帳簿へ返す(溢れの変種。CPU の ReturnFractionsToLedger)。T-0312(畳む)と合わせてもよい。
5. 試験: gpu_multires_test に TestCoarsenFullWide の場面をそのまま(GPU で粗くしてから伝導ありで刻む)。今の RunCoarsenFullWide(CPU で粗くして写す)は残す。

## 完了条件
- TestCoarsenFullWide の場面が CPU と HW・WARP で毎刻みビット一致(断った 0・親のセル 16 種)・保存がビット一致

## 見積もり
1 チャット(2 時間。和集合の読み手と塊の配り方で重い。溢れれば 4 を T-0312 へ)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
