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

add_executable(debug_camera_test debug_camera_test.cpp)
target_link_libraries(debug_camera_test PRIVATE bicameral_view)
add_test(NAME debug_camera COMMAND debug_camera_test)

add_executable(replay_file_test replay_file_test.cpp)
target_link_libraries(replay_file_test PRIVATE bicameral_save)
add_test(NAME replay_file COMMAND replay_file_test)

add_executable(fixed_test fixed_test.cpp)
target_link_libraries(fixed_test PRIVATE bicameral_fixed)
add_test(NAME fixed COMMAND fixed_test)

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
# T-0004・T-0012・T-0086 の仮の刻み: 分け方を変えても CPU とビット一致、イベントの並び、記録 → 再生でハッシュ列が一致
bicameral_add_gpu_test(gpu_probe_sim_test)
target_link_libraries(gpu_probe_sim_test PRIVATE bicameral_sim bicameral_save)
bicameral_add_gpu_test_case(gpu_probe_sim gpu_probe_sim_test gpu)
bicameral_add_gpu_test_case(gpu_probe_sim_warp gpu_probe_sim_test warp --warp)
# 伝導の連鎖のトレース(T-0087): 2 回・分け方を変えて同じ、CPU リファレンスの予想と一致、わざとずらして最初の刻み・ブロック・セル
bicameral_add_gpu_test(gpu_probe_trace_test)
target_link_libraries(gpu_probe_trace_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_trace gpu_probe_trace_test gpu)
bicameral_add_gpu_test_case(gpu_probe_trace_warp gpu_probe_trace_test warp --warp)

# 伝導の Work Graph の時間を伝播の規模ごとに測る(T-0005。ctest には登録しない。docs/perf.md)。
# 走らせ方: job.py run -Preset release -Exe gpu_conduct_bench
bicameral_add_gpu_test(gpu_conduct_bench)
target_link_libraries(gpu_conduct_bench PRIVATE bicameral_sim)

# 窓の操作の記録 → 再生(T-0086、06「テスト」の 2 つ目・15 §2)。ランタイム(bicameral)を 2 回走らせる:
#   1 回目: 自動クリックで窓の操作を入れて記録する。2 回目: 別の分け方(重さを分けた単位・目標 fps)で再生し、刻みごとのハッシュが全部一致すれば 0。
# 窓を開くのでハードウェアの GPU と画面が要る(ラベル gpu。CI は外す)。
set(WINDOW_REPLAY_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_replay.bcreplay)
add_test(NAME window_replay_record
         COMMAND bicameral --frames 300 --auto-click --record ${WINDOW_REPLAY_FILE})
add_test(NAME window_replay_play
         COMMAND bicameral --replay ${WINDOW_REPLAY_FILE} --sim-load 400000 --sim-split 3 --target-fps 30)
set_tests_properties(window_replay_record PROPERTIES LABELS gpu FIXTURES_SETUP window_replay TIMEOUT 60)
set_tests_properties(window_replay_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_replay TIMEOUT 120)
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
