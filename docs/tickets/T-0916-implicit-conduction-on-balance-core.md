# T-0916 C-0 の 3: 熱の陰解法(広がる家族の陰)を釣り合う家族の部品の上へ

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列 1(C-0。T-0853 から分けた)
- 設計: docs/design/20-transport-substrate.md §8.1(家族は型・解く部品は道具)・§13.1 完了条件 1 / ADR-0397

## 目的
熱の陰解法(implicit_conduction の SolveImplicitFlows・multires_implicit_conduction)を部品の 2 つ目の使い手にし、部品の外には式の作り(温度・熱容量・面の係数・安全網)だけを残す。

## 完了条件
- [ ] SolveImplicitFlows の Multigrid の道が SolveEquilibrium(rounding = 重み・固定した面 = 開いた世界の端)を呼ぶ。RedBlack・Rkl2 は比べる試作として残すか消すかを決めて書く
- [ ] multires_implicit_test・multires_implicit_tree_test の CPU と、gpu_multires_implicit_* の CPU 側が前と同じハッシュ
- [ ] 誤差の見込みで早く止める口(toleranceMillikelvin)の扱い: 部品は早く止める口を持たない(20 §9.2)。熱の道で使っている所を決まった回数へ移すか、熱だけ残すかを決めて ADR に
- [ ] GPU の熱の陰解法(GpuImplicit・GpuMultiresImplicit)が部品の GPU 版(T-0915)と同じ関数を呼ぶ形に

## 作業ログ
