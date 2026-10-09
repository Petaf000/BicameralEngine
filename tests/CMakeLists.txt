# テスト。GPU カーネルごとに「CPU リファレンス実装との一致」テストを置く方針(docs/design/02-reaction-system.md)。
add_executable(smoke_test smoke_test.cpp)
add_test(NAME smoke COMMAND smoke_test)

add_executable(singleton_test singleton_test.cpp)
target_link_libraries(singleton_test PRIVATE bicameral_core)
add_test(NAME singleton COMMAND singleton_test)

add_executable(log_test log_test.cpp)
target_link_libraries(log_test PRIVATE bicameral_core)
add_test(NAME log COMMAND log_test)

add_executable(sim_scheduler_test sim_scheduler_test.cpp)
target_link_libraries(sim_scheduler_test PRIVATE bicameral_frame)
add_test(NAME sim_scheduler COMMAND sim_scheduler_test)

add_executable(time_control_test time_control_test.cpp)
target_link_libraries(time_control_test PRIVATE bicameral_editor)
add_test(NAME time_control COMMAND time_control_test)

add_executable(debug_camera_test debug_camera_test.cpp)
target_link_libraries(debug_camera_test PRIVATE bicameral_view)
add_test(NAME debug_camera COMMAND debug_camera_test)

add_executable(replay_file_test replay_file_test.cpp)
target_link_libraries(replay_file_test PRIVATE bicameral_save)
add_test(NAME replay_file COMMAND replay_file_test)

add_executable(luau_sandbox_test luau_sandbox_test.cpp)  # Luau の殻(T-0020)
target_link_libraries(luau_sandbox_test PRIVATE bicameral_script)
add_test(NAME luau_sandbox COMMAND luau_sandbox_test)
add_executable(luau_package_test luau_package_test.cpp)  # Luau のパッケージ(T-0138)
target_link_libraries(luau_package_test PRIVATE bicameral_script)
add_test(NAME luau_package COMMAND luau_package_test)
add_executable(luau_type_check_test luau_type_check_test.cpp)  # 読み込む前の型検査(T-0140)
target_link_libraries(luau_type_check_test PRIVATE bicameral_script)
target_compile_definitions(luau_type_check_test PRIVATE BICAMERAL_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
add_test(NAME luau_type_check COMMAND luau_type_check_test)
add_executable(reaction_package_test reaction_package_test.cpp)  # Luau の反応表 → ベイク(T-0021)
target_link_libraries(reaction_package_test PRIVATE bicameral_reaction_script bicameral_core)
target_compile_definitions(reaction_package_test PRIVATE
  BICAMERAL_DATA_PACKAGES_DIR="${PROJECT_SOURCE_DIR}/data/packages")
add_dependencies(reaction_package_test bicameral_data)  # ランタイムの入り口は exe の横の data/packages を読む(T-0157)
add_test(NAME reaction_package COMMAND reaction_package_test)
add_executable(reaction_hot_reload_test reaction_hot_reload_test.cpp)  # 反応表のホットリロード(T-0139)
target_link_libraries(reaction_hot_reload_test PRIVATE bicameral_reaction_script bicameral_core)
target_compile_definitions(reaction_hot_reload_test PRIVATE
  BICAMERAL_DATA_PACKAGES_DIR="${PROJECT_SOURCE_DIR}/data/packages")
add_dependencies(reaction_hot_reload_test bicameral_data)  # 型の定義は exe の横の data/types を読む
add_test(NAME reaction_hot_reload COMMAND reaction_hot_reload_test)
set_tests_properties(reaction_hot_reload PROPERTIES TIMEOUT 300)
# ランタイムが読むパッケージの表で、今の場面(仮の世界の燃焼)が C++ の表とビット単位で同じ結果になる(T-0157。CPU リファレンス)
add_executable(reaction_package_scene_test reaction_package_scene_test.cpp)
target_precompile_headers(reaction_package_scene_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(reaction_package_scene_test PRIVATE bicameral_reaction_script bicameral_sim bicameral_core)
add_dependencies(reaction_package_scene_test bicameral_data)
add_test(NAME reaction_package_scene COMMAND reaction_package_scene_test)
# 再生ファイルに残した反応表だけで、パッケージのフォルダ無しに同じ表で再生でき、ハッシュ列が一致する(T-0170・T-0193。CPU リファレンス)
add_executable(reaction_replay_table_test reaction_replay_table_test.cpp)
target_precompile_headers(reaction_replay_table_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(reaction_replay_table_test PRIVATE bicameral_reaction_script bicameral_sim bicameral_save bicameral_core)
target_compile_definitions(reaction_replay_table_test PRIVATE
  BICAMERAL_DATA_PACKAGES_DIR="${PROJECT_SOURCE_DIR}/data/packages")
add_dependencies(reaction_replay_table_test bicameral_data)  # 型の定義は exe の横の data/types を読む
add_test(NAME reaction_replay_table COMMAND reaction_replay_table_test)
set_tests_properties(reaction_replay_table PROPERTIES TIMEOUT 600)
# 表が読めない・検査で落ちる時、ランタイムは窓を開く前に止まる(T-0157。GPU を使わない)
add_test(NAME reaction_package_runtime_stops
         COMMAND bicameral --frames 1 --packages ${CMAKE_CURRENT_SOURCE_DIR}/packages_broken)
set_tests_properties(reaction_package_runtime_stops PROPERTIES
  PASS_REGULAR_EXPRESSION "フレームのループを始められない: 反応表のパッケージ.*ベイクの検査で落ちた" TIMEOUT 60)

add_executable(reaction_test reaction_test.cpp)
target_link_libraries(reaction_test PRIVATE bicameral_reaction bicameral_core)
add_test(NAME reaction COMMAND reaction_test)

add_executable(reaction_contention_test reaction_contention_test.cpp)
target_link_libraries(reaction_contention_test PRIVATE bicameral_reaction bicameral_core)
add_test(NAME reaction_contention COMMAND reaction_contention_test)

add_executable(reaction_wait_test reaction_wait_test.cpp)
target_link_libraries(reaction_wait_test PRIVATE bicameral_reaction bicameral_core)
add_test(NAME reaction_wait COMMAND reaction_wait_test)

add_executable(gas_reference_test gas_reference_test.cpp)  # 気体の流れの CPU リファレンス G1(T-0026)
target_link_libraries(gas_reference_test PRIVATE bicameral_gas bicameral_core)
add_test(NAME gas_reference COMMAND gas_reference_test)

add_executable(multires_test multires_test.cpp)
target_link_libraries(multires_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires COMMAND multires_test)

add_executable(multires_activity_test multires_activity_test.cpp)  # 木の上の活性(T-0100)
target_link_libraries(multires_activity_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires_activity COMMAND multires_activity_test)

add_executable(multires_quiet_test multires_quiet_test.cpp)  # 静かなブロックを粗くする(T-0101)
target_link_libraries(multires_quiet_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires_quiet COMMAND multires_quiet_test)

add_executable(multires_uniform_test multires_uniform_test.cpp)  # 一様なブロック(T-0102)
target_link_libraries(multires_uniform_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires_uniform COMMAND multires_uniform_test)

add_executable(multires_conduction_test multires_conduction_test.cpp)  # 木の上の熱の伝導(T-0019)
target_link_libraries(multires_conduction_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires_conduction COMMAND multires_conduction_test)

add_executable(lab_box_test lab_box_test.cpp)  # 実験室の箱の CPU 側(T-0142)
target_link_libraries(lab_box_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME lab_box COMMAND lab_box_test)

add_executable(multires_subcycle_test multires_subcycle_test.cpp)  # 細かいレベルの熱の刻み(T-0108)
target_link_libraries(multires_subcycle_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires_subcycle COMMAND multires_subcycle_test)

add_executable(multires_implicit_test multires_implicit_test.cpp)  # 細かいレベルの熱の陰解法の試作(T-0110 研究)
target_link_libraries(multires_implicit_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires_implicit COMMAND multires_implicit_test)

add_executable(multires_implicit_tree_test multires_implicit_tree_test.cpp)  # 細かいレベルの熱の陰解法を木の伝導に(T-0119)
target_link_libraries(multires_implicit_tree_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires_implicit_tree COMMAND multires_implicit_tree_test)

add_executable(fixed_test fixed_test.cpp)
target_link_libraries(fixed_test PRIVATE bicameral_fixed)
add_test(NAME fixed COMMAND fixed_test)

# 整数の AVBD(T-0016)。場面のテストは数十秒かかる(debug はもっと)
add_executable(physics_test physics_test.cpp)
target_link_libraries(physics_test PRIVATE bicameral_physics bicameral_core)
add_test(NAME physics_math COMMAND physics_test math)
foreach(_scene IN ITEMS stack mass_ratio pile wall)
  add_test(NAME physics_${_scene} COMMAND physics_test scene ${_scene})
  set_tests_properties(physics_${_scene} PROPERTIES TIMEOUT 600)
endforeach()
# release の速さで FX_ASSERT(桁あふれ・割り算の商の範囲)を有効にして(BICAMERAL_FORCE_ASSERT、fixed.hlsli)、場面を全部の刻み走らせる(R8 の確認)。
# debug は約 20 倍遅く場面の全部を回せないので、release のときだけ足す
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_executable(physics_checked_test physics_test.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/sim/physics_scene.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/sim/physics_world.cpp)
  target_include_directories(physics_checked_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src)
  target_link_libraries(physics_checked_test PRIVATE bicameral_fixed bicameral_core)
  target_compile_definitions(physics_checked_test PRIVATE BICAMERAL_FORCE_ASSERT=1)
  foreach(_scene IN ITEMS stack mass_ratio pile)
    add_test(NAME physics_${_scene}_checked COMMAND physics_checked_test scene ${_scene} --full)
    set_tests_properties(physics_${_scene}_checked PROPERTIES TIMEOUT 600)
  endforeach()
endif()

# --- GPU を使うテスト(T-0013)-------------------------------------------------------------------------
# ラベル gpu: ハードウェアの GPU が要る(CI の Windows の機械には無いので CI は -LE gpu で外す)。
# ラベル warp: WARP(ソフトウェアの D3D12)で走る。GPU の無い機械でも回る。
# exe はランタイムと同じ bin/ に出るので、Agility SDK の D3D12Core.dll(bin/D3D12)とシェーダー(bin/shaders)をそのまま使う。
function(bicameral_add_gpu_test name)
  add_executable(${name} ${name}.cpp ${PROJECT_SOURCE_DIR}/engine/src/platform/agility_sdk.cpp)
  target_precompile_headers(${name} PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)
  target_link_libraries(${name} PRIVATE bicameral_gpu bicameral_fixed Microsoft::DirectX12-Agility)
  add_dependencies(${name} bicameral bicameral_shaders)  # bin/D3D12 と bin/shaders を先に作る
endfunction()

# add_gpu_test_case(<テスト名> <exe> <ラベル> <引数...>)
function(bicameral_add_gpu_test_case testName executable label)
  add_test(NAME ${testName} COMMAND ${executable} ${ARGN})
  # GPU のテストが固まったとき(Work Graph のハング・WARP)に runner を長く塞がないよう、ctest の既定(1500 秒)より短く切る(T-0005)
  set_tests_properties(${testName} PROPERTIES LABELS ${label} TIMEOUT 300)
endfunction()

bicameral_add_gpu_test(gpu_fixed_test)
bicameral_add_gpu_test_case(gpu_fixed gpu_fixed_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_fixed_warp gpu_fixed_test warp --warp --queue compute)

bicameral_add_gpu_test(gpu_reaction_test)
target_link_libraries(gpu_reaction_test PRIVATE bicameral_reaction)
bicameral_add_gpu_test_case(gpu_reaction gpu_reaction_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_reaction_warp gpu_reaction_test warp --warp --queue compute)
bicameral_add_gpu_test_case(gpu_reaction_limits gpu_reaction_test gpu --queue compute --limits)  # 上限に当たる場面(T-0022)
bicameral_add_gpu_test_case(gpu_reaction_limits_warp gpu_reaction_test warp --warp --queue compute --limits)

bicameral_add_gpu_test(gpu_multires_test)  # 多重解像度の入れ子の GPU と CPU のビット一致(T-0017)
target_link_libraries(gpu_multires_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires gpu_multires_test gpu --queue compute)
set_tests_properties(gpu_multires PROPERTIES TIMEOUT 1200)  # debug の GPU-based validation で、HW も待ちの丸めにしてから 600 s を超えた(T-0124。場面ごとに約 80 s × 8)
# WARP は exe の横の NuGet の版(T-0097)で動く。OS の WARP では Work Graph を作る所で落ちる(ADR-0013)
bicameral_add_gpu_test_case(gpu_multires_warp gpu_multires_test warp --warp --queue compute)

bicameral_add_gpu_test(gpu_multires_implicit_test)  # 細かいレベルの熱の陰解法の GPU と CPU のビット一致(T-0117)
target_link_libraries(gpu_multires_implicit_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_implicit gpu_multires_implicit_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_implicit_warp gpu_multires_implicit_test warp --warp --queue compute)
set_tests_properties(gpu_multires_implicit gpu_multires_implicit_warp PROPERTIES TIMEOUT 900)
bicameral_add_gpu_test(gpu_multires_implicit_tree_test)  # 木につないだ陰解法の系を GPU で解いて CPU とビット一致(T-0127)
target_link_libraries(gpu_multires_implicit_tree_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_implicit_tree gpu_multires_implicit_tree_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_implicit_tree_warp gpu_multires_implicit_tree_test warp --warp --queue compute)
set_tests_properties(gpu_multires_implicit_tree gpu_multires_implicit_tree_warp PROPERTIES TIMEOUT 1200)
bicameral_add_gpu_test(gpu_multires_implicit_build_test)  # 陰解法の系(未知数・境のセル・面)を GPU で作って CPU とビット一致(T-0129)
target_link_libraries(gpu_multires_implicit_build_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_implicit_build gpu_multires_implicit_build_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_implicit_build_warp gpu_multires_implicit_build_test warp --warp --queue compute)
set_tests_properties(gpu_multires_implicit_build gpu_multires_implicit_build_warp PROPERTIES TIMEOUT 1200)
bicameral_add_gpu_test(gpu_multires_implicit_conduction_test)  # GPU の伝導の段から陰解法を呼んで全部・活性の刻みで CPU とビット一致(T-0132)
target_link_libraries(gpu_multires_implicit_conduction_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_implicit_conduction gpu_multires_implicit_conduction_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_implicit_conduction_warp gpu_multires_implicit_conduction_test warp --warp --queue compute)
set_tests_properties(gpu_multires_implicit_conduction gpu_multires_implicit_conduction_warp PROPERTIES TIMEOUT 1200)

bicameral_add_gpu_test(gpu_multires_activity_test)  # 木の上の活性の GPU と CPU のビット一致(T-0100)
target_link_libraries(gpu_multires_activity_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_activity gpu_multires_activity_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_activity_warp gpu_multires_activity_test warp --warp --queue compute)

bicameral_add_gpu_test(gpu_multires_quiet_test)  # 静かなブロックを粗くする要求の GPU と CPU のビット一致(T-0101)
target_link_libraries(gpu_multires_quiet_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_quiet gpu_multires_quiet_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_quiet_warp gpu_multires_quiet_test warp --warp --queue compute)

bicameral_add_gpu_test(gpu_multires_uniform_test)  # 一様なブロックの GPU と CPU のビット一致(T-0102)
target_link_libraries(gpu_multires_uniform_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_uniform gpu_multires_uniform_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_uniform_warp gpu_multires_uniform_test warp --warp --queue compute)
set_tests_properties(gpu_multires_uniform gpu_multires_uniform_warp PROPERTIES TIMEOUT 900)  # debug で活性のグラフの作成(場面ごとに 1 回)が数分

bicameral_add_gpu_test(gpu_multires_conduction_test)  # 木の上の熱の伝導の GPU と CPU のビット一致(T-0107)
target_link_libraries(gpu_multires_conduction_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_conduction gpu_multires_conduction_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_conduction_warp gpu_multires_conduction_test warp --warp --queue compute)
set_tests_properties(gpu_multires_conduction gpu_multires_conduction_warp PROPERTIES TIMEOUT 900)  # debug で活性のグラフの作成に数分
bicameral_add_gpu_test_case(gpu_multires_subcycle gpu_multires_conduction_test gpu --queue compute --subcycle)  # 細かいレベルの熱の刻み(T-0109)
bicameral_add_gpu_test_case(gpu_multires_subcycle_warp gpu_multires_conduction_test warp --warp --queue compute --subcycle)
set_tests_properties(gpu_multires_subcycle gpu_multires_subcycle_warp PROPERTIES TIMEOUT 2400)  # CPU の小刻み 64 回が重い
bicameral_add_gpu_test_case(gpu_multires_near_fold gpu_multires_conduction_test gpu --queue compute --near-fold)  # 許容差つきで畳む(T-0112)
bicameral_add_gpu_test_case(gpu_multires_near_fold_warp gpu_multires_conduction_test warp --warp --queue compute --near-fold)
set_tests_properties(gpu_multires_near_fold gpu_multires_near_fold_warp PROPERTIES TIMEOUT 2400)  # 静かになるまで約 2000 刻み

bicameral_add_gpu_test(gpu_lab_box_test)  # 実験室の箱の GPU と CPU のビット一致・記録と再生(T-0142)
target_link_libraries(gpu_lab_box_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_lab_box gpu_lab_box_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_lab_box_warp gpu_lab_box_test warp --warp --queue compute)
set_tests_properties(gpu_lab_box gpu_lab_box_warp PROPERTIES TIMEOUT 900)  # 並走中は debug のパイプラインの作成に数分

bicameral_add_gpu_test(gpu_physics_test)  # 物理の 1 刻みの GPU と CPU のビット一致(T-0090)
target_link_libraries(gpu_physics_test PRIVATE bicameral_sim)
foreach(_scene IN ITEMS stack mass_ratio pile)
  bicameral_add_gpu_test_case(gpu_physics_${_scene} gpu_physics_test gpu --scene ${_scene})
endforeach()
# 壁(れんが 300 個が 1 つの島)。CPU リファレンスが重いので 120 刻み(release で約 55 s)
bicameral_add_gpu_test_case(gpu_physics_wall gpu_physics_test gpu --scene wall --ticks 120)
# debug は CPU リファレンスだけで約 280 s(2026-10-05)。パイプラインの作成がドライバのキャッシュに無いと +90 s で 300 s を超える
set_tests_properties(gpu_physics_wall PROPERTIES TIMEOUT 600)
# WARP(NuGet の版。T-0097)でも物理の Work Graph が動き CPU とビット一致する。重いので積み木だけ・120 刻み(PC の debug で 27 s)
bicameral_add_gpu_test_case(gpu_physics_stack_warp gpu_physics_test warp --warp --scene stack --ticks 120)
# 採らなかった方(広域の選別を Compute・色ごとの解を Work Graph。T-0092 で測って比べた)も壊れていないこと
bicameral_add_gpu_test_case(gpu_physics_stack_alternative gpu_physics_test gpu --scene stack --broadphase compute --solver graph)

# 整数のルーチンの費用の計測(T-0010、04 §6)。合否の無い計測なので ctest には登録しない。
# 走らせ方: job.py run -Preset release -Exe gpu_fixed_bench
bicameral_add_gpu_test(gpu_fixed_bench)

bicameral_add_gpu_test(gpu_work_graph_test)
bicameral_add_gpu_test_case(gpu_work_graph_direct gpu_work_graph_test gpu --queue direct)
bicameral_add_gpu_test_case(gpu_work_graph_compute gpu_work_graph_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_work_graph_warp_direct gpu_work_graph_test warp --warp --queue direct)
bicameral_add_gpu_test_case(gpu_work_graph_warp_compute gpu_work_graph_test warp --warp --queue compute)

# GPU のデバッグ基盤(T-0003): シェーダーの printf / assert のリング、debug layer の報告と DRED
bicameral_add_gpu_test(gpu_debug_ring_test)
bicameral_add_gpu_test_case(gpu_debug_ring gpu_debug_ring_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_debug_ring_warp gpu_debug_ring_test warp --warp --queue compute)
# Work Graphs のノードのカウンタと上限の検出(T-0008): わざと上限に近づけ・越えさせ、止めて数えるか・Warning と printf が出るか
bicameral_add_gpu_test(gpu_work_graph_stats_test)
bicameral_add_gpu_test_case(gpu_work_graph_stats gpu_work_graph_stats_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_work_graph_stats_warp gpu_work_graph_stats_test warp --warp --queue compute)
# T-0004・T-0012・T-0086・T-0089 の仮の刻み: 分け方を変えても CPU とビット一致、イベントの並び、記録 → 再生でハッシュ列が一致。
# 仮の世界(伝導 + 反応のノード)の WARP のテストは、NuGet の WARP 1.0.21 で動くようになった(T-0097。OS の WARP では落ちる。ADR-0013)
bicameral_add_gpu_test(gpu_probe_sim_test)
target_link_libraries(gpu_probe_sim_test PRIVATE bicameral_sim bicameral_save)
# HW の 4 つ(sim・trace・peek・fire)は GPU-based validation を切る(--no-gbv。2026-10-05 ユーザー決定): 大きいシェーダーを使うたびに
# GBV の計装で約 2 分止まり、300 s を超えた。どれも CPU とビット一致を毎回比べる。GBV が要る調べ物の時だけ外して走らせる(BACKLOG)
bicameral_add_gpu_test_case(gpu_probe_sim gpu_probe_sim_test gpu --no-gbv)
bicameral_add_gpu_test_case(gpu_probe_sim_warp gpu_probe_sim_test warp --warp)
# 伝導の連鎖のトレース(T-0087): 2 回・分け方を変えて同じ、CPU リファレンスの予想と一致、わざとずらして最初の刻み・ブロック・セル
bicameral_add_gpu_test(gpu_probe_trace_test)
target_link_libraries(gpu_probe_trace_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_trace gpu_probe_trace_test gpu --no-gbv)
bicameral_add_gpu_test_case(gpu_probe_trace_warp gpu_probe_trace_test warp --warp)
# WARP の仮の世界は重い(2026-10-03 の PC の debug で sim 130 s・trace 65 s)。CI の機械(コアが少ない)で 300 s を越えないよう長めに切る
set_tests_properties(gpu_probe_sim gpu_probe_sim_warp gpu_probe_trace_warp PROPERTIES TIMEOUT 900)  # gpu_probe_sim は debug で 300 秒を超える(T-0139 で表の差し替えを足した)
# 覗き窓(T-0096): 影の鎖と抽出の覗きの欄が CPU とビット一致・覗いても世界のハッシュ列が変わらない
bicameral_add_gpu_test(gpu_probe_physics_test)  # 仮の世界の刻みに入れた物理と押すコマンドの GPU と CPU のビット一致(T-0098)
target_link_libraries(gpu_probe_physics_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_physics gpu_probe_physics_test gpu)
bicameral_add_gpu_test_case(gpu_probe_physics_warp gpu_probe_physics_test warp --warp --ticks 120)
bicameral_add_gpu_test_case(gpu_probe_physics_compute gpu_probe_physics_test gpu --broadphase-compute)

# 巻き戻し(T-0143): 保存点へ写して戻し、コマンドを足し直すと最初から流した時と同じハッシュ列
bicameral_add_gpu_test(gpu_probe_rewind_test)
target_link_libraries(gpu_probe_rewind_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_rewind gpu_probe_rewind_test gpu)
# 物理のパイプラインの作成が、ほかの作業ツリーと GPU を分けている間は debug で 4 分を超えることがある
set_tests_properties(gpu_probe_rewind PROPERTIES TIMEOUT 900)

bicameral_add_gpu_test(gpu_probe_peek_test)
target_link_libraries(gpu_probe_peek_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_peek gpu_probe_peek_test gpu --no-gbv)
bicameral_add_gpu_test_case(gpu_probe_peek_warp gpu_probe_peek_test warp --warp)
# 3 回走らせる(覗く・覗かない・表の差し替え。T-0194)。debug で Multires のグラフの作成が並走中は 1 回数分
set_tests_properties(gpu_probe_peek gpu_probe_peek_warp PROPERTIES TIMEOUT 900)
# 木箱に火をつける(T-0089): 刻みごとのエネルギー・最後の元素・もう一度走らせて同じ・燃え広がる。
# ctest(debug・GBV)は 1 分(3600 刻み)。10 分(36,000 刻み)は release で: job.py run -Preset release -Exe gpu_probe_fire_test
bicameral_add_gpu_test(gpu_probe_fire_test)
target_link_libraries(gpu_probe_fire_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_fire gpu_probe_fire_test gpu --ticks 3600 --no-gbv)

# 伝導の Work Graph の時間を伝播の規模ごとに測る(T-0005。ctest には登録しない。docs/perf.md)。
# 走らせ方: job.py run -Preset release -Exe gpu_conduct_bench
bicameral_add_gpu_test(gpu_conduct_bench)
target_link_libraries(gpu_conduct_bench PRIVATE bicameral_sim)

# 窓の操作の記録 → 再生(T-0086、06「テスト」の 2 つ目・15 §2)。ランタイム(bicameral)を 2 回走らせる:
#   1 回目: 自動クリックで窓の操作を入れて記録する。2 回目: 別の分け方(重さを分けた単位・目標 fps)で再生し、刻みごとのハッシュが全部一致すれば 0。
# 窓を開くのでハードウェアの GPU と画面が要る(ラベル gpu。CI は外す)。
set(WINDOW_REPLAY_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_replay.bcreplay)
add_test(NAME window_replay_record
         COMMAND bicameral --frames 300 --auto-click --auto-push --record ${WINDOW_REPLAY_FILE})
add_test(NAME window_replay_play
         COMMAND bicameral --replay ${WINDOW_REPLAY_FILE} --sim-load 400000 --sim-split 3 --target-fps 30)
# 物理(T-0098)の積み木を押す。物理のパイプラインの作成が debug で約 1 分かかるので、時間の上限を広げた
set_tests_properties(window_replay_record PROPERTIES LABELS gpu FIXTURES_SETUP window_replay TIMEOUT 240)
set_tests_properties(window_replay_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_replay TIMEOUT 240)
# エディタの殻(T-0023): パネルを重ね、決まったフレームで止める・1 刻みずつ・速さを変えながら再生しても、ハッシュ列が同じ(ADR-0035)。
# 止まった・ちょうど 1 刻みずつ進んだかは --auto-time が確かめる(違えば 1)
add_test(NAME window_replay_editor
         COMMAND bicameral --replay ${WINDOW_REPLAY_FILE} --editor --auto-time)
set_tests_properties(window_replay_editor PROPERTIES LABELS gpu FIXTURES_REQUIRED window_replay TIMEOUT 240)
# 巻き戻し(T-0143): 40 刻みごとに保存点へ写しながら再生し、保存点が 2 つできたら古い方へ戻して再生し直しても、
# 戻した後のハッシュ列が記録と同じ(保存点 + 再生。ADR-0036)。戻せなかったら 1
add_test(NAME window_replay_rewind
         COMMAND bicameral --replay ${WINDOW_REPLAY_FILE} --editor --auto-rewind --save-interval 40)
set_tests_properties(window_replay_rewind PROPERTIES LABELS gpu FIXTURES_REQUIRED window_replay TIMEOUT 240)
# 実験室(T-0142): パネルの箱に木を置いて火を付け 60 刻み、GPU と CPU が毎刻み一致し、記録から再生しても同じ。通らなければ 1
add_test(NAME window_lab COMMAND bicameral --editor --auto-lab --frames 20)
set_tests_properties(window_lab PROPERTIES LABELS gpu TIMEOUT 900)  # 並走中は debug のパイプラインの作成に数分
# 覗き窓(T-0096): 覗かずに記録した再生ファイルを、火をつけた木箱の壁を覗いて一番細かい段まで潜りながら再生してもハッシュ列が同じ(D-403)
add_test(NAME window_replay_peek
         COMMAND bicameral --replay ${WINDOW_REPLAY_FILE} --peek 28,32,32 --peek-depth 9 --view slice)
set_tests_properties(window_replay_peek PROPERTIES LABELS gpu FIXTURES_REQUIRED window_replay TIMEOUT 240)
# 表の中身(T-0170・T-0193): 再生ファイルに残った反応表だけで、パッケージのフォルダが無くても同じハッシュ列で再生できる
add_test(NAME window_replay_no_packages
         COMMAND bicameral --replay ${WINDOW_REPLAY_FILE} --packages ${CMAKE_CURRENT_BINARY_DIR}/no_such_packages)
set_tests_properties(window_replay_no_packages PROPERTIES LABELS gpu FIXTURES_REQUIRED window_replay TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生ファイルの表を使う.*再生: OK")

# 画像の比較(T-0025、16 §5): 決まった刻みの始めで世界を止め、S(その刻み) を描いた画面を基準の画像(tests/images/)と比べる。
# 写す(ランタイム)→ 比べる(tools/image_compare)の 2 つのテスト。実際の画像と差の画像はビルドのフォルダの images/ に出る。
# 基準の作り方・置き換え方は 16 §5。窓を開くのでラベル gpu(CI は外す)。--frames は刻みに届かなかった時の上限
set(IMAGE_ACTUAL_DIR ${CMAKE_CURRENT_BINARY_DIR}/images)
set(IMAGE_BASELINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/images)
set(IMAGE_COMPARE ${PROJECT_SOURCE_DIR}/tools/image_compare/image_compare.py)
function(bicameral_add_image_test name tick)
  add_test(NAME image_${name}_capture
           COMMAND bicameral --frames 3000 --screenshot-tick ${tick} --screenshot ${IMAGE_ACTUAL_DIR}/${name}.bmp ${ARGN})
  add_test(NAME image_${name}
           COMMAND ${Python3_EXECUTABLE} ${IMAGE_COMPARE} compare ${IMAGE_ACTUAL_DIR}/${name}.bmp
                   ${IMAGE_BASELINE_DIR}/${name}.png --diff ${IMAGE_ACTUAL_DIR}/${name}.diff.png)
  set_tests_properties(image_${name}_capture PROPERTIES LABELS gpu FIXTURES_SETUP image_${name} TIMEOUT 300)
  set_tests_properties(image_${name} PROPERTIES LABELS gpu FIXTURES_REQUIRED image_${name} TIMEOUT 120)
endfunction()
# 木箱の壁に火をつけた(--auto-ignite。最初の刻みに載るので毎回同じ世界)4 秒後を、斜めから(体積)と、z = 32 の面を正面から(断面)。
# debug では 1 つ約 2 分(ほとんどがパイプラインの作成。--no-physics は GPU-based validation が入って逆に約 3 分になる)
bicameral_add_image_test(probe_volume 240 --auto-ignite --view volume)
bicameral_add_image_test(probe_slice 240 --auto-ignite --view slice --camera 0,0,80)
# 比べ方そのもの(読み書き・許容・差の画像)の試験。GPU 無し(CI でも回る)
add_test(NAME image_compare_selftest COMMAND ${Python3_EXECUTABLE} ${IMAGE_COMPARE} selftest)
bicameral_add_gpu_test(gpu_debug_device_test)
bicameral_add_gpu_test_case(gpu_debug_device gpu_debug_device_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_debug_device_warp gpu_debug_device_test warp --warp --queue compute)

# --- 浮動小数点の検査(tools/float_check、04 §4)が本当に落ちるかのテスト ---------------------------------
# わざと浮動小数点を使うシェーダーを(検査なしで)逆アセンブルし、検査が「拒否する」ことを確かめる。
# 検査がいつの間にか何も捕まえなくなっていたら、ここが落ちる。
set(FLOAT_CHECK_DIR ${PROJECT_SOURCE_DIR}/tools/float_check)
set(FLOAT_CHECK_FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/float_check)
set(FLOAT_CHECK_ASM ${CMAKE_CURRENT_BINARY_DIR}/float_check)
set(FLOAT_CHECK_REJECT_SHADERS reject_convert reject_asfloat)
set(FLOAT_CHECK_ASM_FILES "")
foreach(name IN LISTS FLOAT_CHECK_REJECT_SHADERS)
  add_custom_command(
    OUTPUT ${FLOAT_CHECK_ASM}/${name}.asm
    COMMAND ${CMAKE_COMMAND} -E make_directory ${FLOAT_CHECK_ASM}
    COMMAND ${DIRECTX_DXC_TOOL} -HV 2021 -T cs_6_8 -E Main -Fc ${FLOAT_CHECK_ASM}/${name}.asm
            -Fo ${FLOAT_CHECK_ASM}/${name}.cso ${FLOAT_CHECK_FIXTURES}/${name}.hlsl
    DEPENDS ${FLOAT_CHECK_FIXTURES}/${name}.hlsl
    COMMENT "DXC(検査のテスト用): ${name}.hlsl"
    VERBATIM)
  list(APPEND FLOAT_CHECK_ASM_FILES ${FLOAT_CHECK_ASM}/${name}.asm)
  add_test(NAME float_check_dxil_rejects_${name}
           COMMAND ${Python3_EXECUTABLE} ${FLOAT_CHECK_DIR}/dxil_float_check.py
                   ${FLOAT_CHECK_ASM}/${name}.asm --source ${name}.hlsl --expect-reject)
endforeach()
add_custom_target(float_check_fixtures ALL DEPENDS ${FLOAT_CHECK_ASM_FILES})

# 本物のシミュのシェーダー(ビルドで検査済み)は通る
add_test(NAME float_check_dxil_accepts_fixed_selftest
         COMMAND ${Python3_EXECUTABLE} ${FLOAT_CHECK_DIR}/dxil_float_check.py
                 ${CMAKE_BINARY_DIR}/shaders/asm/sim/fixed_selftest.asm --source sim/fixed_selftest.hlsl)
# ソースの検査: 型を書かない抜け道(リテラル・<cmath>)も拒否し、コメント・文字列・16 進は誤検出しない
add_test(NAME float_check_source_rejects
         COMMAND ${Python3_EXECUTABLE} ${FLOAT_CHECK_DIR}/source_float_check.py
                 ${FLOAT_CHECK_FIXTURES}/reject_sources --expect-reject)
add_test(NAME float_check_source_accepts
         COMMAND ${Python3_EXECUTABLE} ${FLOAT_CHECK_DIR}/source_float_check.py
                 ${FLOAT_CHECK_FIXTURES}/accept_source.hlsli)
