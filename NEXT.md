# NEXT.md — 次にやること(上から順に)

1. **T-0002 反応表 v0**(設計・PC 不要)— 元素・トークンの種類、規則性・例外・魔法の介入、最初の 3 つの発見
2. **T-0003 GPU デバッグ基盤**(工学)— GPU printf/assert のリングバッファ(Work Graphs のノードからも書ける形)→ CPU のログへ、
   決定性リプレイの骨組み、debug layer / DRED
3. **T-0004 窓とフレームループ**(工学)— 事前記録コマンドリストで CPU を薄くする形の最小ループ、スクショ出力
4. **T-0005 Hello Work Graph**(工学)— 最小の Work Graph を投げて結果を読み戻す
5. **T-0008 Work Graphs のデバッグ機能**(工学+一部研究)— ノードごとのカウンタ・ノードからの printf・上限の検出・連鎖のトレース・決定性の確認

PC に触れない日(勤務中など)は T-0002 を進める。

## 保留(やるかどうかをユーザーが決める)

- ReSTIR DI の既存コードをどこまで移植するか(場所: ユーザーに確認)
- 3D Gaussian Splatting / Neural Post-process(docs/design/01-architecture.md「保留」)
- AVBD を Work Graphs に載せるか Compute で回すか(ADR-0002)
