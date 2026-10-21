# --- core: どのサブシステムからも使う小さな部品(シングルトンの破棄順・ログ・HRESULT・文字コード)。他のサブシステムを知らない ---
add_library(bicameral_core STATIC
  src/core/hresult.cpp
  src/core/log.cpp
  src/core/log_sinks.cpp
  src/core/paths.cpp
  src/core/singleton.cpp
  src/core/unicode.cpp
)
target_include_directories(bicameral_core PUBLIC src)

# --- fixed: シミュの整数の数学ライブラリ(shaders/common/*.hlsli を C++ からも読む。04 §3)---
# CPU のリファレンス・ベイク・テストがこれを使い、GPU のシェーダーと同じソースで計算する(D-307)
add_library(bicameral_fixed INTERFACE)
target_include_directories(bicameral_fixed INTERFACE ${PROJECT_SOURCE_DIR}/shaders)

# --- gpu: D3D12 のデバイス・キュー・バッファ・Work Graph の小さな道具(T-0013)。core だけを知る ---
# ランタイムと GPU のテスト(tests/gpu_*_test.cpp)が使う。
add_library(bicameral_gpu STATIC
  src/gpu/debug_ring.cpp
  src/gpu/device.cpp
  src/gpu/graph_trace.cpp
  src/gpu/immediate_queue.cpp
  src/gpu/pipeline_cache.cpp
  src/gpu/queue.cpp
  src/gpu/readback_ring.cpp
  src/gpu/resources.cpp
  src/gpu/swap_chain.cpp
  src/gpu/work_graph.cpp
  src/gpu/work_graph_stats.cpp
)
target_include_directories(bicameral_gpu PUBLIC src)
target_precompile_headers(bicameral_gpu PRIVATE src/pch.h)
target_link_libraries(bicameral_gpu PUBLIC
  bicameral_core
  bicameral_fixed  # shaders/common/debug_ring.hlsli・work_graph_stats.hlsli・graph_trace.hlsli(デバッグのリング・カウンタ・トレースのレイアウト)
  Microsoft::DirectX-Headers Microsoft::DirectX-Guids
  d3d12 dxgi
  version)  # 読み込んだ WARP の版を調べる(GetFileVersionInfoW。gpu/device.cpp)
# debug プリセット: デバイスを作るとき debug layer・GPU-based validation・DRED を既定で有効にする(gpu/device.cpp、T-0003)
if(BICAMERAL_GPU_VALIDATION)
  target_compile_definitions(bicameral_gpu PRIVATE BICAMERAL_GPU_VALIDATION=1)
endif()

# --- reaction: 反応表のベイクと 1 セルの評価の CPU リファレンス(02。T-0014)。GPU を知らない(CPU だけのテストがある)---
# 評価の本体は shaders/common/reaction.hlsli(HLSL と共通)。浮動小数点は禁止(engine/src/sim は検査の対象)。
add_library(bicameral_reaction STATIC
  src/sim/reaction_table.cpp
  src/sim/reaction_slope.cpp  # 坂を上る規則のベイクの警告(物質の標準エントロピーから ΔG°。T-0492・D-487)
  src/sim/species_remap.cpp  # 物質を足す・消す表の差し替えの付け替え(T-0223・ADR-0065)
  src/sim/world_layout.cpp  # 世界の組み立て(初めのセル・物。パッケージの world_cells・world_bodies。T-0398)
  src/sim/material_templates.cpp  # 材料のひな形・抽出の物質・既定(同じ読み込みの分類。T-0399)
  src/sim/world_atmosphere.cpp  # 基準の大気(分類 atmosphere と environment の大気の値。T-0373・ADR-0182)
  src/sim/play_metric.cpp  # 遊びの指標の形・前の版との差(分類 play_metrics。T-0432・ADR-0206)
  src/sim/coupling_table.cpp  # 流れどうしの結合(分類 drags・machines)のベイクと |q| ≤ 1 の検査(T-0536・ADR-0238)
  src/sim/radiation_reference.cpp  # 放射の熱の交換の CPU リファレンス(対の流れ。T-0583・ADR-0252)
  src/sim/radiation_lines.cpp  # 放射の対を平行な線の束で見つける(T-0584・ADR-0262)
)
target_include_directories(bicameral_reaction PUBLIC src)
target_link_libraries(bicameral_reaction PUBLIC bicameral_fixed)

# --- multires: 多重解像度の入れ子の細分の CPU リファレンス(17。T-0017)。GPU を知らない。浮動小数点は禁止 ---
# 細かくする・粗くする・影の引き戻しの本体は shaders/common/multires.hlsli(HLSL と共通)。
add_library(bicameral_multires STATIC
  src/sim/cell_view.cpp  # セルの見立ての相の表(MakeCellView を CPU で呼ぶ。T-0713。MR の伝導の当てる段が読む。T-0714)
  src/sim/implicit_conduction.cpp
  src/sim/equilibrium.cpp  # 釣り合う家族の共通部品(多重格子の段・決まった回数・面の形・閉じた領域。T-0853)
  src/sim/lab_box.cpp
  src/sim/lab_gauge.cpp
  src/sim/multires_activity.cpp
  src/sim/multires_conduction.cpp
  src/sim/transport_packet.cpp  # 当てる段の包みをセルに当てる CPU(液体・気体。T-0770)
  src/sim/multires_fields.cpp
  src/sim/multires_implicit_conduction.cpp
  src/sim/multires_nest.cpp
  src/sim/multires_state.cpp
  src/sim/multires_tree.cpp
  src/sim/multires_wide_cell.cpp
)
target_include_directories(bicameral_multires PUBLIC src)
target_link_libraries(bicameral_multires PUBLIC bicameral_reaction)

# --- gas: 気体の流れの CPU リファレンス(1 レベルの箱 G1・G2〔07 §2.1・T-0026・ADR-0043〕と多重解像度の木〔07 §2.5・T-0208・ADR-0117〕)。GPU を知らない。浮動小数点は禁止 ---
add_library(bicameral_gas STATIC
  src/sim/gas_reference.cpp
  src/sim/gas_tree.cpp
)
target_include_directories(bicameral_gas PUBLIC src)
target_link_libraries(bicameral_gas PUBLIC bicameral_fixed)

# --- gas_projection: 気体の圧力を刻みの中で解く投影の CPU 模型(T-0503 研究・07 §2.6)。熱の陰解法の多重格子(multires)を流用する。浮動小数点は禁止 ---
add_library(bicameral_gas_projection STATIC
  src/sim/gas_projection.cpp
  src/sim/gas_fastest_check.cpp
  src/sim/gas_fastest_layout.cpp  # 最速 × 小さい空きを仮の世界の区画・置いた世界で検査(読み込みから呼ぶ。T-0804)
  src/sim/cell_volume.cpp  # 1 セルの体積を相で分け合う(固体・液体・気体の空き。T-0593・T-0612。読み込みの検査も使うのでここ)
)
target_include_directories(bicameral_gas_projection PUBLIC src)
target_link_libraries(bicameral_gas_projection PUBLIC bicameral_gas bicameral_multires)

# --- case_b: 案 B(多流体の運動量)を CPU で確かめる試作の箱・場面・計器(docs/design/21 §5・T-0866・T-0867)。GPU を知らない。浮動小数点は禁止 ---
# 判定(21 §5.1)で案 A に戻る時は、このライブラリと src/sim/case_b ごと捨てる。陰の圧力は C-0 ができるまで熱の陰解法の多重格子(multires)を呼ぶ
add_library(bicameral_case_b STATIC
  src/sim/case_b/case_b_box.cpp
  src/sim/case_b/case_b_gauge.cpp
  src/sim/case_b/case_b_pressure.cpp
  src/sim/case_b/case_b_scene.cpp
)
target_include_directories(bicameral_case_b PUBLIC src)
target_link_libraries(bicameral_case_b PUBLIC bicameral_multires)

# --- physics: 物理の場面の定義と整数のソルバーの CPU 側(08。T-0016)。GPU を知らない。浮動小数点は禁止 ---
add_library(bicameral_physics STATIC
  src/sim/physics_scene.cpp
  src/sim/physics_world.cpp
)
target_include_directories(bicameral_physics PUBLIC src)
target_link_libraries(bicameral_physics PUBLIC bicameral_fixed)

# --- sim: シミュの CPU 側(GPU で走らせる道具と CPU リファレンス)。浮動小数点は禁止(shaders/CMakeLists.txt が検査)---
# 今は T-0004 の仮の刻みを T-0012 の単位の形にしたもの(中身は T-0005 以降で本物に)。
add_library(bicameral_sim STATIC
  src/sim/gpu_gas.cpp
  src/sim/gpu_gas_tree.cpp  # 気体の多重解像度の木の GPU(T-0209)
  src/sim/gpu_gas_projection.cpp  # 気体の圧力の投影の 1 刻みの GPU(T-0845。計測と比べる口)
  src/sim/gpu_lab_box.cpp
  src/sim/gpu_multires.cpp
  src/sim/gpu_implicit.cpp
  src/sim/gpu_implicit_build.cpp
  src/sim/gpu_implicit_levels.cpp
  src/sim/gpu_multires_implicit.cpp
  src/sim/gpu_physics.cpp
  src/sim/lab_comparison.cpp
  src/sim/lab_session.cpp
  src/sim/probe_peek.cpp
  src/sim/gas_cells.cpp  # 世界のセルと気体の箱の間の写し(仮の世界と多重解像度の世界が共有。T-0720)
  src/sim/probe_gas.cpp  # 仮の世界に気体の流れを載せる CPU リファレンス(T-0590)
  src/sim/probe_liquid.cpp  # 仮の世界に液体の流れを載せる CPU リファレンス(T-0609)
  src/sim/multires_liquid.cpp  # 多重解像度の世界の 1 段で液体を流す CPU(土台の段 2 ②。T-0717)
  src/sim/multires_gas.cpp  # 多重解像度の世界の 1 段で気体を流す CPU(土台の段 3 ②。T-0720)
  src/sim/multires_world_edges.cpp  # 世界の端を環境から作る・替える(外の柱は気体の層の基準 GasAtmosphereLayers と同じ。T-0772)
  src/sim/probe_surface.cpp  # 筆の面に沿って置く物の印(T-0332)
  src/sim/probe_save_point.cpp
  src/sim/probe_sim.cpp
  src/sim/probe_sim_gas.cpp  # 仮の世界の GPU の刻みに気体の区画(T-0592)
  src/sim/probe_sim_liquid.cpp  # 仮の世界の GPU の刻みに液体の区画(T-0613)
  src/sim/slope_gauge.cpp  # 開発ビルドの坂の計器(反応が坂を上った量を刻みごとに数える。T-0518)
  src/sim/probe_trace.cpp
  src/sim/tick_cost.cpp  # 単位の GPU 時間を刻みごとに(重さの検査。T-0413)
  src/sim/world_copy.cpp  # 世界の写しの早送り(T-0412)
  src/sim/play_metric_measure.cpp  # 遊びの指標を写しの早送りで測る(T-0432)
)
target_include_directories(bicameral_sim PUBLIC src)
target_precompile_headers(bicameral_sim PRIVATE src/pch.h)
target_link_libraries(bicameral_sim PUBLIC bicameral_gpu bicameral_reaction bicameral_physics bicameral_multires bicameral_gas bicameral_gas_projection)

# --- save: セーブ・再生ファイルの読み書き(15。T-0086)。CPU だけ(GPU を知らない。CPU だけのテストがある)---
# コマンドの形は sim/command.h(ヘッダだけ)を使う。
add_library(bicameral_save STATIC
  src/save/replay_file.cpp
  src/save/replay_session.cpp
)
target_include_directories(bicameral_save PUBLIC src)
target_link_libraries(bicameral_save PUBLIC bicameral_core)

# --- script: Luau の殻(サンドボックス・上限・決定性。13 §2・T-0020・ADR-0030)とパッケージ(T-0138・ADR-0031)・型検査(T-0140・ADR-0046)。CPU だけ(GPU もシミュの状態も知らない)---
# エディタ・ツール・ゲームのオーサリングが使う。Luau の C API(lua.h)はホスト関数を書く側も使うので PUBLIC で渡す。
add_library(bicameral_script STATIC
  src/script/luau_sandbox.cpp
  src/script/luau_package.cpp
  src/script/luau_type_check.cpp
  src/script/luau_literal_edit.cpp
  src/script/package_fields.cpp  # パッケージの欄を読む道具(反応表と世界のデータの分類が共有。T-0398)
  src/script/script_value.cpp
)
target_include_directories(bicameral_script PUBLIC src)
target_link_libraries(bicameral_script PUBLIC bicameral_core unofficial::luau::Luau.VM unofficial::luau::Luau.Compiler)
# 型検査(T-0140・ADR-0046)。Luau.Analysis を知るのは luau_type_check.cpp だけなので PRIVATE
target_link_libraries(bicameral_script PRIVATE unofficial::luau::Luau.Analysis)

# --- reaction_script: Luau のパッケージで書いた反応表を、ベイクに渡す定義に読む(T-0021・ADR-0032)。CPU だけ ---
# script(パッケージ)と reaction(ベイク)をつなぐ。script は sim を知らないままにするため、別のライブラリにする。
# reaction_table_loader はランタイムとテストがデータのフォルダのパッケージから表を作る入り口(T-0157・ADR-0033)。
# reaction_hot_reload はエディタがファイルの変更を見て表を読み直す(T-0139・ADR-0047)。
# reaction_table_edit はエディタの反応表のパネルの一覧・保存則の検査・Luau への書き戻し(T-0219)。
add_library(bicameral_reaction_script STATIC
  src/script/reaction_package.cpp
  src/script/reaction_table_loader.cpp
  src/script/reaction_hot_reload.cpp
  src/script/reaction_table_edit.cpp
  src/script/world_layout_package.cpp  # 世界の組み立ての分類を読む(T-0398)
  src/script/material_templates_package.cpp  # 分類 material_templates・view_quantities・editor_defaults(T-0399)
  src/script/environment_package.cpp  # 世界の環境の値(分類 environment → sim::WorldPhysics。T-0400・ADR-0176)
  src/script/play_metrics_package.cpp  # 遊びの指標の分類 play_metrics を読む(T-0432・ADR-0206)
  src/script/coupling_package.cpp  # 分類 drags・machines を 1 つの読み手で読む(T-0536・ADR-0238)
  src/script/standard_packages.cpp  # 標準パッケージの棚と、環境ミキサーで足す一覧(T-0373・ADR-0183)
)
target_include_directories(bicameral_reaction_script PUBLIC src)
# 読み込みの検査(最速 × 小さい空き。T-0804)が gas_projection の検査を呼ぶ
target_link_libraries(bicameral_reaction_script PUBLIC bicameral_script bicameral_reaction bicameral_gas_projection)

# --- frame: フレームの歩調の計算(GPU を知らない。CPU だけのテストがある)---
add_library(bicameral_frame STATIC
  src/frame/sim_scheduler.cpp
  src/frame/weight_check.cpp  # 重さの検査の判定(T-0413)
)
target_include_directories(bicameral_frame PUBLIC src)

# --- view: デバッグ表示の操作とカメラ(D3D12 を知らない。CPU だけのテストがある。T-0015)---
add_library(bicameral_view STATIC
  src/render/debug_camera.cpp
  src/render/debug_view_controller.cpp
)
target_include_directories(bicameral_view PUBLIC src)
target_link_libraries(bicameral_view PUBLIC bicameral_core)

# --- editor: エディタの時間の操作・筆の運び(GPU を知らない。CPU だけのテストがある。T-0023・T-0245)---
# ImGui のパネル(src/editor/editor_overlay.cpp)は D3D12 を使うのでランタイムの方に入れる
add_library(bicameral_editor STATIC
  src/editor/brush_preview.cpp  # カーソルの下の筆の形の線(T-0279。common/probe_place.hlsli の形の読み方を使う)
  src/editor/brush_stroke.cpp
  src/editor/brush_undo.cpp
  src/editor/edit_table.cpp  # 編集の境界に当てる反応表の印(T-0548)
  src/editor/surface_pick.cpp  # 光線が最初に当たる物のセルの手前(T-0332)
  src/editor/time_control.cpp
)
target_include_directories(bicameral_editor PUBLIC src)
target_link_libraries(bicameral_editor PUBLIC bicameral_frame bicameral_view bicameral_fixed)

# ランタイム(ゲーム本体)。世界の状態は VRAM に常駐し、CPU は窓・入力・Present・音の出力・セーブ I/O だけを担う。
add_executable(bicameral
  src/main.cpp
  src/editor/brush_panel.cpp
  src/editor/copy_panel.cpp  # 写しの早送り(T-0412)
  src/editor/play_metric_panel.cpp  # 遊びの指標(T-0432)
  src/editor/editor_overlay.cpp
  src/editor/gauge_panel.cpp
  src/editor/graph_panel.cpp
  src/editor/lab_panel.cpp
  src/editor/material_rows.cpp
  src/editor/mixer_panel.cpp  # 環境ミキサー(標準パッケージを世界に足す・外す。T-0452・D-457)
  src/editor/reaction_table_panel.cpp
  src/editor/coupling_rows.cpp  # 反応表のパネルの「結合」の欄(分類 drags・machines。T-0536)
  src/frame/auto_reload.cpp
  src/frame/auto_play.cpp  # --auto-play: 編集の状態・元に戻す/やり直す・プレイとやめるの確かめ(T-0497)
  src/frame/auto_play_table.cpp  # --auto-play-table: 表の差し替えを編集として扱う確かめ(T-0548)
  src/frame/frame_loop.cpp
  src/frame/table_hot_reload.cpp
  src/frame/trace_capture.cpp
  src/frame/world_copy_runner.cpp  # 写しの早送りを本物の刻みの合間に回す(T-0412)
  src/frame/play_metric_cli.cpp  # bicameral --measure-metrics(遊びの指標を測る。T-0432)
  src/frame/slope_gauge_cli.cpp  # bicameral --slope-gauge(開発ビルドの坂の計器。T-0518)
  src/frame/play_metric_runner.cpp  # 遊びの指標を本物の刻みの合間に測る(T-0432)
  src/platform/agility_sdk.cpp
  src/platform/caps.cpp
  src/platform/window.cpp
  src/render/probe_view.cpp
  src/render/screenshot.cpp
)
target_include_directories(bicameral PRIVATE src)
target_precompile_headers(bicameral PRIVATE src/pch.h)  # 重い外部ヘッダをまとめる(docs/style.md)
target_link_libraries(bicameral PRIVATE
  bicameral_core
  bicameral_gpu
  bicameral_sim
  bicameral_reaction_script
  bicameral_frame
  bicameral_save
  bicameral_view
  bicameral_editor
  imgui::imgui
  Microsoft::DirectX-Headers Microsoft::DirectX-Guids
  Microsoft::DirectX12-Agility
  d3d12 dxgi)

# --- ゲームのデータ(T-0157・ADR-0033)----------------------------------------------------------
# リポジトリの data/(反応表のパッケージ data/packages など)を exe の横の data/ に写す。ランタイムとテストはそこから読む。
# 消したファイルが残らないように、写す前に bin/data を消す(data/ のファイルが変わった時だけ走る)。
file(GLOB_RECURSE BICAMERAL_DATA_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/data/*)
set(BICAMERAL_DATA_OUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/data)
set(BICAMERAL_DATA_STAMP ${CMAKE_CURRENT_BINARY_DIR}/data.stamp)
add_custom_command(OUTPUT ${BICAMERAL_DATA_STAMP}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${BICAMERAL_DATA_OUT}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data ${BICAMERAL_DATA_OUT}
  COMMAND ${CMAKE_COMMAND} -E touch ${BICAMERAL_DATA_STAMP}
  DEPENDS ${BICAMERAL_DATA_FILES}
  COMMENT "data/ を bin/data に写す")
add_custom_target(bicameral_data ALL DEPENDS ${BICAMERAL_DATA_STAMP})
add_dependencies(bicameral bicameral_data)

# --- Agility SDK の配置(ADR-0004)-------------------------------------------------------------
# exe の横の D3D12\ に D3D12Core.dll を置く。exe は agility_sdk.cpp でその場所と版をエクスポートし、
# OS の d3d12.dll が起動時にこちらのランタイムを読み込む。デバッグレイヤーも同じ版を使う。
add_custom_command(TARGET bicameral POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:bicameral>/D3D12
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_PROPERTY:Microsoft::DirectX12-Core,IMPORTED_LOCATION_RELEASE>
          $<TARGET_PROPERTY:Microsoft::DirectX12-Layers,IMPORTED_LOCATION_DEBUG>
          $<TARGET_FILE_DIR:bicameral>/D3D12
  COMMAND_EXPAND_LISTS
  COMMENT "Agility SDK の D3D12Core.dll を bin/D3D12 にコピー")
# NuGet の WARP(版はルートの CMakeLists.txt で固定。T-0097・ADR-0013)。exe と同じフォルダに置く
add_custom_command(TARGET bicameral POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BICAMERAL_WARP_DLL} $<TARGET_FILE_DIR:bicameral>
  COMMENT "WARP ${BICAMERAL_WARP_VERSION} の d3d10warp.dll を bin にコピー")
if(BICAMERAL_GPU_VALIDATION)
  target_compile_definitions(bicameral PRIVATE BICAMERAL_GPU_VALIDATION=1)
endif()
if(BICAMERAL_PIX)
  target_compile_definitions(bicameral PRIVATE BICAMERAL_PIX=1)
endif()
