# T-0672 エディタの窓で区画のある世界を 置く → Undo → プレイ → やめる・写しの早送りで遊びの指標

- Status: Done(窓での断る表の試験は T-0687 に分けた)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ⑤ の T-0414(川が熱を吸う)・T-0415(氷が溶けて流れる)の窓での確かめ(T-0620 から分けた。T-0671 の後)
- 設計: docs/design/14(保存点・写しの早送り)/ 決定: ADR-0300(仮)・T-0497・T-0509

## 目的(1〜2 行)
区画のある世界をエディタの窓で 置く → Undo → プレイ → やめる、写しの早送りで遊びの指標を測る、ができることを窓の試験で確かめる。

## 完了条件(チェックできる形で)
- [x] window の試験 window_regions_play_record/_play: ⑤ の場面(区画のある世界)で --auto-play(置く 2 回 → プレイ → やめる → 元に戻す 2 回・
      やり直す 2 回 → プレイ 2・3 のハッシュ列の比べ)。置く物は気体の区画の真ん中(FrameLoop::MakeAutoPlayStrokes。材料は場面の草と水)。記録を別の起動で再生して同じ
- [x] 写しの早送り: window_regions_copy_record/_play(--auto-copy。刻み 60 から 600 刻みの写しのハッシュが本物と全部同じ)。
      遊びの指標(--measure-metrics。PlayMetricMeasurer の元の世界と写しに区画を渡すよう直した)は下の「結果」
- [~] 表のホットリロード: 読み直した表を当てる前に世界に聞く(ReactionTableHotReload::SetWorldCheck → ProbeSim::CheckRegionTableSwap)。
      断った表は「今の世界に当てられない表: 〇〇」で古い表のまま(エディタの反応表の行とログ。ファイルの正本も古い表のまま)。
      差し替えの刻みで RecordFrame が断って止まる道は通らなくなった。窓の試験は T-0687

## メモ・参考
- エンジン側(保存点・写し・ハッシュ・差し替えの約束)は T-0620 で済み(gpu_probe_rewind_regions)。ここは窓とアプリの道。

## 結果(2026-10-17・release・RTX 3070 Ti)
- window_regions_play_record(12 秒)・window_regions_play_play・window_regions_copy_record(13 秒)・window_regions_copy_play が通過。
  --auto-play のログ: 起動は止まっていた → 置く 2 回で刻み 0 のまま → やめるで境界へ戻った → 元に戻す・やり直すも刻みが進まない →
  B を戻したプレイは違い・やり直したプレイは初めの 50 刻みが同じ・プレイ中に置いた分が消えて 40 刻みが違う。OK
- 重さ: エディタのプレイ中 GPU 2.7〜2.9 ms/投入(docs/perf.md)。

## 並走の重なり(2026-10-17)
- frame_loop.cpp: CreateSimulation・WorldCopyFactory・PlayMetricFactory(SetLayoutRegions を足した)・ApplyAutoPlayAction(MakeAutoPlayStrokes に分けた)・
  コンストラクタ(SetWorldCheck)・LogLayoutRegions(新)。world_layout(.h/.cpp)と world_layout_package・reaction_table.cpp の BakeWorldLayout の位置
  (環境の値と基準の大気の後へ)。wt3(T-0623 水の規則)は場面の水の流れ方が変わるだけ(試験はハッシュの再生なので値は見ない)。

## 分けたもの
- T-0687: 区画のある世界で断る表と通る表のホットリロードを窓で確かめる(--packages の写しの場面の速さを直す → 当たる / 水の密度を変える → 理由が出て古い表のまま)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17(wt5): ⑤ の場面で エディタの 置く → Undo → Redo → プレイ → やめる と写しの早送りを窓の試験にした(window_regions_play・window_regions_copy)。
  置く物は気体の区画の真ん中へ。表のホットリロードは当てる前に CheckRegionTableSwap を聞く(SetWorldCheck)。指標は PlayMetricMeasurer が区画を落としていたのを直した。
