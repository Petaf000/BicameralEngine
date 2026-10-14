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
- 残したもの(パッチと関係なく使う): 物質の付け替え(species_remap。物質を足す・消す表の差し替え。ADR-0065)・置くコマンドの付け替え
  (RemapPlaceCommand・RemapLabCommand。パッチの分だけ外した)・表の約束の RateRule(速さと優先度に使う規則。今の表はどれも Rule と同じ。
  核が規則を読む所を分けておくと HW の Work Graph が小さく保てる〔T-0397〕ので残した)・RxRule::activationEnergy(表を読む道具が見る。
  構造体の大きさも変わらない)・多重解像度のルート署名の u5(外のバッファの枠。覗き窓・実験室のコマンドが使う)。
- 番号: 実験室のコマンド 0x4C06・仮の世界のコマンド 5・イベント 5 は「使わない」と書いて残した(古い記録の番号と重ねない)。
  仮の世界のハッシュの表の欄 [14] は 0 のまま。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-15(作業役・wt4): シェーダー・C++・試験・エディタ・設計書からパッチを全部外した(57 ファイル・−4306 行。e659463 の上の 1 コミット目)。
  release のビルドが警告 0・archmap OK・CPU の試験 28 本(lab_box・reaction_species_remap ほか)が通った。外す前の main(A)と外した版(B)の
  bin を写して measure-wide と活性のグラフを A・B・A・B で測った(docs/perf.md 2026-10-15 の 2 行)。試験の表の転換用の仮の物質の表
  (MakeConversion*TestTable)も使わなくなったので外した。

## 結果
- **DXIL の行数(A → B)**: step_wait 13903 → 10103・step_wide_wait 38152 → 29425・activity_graph 35477 → 25563・conduct_apply 7433 → 5622
  (各 −24〜28%。T-0396 の「転換の道だけを外した版」より小さい。倍率・止める・触媒・範囲の判定も外れたので)。
- **普段の世界の反応の段**: 反応の多いセルを全部刻む段(伝導なし)で A 1151〜1187 → B 1135 ns/セル(約 −1〜4%)、空気の伝導ありで
  5.13〜5.44 → 5.01〜5.04(約 −2〜8%)、ほかの場面と活性のグラフは揺れの中。**T-0396 の「B で約 −18%」ほどは戻らなかった**
  (T-0396 の B の 908〜1035 は負荷の重い時の 2 回で、今回の A も T-0396 の A〔1190〜1220〕より速い。シェーダーは 1/4 小さくなったが、
  この場面の時間はレジスタや命令の数で決まっていない見込み〔未確認〕)。
- **結果は変わらない**: 活性のグラフの要約 22931f122b05d6d1 が A・B で同じ。パッチの列が空の時の状態の要約は前と同じ値だった
  (多重解像度の HashWholeNest は列が空なら足さない・仮の世界は MpListHash が 0)ので、試験のハッシュは変わらない見込み
  (RxSpecies が 32 → 24 バイトになったが、ハッシュは表のバイトを読まない。GPU の試験の結果で確かめる)。

## 司令塔へ(マージ後に本体で直すもの)
- ROADMAP の T-0496 の行を Done に。T-0473(転換の無い変種のシェーダー)は D-493 で作らないことになっている(行があれば消す)。
- CLAUDE.md §4 と docs/claude/COMMANDER.md・runner/worker-common.md の「magic_patch.hlsli など」「魔法のパッチはエディタの実験の道具だけ」の行
  (magic_patch.hlsli はもう無い。共通のコードの例は reaction.hlsli・multires.hlsli に)。
