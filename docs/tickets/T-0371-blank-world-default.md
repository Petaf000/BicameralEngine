# T-0371 サンプルの世界をパッケージへ分け、ブランクから起動するのを既定にする

- Status: Done(2026-10-12 その 3 のマージの後、release で window_* と image_* が全部通った。残りの中身は T-0398〜T-0400)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(この範囲。残りは T-0398〜T-0400)
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: docs/design/13-scripting-game-model.md §2.6・14 §2 / 決定: D-454〜D-456・ADR-0151

## 目的(1〜2 行)
何のパッケージも足さずに起動した世界を本当に空(表が空・セルが空)にし、それを既定にする。今の「空気の中の木箱 + 積み木 + 試験の表」は
「サンプルの世界」として名前を付けて選ぶ形にし、試験はどちらの世界を使うかを明示する。

## 完了条件(チェックできる形で)
- [x] 起動の既定がブランク(`--world blank`。表は空〔物質 0・規則 0〕・セルは全部空・積み木なし)。今の世界は `--world sample`
  (2026-10-12 その 2: frame_loop.h の既定を Blank に・main.cpp の説明を直した)
- [x] ブランクの表のベイク・版・中身からの作り直し(再生)が壊れない(CPU の試験 blank_world。通った)
- [x] ブランクの世界は何刻み進めても何も変わらない(CPU リファレンス。空の表・試験の表とも 4 刻み、ハッシュとエネルギーが刻み 0 のまま)
- [x] ブランクの世界の GPU の刻みが CPU とハッシュ一致(gpu_probe_blank。止まりは GBV の計装が遅いだけだった: --no-gbv の HW 27 秒・
  WARP 35 秒で通る。ctest の HW も gpu_probe_sim と同じく --no-gbv にした)
- [x] 窓: ブランクで起動・エディタ付きで起動・記録 → 再生でハッシュ列一致(window_blank_record/play。debug で通過。実験室の空の箱は window_lab_blank)
- [x] 今の窓の試験はサンプルの世界を明示(`--world sample`)して読み、結果(ハッシュ・画像)が変わらない(2026-10-12 その 3: release で window_* と image_* が全部通過)
- [x] サンプルの世界の中身のうち 1〜2 個をパッケージへ → 時間切れで T-0398〜T-0400 へ分けた

## C++・HLSL に埋まっているサンプルの世界の中身(2026-10-12 の洗い出し)
| 何 | どこ | 行き先 |
|---|---|---|
| 初めの世界(空気の中の木箱。8³・壁 1 セル・セルロース 1 割)と空気の成分(O2 1063800 µmol・N2 4014200 µmol)・300 K | sim/probe_sim.cpp `MakeProbeInitialWorld` | T-0398(世界の組み立てをパッケージに) |
| 抽出に写す物質(O2・CO2・炭)と、描画の O2 の基準量 | sim/probe_sim.cpp `ProbeViewSpecies`・shaders/render/probe_view.hlsl `AIR_OXYGEN_MICROMOLES` | T-0399 |
| 積み木の場面 | sim/physics_scene.cpp `MakeProbeStackScene` | T-0398 |
| ゲーム本体のパッケージの名前 `combustion_test` | script/reaction_table_loader.h `DEFAULT_BASE_PACKAGE` | この チケット(サンプルの世界の表として) |
| 実験室の材料のひな形(空気・木・木炭・二酸化炭素・窒素・魔素〔試験〕)と量・実験室の空気で満たす・300 K | sim/lab_box.cpp `MakeLabMaterials`・`MakeLabBoxNest`・lab_box.h `LAB_INITIAL_TEMPERATURE_MILLIKELVIN` | T-0399(材料をパッケージに) |
| 筆・魔法のパッチのパネルの既定(「木」・cellulose・木箱の壁の位置 28,32,32)・実験室のパネルの自動の確かめの材料・計器のセルロース | editor/brush_panel.cpp・patch_panel.cpp/.h・lab_panel.cpp・gauge_panel.cpp | T-0399 |
| 自動の確かめの書き換える所(combustion_test/reactions.luau の木の燃焼・species.luau のオゾン) | frame/auto_reload.cpp | サンプルの世界の試験の道具のまま(動かさない) |
| 気体の音速の基準 340 m/s・地面の層の基準の温度 | sim/gas_reference.h `GasOptions::soundSpeedMmPerS` 等 | T-0400(環境の値。T-0372 と合わせる) |
| 気体定数 R・重力 | gas_internal.h・reaction_table.cpp・lab_box.cpp・magic_patch.hlsli・physics_step.hlsli | T-0372(触らない) |

## 判断待ち

## 分けたもの
- T-0398: サンプルの世界の組み立て(木箱・空気・温度・積み木)を Luau のパッケージで書く(世界の組み立ての分類・型の定義・読む所)
- T-0399: 実験室・筆・パネルの材料のひな形と抽出の物質をパッケージから読む(名前をソースから消す)
- T-0400: 気体の環境の値(音速の基準・基準の温度)をパッケージの環境の値へ(T-0372 と一緒に)

## メモ・参考
- 再生ファイルの「世界の種類」(save::ReplayWorld)に Blank を足す。形式の版は変えない(ヘッダの world の欄は既にある)。

## 続き(次の作業役へ)
- **gpu_probe_blank の止まり(解決)**: GBV の計装が遅いだけだった。`run -Exe gpu_probe_blank_test -- --no-gbv` は HW 27 秒・`--warp` 35 秒で
  空の表・試験の表とも CPU とハッシュ一致・刻み 0 のまま。ctest の HW は gpu_probe_sim と同じく `--no-gbv` に(debug 39 秒)。T-0397 とは別の件。
- **窓の GBV**: 前の巡目で GBV を切る条件を「積み木がある(HasStackScene)」にしていたので、ブランクの窓は GBV ありで最初のフレームが
  330 秒かかり、window_blank_play が TIMEOUT 240 で落ちた。条件を元の `options.physics` に戻した(サンプルの窓は前から GBV なし)。
- 既定を Blank に。window_blank_record は `--world` を外して既定を確かめる。ブランクの実験室: `--auto-lab` は材料が 1 つも無い表なら
  空の箱を 60 刻み流して GPU と CPU の一致・隅のセルが空のまま・記録から別の箱で再生しても同じ、を確かめる(window_lab_blank を足した)。
- 残り: --world sample を付けた今の窓の試験(window_*)・画像の試験(image_*)の流し直し(release でよい。サンプルの窓は前から GBV なしで、
  --world sample を明示しただけなので変わらない見込み)。通ったら完了条件の残り 2 つに印を付けて Done。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-12(wt2): `--world blank|sample`・空の表(LoadBlankReactionTable)・空の世界(MakeBlankProbeWorld)・再生ファイルの世界の種類(Blank = 2)・
  空の表の GPU のバッファ(0 の要素 1 つ)・実験室の空のセル。試験 blank_world(CPU。通った)・gpu_probe_blank(返らない)・window_blank を足し、
  今の窓の試験に --world sample を明示。GPU の止まりのため既定は仮で sample。ADR-0151 追記 1・13 §2.6。残りは T-0398〜T-0400。
- 2026-10-12 その 2(wt2): gpu_probe_blank の止まりは GBV の遅さ(--no-gbv で HW 27 秒・WARP 35 秒で通る。ctest の HW を --no-gbv に)。
  既定の世界を Blank に・main.cpp の説明を直した。窓の GBV を切る条件を `options.physics` に戻した(ブランクの窓は最初のフレームが 330 秒だった)。
  `--auto-lab` に材料の無い表の空の箱の確かめ(RunAutoEmptyBox)と window_lab_blank を足した。debug: blank_world・gpu_probe_blank(_warp)・
  window_blank_record・window_lab_blank は通過(GBV あり)、window_blank_play は GBV ありで TIMEOUT 240 → 条件を戻して流し直し、window_blank_record 60 秒・window_blank_play 59 秒・
  window_lab_blank 78 秒で通過。**--world sample の窓の試験・画像の試験は時間切れで未実行**(GPU の順番待ちで 31 分待った)。
- 2026-10-12 その 3(本体・記録だけ): マージの後の release で window_*・image_*(窓と画像 28 本)が全部通ったので Done にした。
