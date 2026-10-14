# T-0496 パッチ(触媒・阻害・転換・増幅)を全部外す

- Status: Doing
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 今の順 ⑦ の前の片付け (docs/plan/ROADMAP.md)
- 設計: docs/design/02-reaction-system.md §5.0 / 決定: D-493(D-466 を改訂)・ADR-0095(Superseded)

## 目的(1〜2 行)
前の方針(魔法 = 反応表へのパッチ)の名残りを全部外し、使わない普段の世界の反応の段が転換の道の費用(T-0396 で最大約 2 割)を払わないようにする。

## 完了条件(チェックできる形で)
- [x] シェーダー: magic_patch.hlsli・probe_patch.hlsli・multires_patches.hlsli を消す。反応の核に渡す表は重ねない表(GpuReactionTable / ProbeReactionTable)
- [x] 転換の道: RxSpecies の組成の型(elementClass・elementUnits)と BakeElementClasses・付け替えの MpRemapPatchList / RemapPatchEffect /
      RemapPatchList・SpeciesRemapReport::conversionsDisabled・Remap の SpeciesCount
- [x] パッチの列のバッファ(多重解像度の m_patches・仮の世界の m_magicPatches)と、ルート署名の枠(仮の世界の u14・ルート定数 [3] の規則の数)。
      多重解像度の u5 は外のバッファの枠として残す(覗き窓などが使う。刻みの間は外のバッファか代わりのセル)
- [x] コマンド(LAB_COMMAND_TYPE_PATCH・PROBE_COMMAND_TYPE_PATCH・PROBE_EVENT_PATCH_APPLIED・ハッシュの表の欄 [14])。番号は使わないまま残す
- [x] エディタのパッチのパネル(editor/patch_panel)・--auto-patch・frame_loop のパッチの行
- [x] 試験: gpu_probe_patch_test・window_patch_*・gpu_lab_box_test の RunPatch / RunConvert / RunConvertRemap・lab_box_test の魔法の試験・
      gpu_multires_test の --measure-convert
- [x] 設計書(02 §5.0・14 §2・00・01・13・15・19)と map.yaml
- [ ] release のビルドが警告 0・archmap --check・関係する試験(lab_box・reaction_species_remap・gpu_lab_box・gpu_probe_sim・gpu_multires の組)
- [ ] 普段の世界の反応の段が速く戻ること(--measure-wide の形。T-0396 の A / B と同じ測り方)。前後の値を docs/perf.md に

## メモ・参考
- 外したもの・残したものの見取り図は「作業ログ」と「結果」に。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
