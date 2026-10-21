# T-0981 支えの法則の段 0・面の力・帳簿を GPU で組む(CPU とビット一致)

- Status: Todo(T-0970 から分けた)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列 3(支えの法則)
- 設計: docs/design/22-support-law.md §3.2・§6 / ADR-0418・ADR-0422

## 目的(1〜2 行)
T-0970 の GPU 版は段 0 の係数と右辺の項を CPU の系から写している。支えのセル(固い物・固定した面・硬さ・密度)が VRAM にある時に、
GPU が自分で段 0(面の 3 × 3 のブロック・段の斜めの棒・固定した面・重さ)を組み、解いた値から面の力(FaceForces)と帳簿(Ledger)を作る。

## 完了条件(チェックできる形で)
- [ ] 支えの段 0 を組む段(セル 1 つにつき 1 スレッド。ADR-0406 の「段 0 は法則の段」の形)・面の力と帳簿の段
- [ ] CPU の SupportLaw と毎刻み、値・面の力・帳簿をビット一致(T-0970 の 10 場面)・逆順・2 回でハッシュ一致
- [ ] 段の形が変わる時(掘った・割れた)の写し方を決める(形は CPU から写す / GPU で作る。T-0939 と同じ問い)

## メモ・参考
- 口: engine/src/sim/gpu_equilibrium_block.h(RecordUpload が今の写し。段 0 の置き場は common/equilibrium_block.hlsli の EqbDiagonalWord・EqbBlockWord・EqbFixedWord・EqbSourceWord)。
- 3 次元(k = 6)の支え(T-0960)が入ったら同じ段で k を変える。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
