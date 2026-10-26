# T-1211 2 つのエネルギーを世界へつなぐ(D-665・ADR-0486)

- Status: Todo(2026-10-26 wt3 が T-1160 から分けた)
- 種類: 工学
- PC: 必須
- 設計: docs/design/20-transport-substrate.md §10.6 / 決定: D-665・D-666・ADR-0486 / 親: docs/tickets/T-1160-phase-energy.md

## 目的
T-1160 の共通の核(shaders/common/phase_layer.hlsli)を世界の木(と仮の世界)へ: Ec の欄(疎・ハッシュ・セーブ)・伝導(陽 TxConduction・陰)の面を部分ごとに・反応 2 回(凝縮相の項を持つ規則 / 気体だけの規則の印をベイクで)・気体と液体の区画が部分の h を持ち出す・細かく/粗くする(T-1176 の熱容量の重みを部分ごとに)・CPU と GPU のビット一致(PlEvenCondensedEnergy の 64bit の差と比べは GPU の試験で確かめる)・混ざったセルの頁の数と GPU の時間を測る

## 作業ログ
- 2026-10-26 wt3: T-1160 から分けて作った。
