# T-1209 multires_sparse_test の要約の束 6 件の落ち(帳簿の理由を足して区画が伸びた)

- Status: Done
- 種類: 工学(不具合の切り分け)
- PC: 必須(CPU の試験 multires_sparse_test)
- マイルストーン: 試作の列(T-1177 の確かめで見つけた)
- 設計: docs/design/20 §10.2 / 関係: T-0931・ADR-0417・T-1077・T-1177

## 目的
`multires_sparse_test` の「前とビット一致(T-0931)」の要約の束が 6 場面とも前の値と合わなくなった(T-1177 の作業役が見つけた。main の 14:51 のビルドでも同じ値)。
どのコミットで落ち始めたかを絞り、根で直す。基準の値の取り直しは、変化が意図どおりと説明できる時だけ。

## 切り分け(release・試験の実行ファイルだけを各コミットで作って回した)
| main のコミット(first-parent) | 結果 |
|---|---|
| 513ce720(T-1066 伝導率) | 合格(前の値と一致) |
| cae39b42(T-1157 置かれ方で配る) | 合格 |
| 7d87b630(wt5 物の熱 T-1077 の GPU と多重解像度) | **6 件落ち**(今と同じ値) |
| ecaa022a(T-1159)・82453cf5(今の main) | 6 件落ち(同じ値) |
- 間の 4952ab95(time_step_tree)・e7cd5a94(支え)は多重解像度の世界の状態に触れない。7d87b630 は `shaders/common/world_ledger.hlsli` に
  帳簿の理由 `WL_REASON_OBJECT_EXCHANGE`(物との受け渡し)を足し、`WL_REASON_COUNT` が 11 → 12 になった。帳簿の区画(`nest.ledger`)は
  欄(理由 × 向き)が外側の並び(`WlAddress`)なので、22 欄の後ろに 2 欄(全部 0)が足され、`HashWholeNest` の「ledger」の区画の要約が変わる。
- **確かめ**: 今の main で、帳簿を前の 22 欄に切り詰めた写しの要約で束を取ると、6 場面とも T-0931 の前の値とビット単位で一致した
  (一時の計器。コミットしていない)。= 状態の値は変わっておらず、区画の長さだけが変わった。

## 直し
- 基準の値を取り直した(`GOLDEN_DIGESTS`。上の説明と前の値をコメントに残した)。変化は意図どおり(理由を 1 つ足した)で、値の変化ではない。
- 同じ落ちを繰り返さないよう、`world_ledger.hlsli` の理由の見出しに「数を変えたら multires_sparse_test の要約の束を取り直す」を足した。
- 要約の作り方(`HashWholeNest`)は変えない(GPU の保存点・写しと同じ区画の一覧を回るので、区画の中身の読み方を変えると GPU 側との約束が変わる)。

## 確かめ
- release のビルド警告 0・archmap OK。CPU の試験 18 本を実行ファイルで直に回して全部合格(2026-10-26): multires_sparse(67 秒)・multires_state・field_storage・placement_refine・
  multires・multires_wide・multires_uniform・multires_quiet・multires_activity・multires_edge・multires_grading・multires_graded・multires_gas・multires_liquid・
  multires_implicit・multires_conduction・multires_subcycle・multires_implicit_tree。

## 司令塔へ
- この試験はマージの確かめの組に入っていなかったので 1 日気づかなかった(main の全部の CPU の試験は 10-24 の T-1066 の時が最後)。
  帳簿・区画の一覧(`VisitNestState`)・`MrBlock` の形に触るマージでは multires_sparse を流す。

## 作業ログ
- 2026-10-26(wt3・t-1209-sparse-fix): 疑わしいマージを first-parent で 4 か所ビルドして 7d87b630 に絞り、帳簿の区画が伸びたことを切り詰めた要約で確かめて基準を取り直した。
