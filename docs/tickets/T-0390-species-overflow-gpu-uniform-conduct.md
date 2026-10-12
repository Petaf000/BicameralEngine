# T-0390 GPU の伝導の段が一様の値の溢れを読む(T-0384 から分けた)

- Status: Todo(T-0384 の一部の後)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052(追記 1・8・15)。関係: T-0211・T-0272・T-0384

## 目的
伝導ありの刻みで、溢れのある一様の値を伝導の段が落とさずに読む(今は溢れを読まずに評価・広げる)。

## やること
1. 評価(multires_conduct.hlsli の EvaluateUniformCells。512 スレッドの段): 溢れのある一様の値は 512 スレッドの段に上限の無い形を持たせない
   (T-0290)。「変わる」として頁に広げる(CPU と食い違う)のではなく、64 スレッドの段で 1 回だけ評価する形を考える(T-0384 の EvaluateUniformWaitWide)。
2. 一様の値の熱(伝導の流れ): 溢れのある一様の値の熱容量・伝導率を WideTailThermal と同じく溢れまで読む。
3. 広げる(ConductPrepare): 一様の値で埋める時に頁の面 0 へ溢れを写す(塊は TreeExpand の溢れの変種が配り済み。CopyUniformTails と同じ)。
4. 試験: T-0384 の RunUniformOverflowSteps を伝導ありでも(TestExpandWide の場面)。CPU と HW・WARP が毎刻みビット一致。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
