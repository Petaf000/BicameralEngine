# T-0358 気体: 多重解像度の木の GPU の費用を測る(T-0209 から分けた)

- Status: Todo
- 種類: 工学(計測)
- PC: 必須(ランナーが idle の時に。6 つのランナーが並走している間の値は比べられない)
- 見積もり: 作業役 1 体(1 時間)
- マイルストーン: M3 (docs/plan/ROADMAP.md)
- 設計: docs/perf.md・07 §2.5・D-302 / 関係: T-0209・T-0185(1 レベルの計測)・T-0210(WG と Compute の比べ)

## 目的
gpu_gas_tree_test に `--profile`(CPU と比べず大きい木で 1 刻みの GPU 時間)を足し、同じいちばん細かい細かさの一様な箱(gpu_gas_test --profile)と比べる。
セルの段が自分の面の並び(粗いセルは最大 24 面)を集める形の費用と、面の段の Dispatch の数を docs/perf.md に残す。
