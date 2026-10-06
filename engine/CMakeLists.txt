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
  src/sim/reaction_test_table.cpp
)
target_include_directories(bicameral_reaction PUBLIC src)
target_link_libraries(bicameral_reaction PUBLIC bicameral_fixed)

# --- multires: 多重解像度の入れ子の細分の CPU リファレンス(17。T-0017)。GPU を知らない。浮動小数点は禁止 ---
# 細かくする・粗くする・影の引き戻しの本体は shaders/common/multires.hlsli(HLSL と共通)。
add_library(bicameral_multires STATIC
  src/sim/implicit_conduction.cpp
  src/sim/multires_activity.cpp
  src/sim/multires_conduction.cpp
  src/sim/multires_implicit_conduction.cpp
  src/sim/multires_nest.cpp
  src/sim/multires_tree.cpp
)
target_include_directories(bicameral_multires PUBLIC src)
target_link_libraries(bicameral_multires PUBLIC bicameral_reaction)

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
  src/sim/gpu_multires.cpp
  src/sim/gpu_implicit.cpp
  src/sim/gpu_physics.cpp
  src/sim/probe_peek.cpp
  src/sim/probe_sim.cpp
  src/sim/probe_trace.cpp
)
target_include_directories(bicameral_sim PUBLIC src)
target_precompile_headers(bicameral_sim PRIVATE src/pch.h)
target_link_libraries(bicameral_sim PUBLIC bicameral_gpu bicameral_reaction bicameral_physics bicameral_multires)

# --- save: セーブ・再生ファイルの読み書き(15。T-0086)。CPU だけ(GPU を知らない。CPU だけのテストがある)---
# コマンドの形は sim/command.h(ヘッダだけ)を使う。
add_library(bicameral_save STATIC
  src/save/replay_file.cpp
  src/save/replay_session.cpp
)
target_include_directories(bicameral_save PUBLIC src)
target_link_libraries(bicameral_save PUBLIC bicameral_core)

# --- frame: フレームの歩調の計算(GPU を知らない。CPU だけのテストがある)---
add_library(bicameral_frame STATIC
  src/frame/sim_scheduler.cpp
)
target_include_directories(bicameral_frame PUBLIC src)

# --- view: デバッグ表示の操作とカメラ(D3D12 を知らない。CPU だけのテストがある。T-0015)---
add_library(bicameral_view STATIC
  src/render/debug_camera.cpp
  src/render/debug_view_controller.cpp
)
target_include_directories(bicameral_view PUBLIC src)
target_link_libraries(bicameral_view PUBLIC bicameral_core)

# ランタイム(ゲーム本体)。世界の状態は VRAM に常駐し、CPU は窓・入力・Present・音の出力・セーブ I/O だけを担う。
add_executable(bicameral
  src/main.cpp
  src/frame/frame_loop.cpp
  src/frame/trace_capture.cpp
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
  bicameral_frame
  bicameral_save
  bicameral_view
  Microsoft::DirectX-Headers Microsoft::DirectX-Guids
  Microsoft::DirectX12-Agility
  d3d12 dxgi)

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
