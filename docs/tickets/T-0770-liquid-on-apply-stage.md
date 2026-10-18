# T-0770 多重解像度の 1 段の液体を当てる段(T-0714)に載せ替える(T-0717 から分けた)

- Status: Done(2026-10-17。面の隣を世界の Neighbor へは T-0771。GPU の液体の種類は段 2 の GPU の本で)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分(2 時間で区切る)
- マイルストーン: ③' 運ぶ計算の共通の土台 段 2(T-0717 の続き。T-0718 の前か同時)
- 設計: docs/design/20-transport-substrate.md §2.3〜§2.4 / 決定: ADR-0326・ADR-0334(Proposed)

## 包みの型(並走の wt4 T-0720〔気体〕と共有。先に決めた形。shaders/common/transport_apply.hlsli)
- **上限の倍率は分数** `TxScale { numerator, denominator }`(1 以下)。`TxLimit { TxScale outScale; TxScale inScale; }`。`TxMakeLimit(中身, 出る合計, 器の残り, 入る合計)`・
  `TxMinLimit`・`TxFaceScale(出ていく側, 入る側)` → `TxScale`。`TxScaleAmount(量, 倍率)`・`TxScaleFlow(符号つき, 倍率)` は「量 × 分子 ÷ 分母」の切り捨て 1 回
  (仮の世界の液体 LqLimitOutflow・気体 GfLimitMovedAmount と同じ式 = 載せ替えてもビット一致。前の Q32 の倍率は 2 回切り捨てて値が変わった)。
  器で切らない種類は room = `TX_ROOM_UNLIMITED`。
- **包み** `TxPacket { speciesCount; species[RX_MAX_CELL_SPECIES]; amounts[RX_MAX_CELL_SPECIES](符号なし); energy; carried[TX_PACKET_CARRIED = 4] }`。
  向きはいつも上流 → 下流。作る: `TxMakePacket`・`TxPacketWithSpecies(包み, 物質, 量)`(返り値で受ける。HLSL に inout が無いので)・`TxScalePacket(包み, 倍率)`(全部の列に同じ倍率)。
- **流し方(面に状態を持つ種類)**: 主の量(液体 = 質量 mg・気体 = 種類が決める 1 つの数)の生の流れを面ごとに作る → 世界がセルごとの `Limit` を出る合計・入る合計から先に数える(Jacobi)
  → `TxLimitAmount(world, 上流, 下流, 生の量)` → 種類が上流のセルから包みを作る(`Pack`)→ `TxApplyPacket(world, 上流, 下流, 包み)`(世界の
  `AddPacket(slot, index, packet, incoming)` を両側に)。列ごとに別の上限が要る種類(気体の成分ごとの中身)は包みを作ってから `TxScalePacket`。
- **CPU のセルへの当て方** `engine/src/sim/transport_packet.{h,cpp}`: `TxCellChange`(包みの和)・`AddPacket(change, packet, incoming)`・`ApplyCellChange(RxCell&, change)`
  (成分は物質 ID の順・0 は消す。仮の世界の ApplyDelta と同じ式)。運ぶ欄(carried)は種類が自分の欄へ当てる(液体: carried[0] = 位置のエネルギー)。

## 目的(1〜2 行)
T-0717 の `sim/multires_liquid.cpp` が仮に持つ「上限(出る側だけ。LqLimitOutflow)・当てる(成分の増減の包み ApplyDelta)・起こす」を、
T-0714 の当てる段(shaders/common/transport_apply.hlsli の TxFlowCell・TxLimit・世界の Add / Neighbor / View)へ載せ替え、液体を「種類」の 1 つにする。

## 完了条件(チェックできる形で)
- [x] 当てる段の包みを、熱だけ(MrEnergyDelta)から「成分ごとの量 + エネルギー + 運ぶ欄」へ広げる(液体と気体〔段 3〕が同じ形を使う)
- [x] 液体の種類: 坂(柱の圧力 + 重さ)・面の式(LqOpenFaceForce)・安定の条件(出る合計 ≤ 中身 = LimitsOutflow・速さの上限)と読む欄の宣言
- [x] 上限が「出る側と入る側の倍率の min」になる所で値が変わるなら、差を説明して基準(multires_liquid_test の仮の世界との比べ)を取り直す(D-487 ③)。
      入る側の器の残り = 液体の空き(溢れの圧力で押し込める分をどう数えるか)を決めてから
- [ ] (T-0771 へ)`LiquidGrid` の面の隣を世界の Neighbor(MrFindFaceNeighbor)に替える(違うレベルの面は T-0717 の範囲外のまま断る)
- [x] release のビルド 警告 0・multires_liquid・transport_apply・archmap --check

## メモ・参考
- 載せ替える所は T-0717 のチケットの「載せ替えの口」1・4・6。帳簿(口 3)は T-0769、欄が物質に付く(口 2)は T-0718。
- 仮の世界(probe_liquid)は基準として残す(D-513)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17(wt6・作業役): 上限を分数の倍率に・包み TxPacket・TxLimitAmount / TxApplyPacket・CPU の当て方 transport_packet を足し、MultiresLiquid の上限と当てるを載せ替えた。
  液体の入る側は器で切らない(仮。下の判断待ち)ので、仮の世界との毎刻みビット一致はそのまま(基準の取り直しは要らなかった)。
  面の隣を世界の Neighbor に替えるのは T-0771 に分けた(液体の面の力・移流は面の欄を + 側の隣で読むので、TxFlowCell の 6 面の回し方と合わせる設計が要る)。

## 判断待ち(仮〔ユーザー未確認〕。取り消しやすい)
1. **液体の入る側の上限(器の残り)**: 仮 = 器で切らない(room = TX_ROOM_UNLIMITED)。作り手から見た違い: 液体の種類を書く人は「入る側の安定」を
   溢れの圧力(overfillStiffness)で持つ。器で切ると、満ちたセルへ流れ込む量が器の残りで止まり、溢れの圧力で押し込む動き(ダムの決壊・高い所からの落水)が
   別の模型になる(仮の世界と値が変わる)。案 B: 器の残り = 開いた体積 − 液体の体積 + 溢れの許す分。おすすめ: 仮のまま(1 か所 TX_ROOM_UNLIMITED を替えれば B)。

## 分けたもの
- **T-0771**(工学): 液体の面の隣を世界の Neighbor(MrFindFaceNeighbor)へ・違うレベルの面(LiquidGrid → 世界)。

