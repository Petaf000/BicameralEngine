# NEXT.md — 次にやること(上から順に)

チケットは docs/plan/ROADMAP.md から取る。思いつきは docs/plan/BACKLOG.md へ(CLAUDE.md 原則 8)。

1. **M1 原理の確認**(D-412)を ROADMAP の順に: ~~T-0010 整数の数学ライブラリ~~ → ~~T-0011 シェーダーのビルドと float の検査~~(完了)→ **T-0013 能力の確認**(int64・64bit atomic・compute キューの DispatchGraph・WARP。fixed の自己テスト bin/shaders/sim/fixed_selftest.cso を GPU で走らせ、debug と release の両方で CPU の要約と比べる)→
   T-0003 GPU デバッグ → T-0004 窓とフレームループ → T-0012 刻みのループ → **T-0005 クリックから Work Graphs で自動伝播** → T-0015 デバッグ表示 →
   T-0008 Work Graphs のデバッグ → T-0014 原理: 化学 → T-0016 原理: 物理(角ばった箱)→ T-0017 原理: 多重解像度

PC に触れない日(勤務中など)は、中身の道(ROADMAP の C1〜C8。まず C1 = T-0002 反応表 v0、C8 魔素と魔法具)を設計チャットで進める。
