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

add_executable(reaction_test reaction_test.cpp)
target_link_libraries(reaction_test PRIVATE bicameral_reaction bicameral_core)
add_test(NAME reaction COMMAND reaction_test)

add_executable(multires_test multires_test.cpp)
target_link_libraries(multires_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires COMMAND multires_test)

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

bicameral_add_gpu_test(gpu_multires_test)  # 多重解像度の入れ子の GPU と CPU のビット一致(T-0017)
target_link_libraries(gpu_multires_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires gpu_multires_test gpu --queue compute)
# WARP は exe の横の NuGet の版(T-0097)で動く。OS の WARP では Work Graph を作る所で落ちる(ADR-0013)
bicameral_add_gpu_test_case(gpu_multires_warp gpu_multires_test warp --warp --queue compute)

bicameral_add_gpu_test(gpu_physics_test)  # 物理の 1 刻みの GPU と CPU のビット一致(T-0090)
target_link_libraries(gpu_physics_test PRIVATE bicameral_sim)
foreach(_scene IN ITEMS stack mass_ratio pile)
  bicameral_add_gpu_test_case(gpu_physics_${_scene} gpu_physics_test gpu --scene ${_scene})
endforeach()
# 壁(れんが 300 個が 1 つの島)。CPU リファレンスが重いので 120 刻み(release で約 55 s)
bicameral_add_gpu_test_case(gpu_physics_wall gpu_physics_test gpu --scene wall --ticks 120)
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
bicameral_add_gpu_test_case(gpu_probe_sim gpu_probe_sim_test gpu)
bicameral_add_gpu_test_case(gpu_probe_sim_warp gpu_probe_sim_test warp --warp)
# 伝導の連鎖のトレース(T-0087): 2 回・分け方を変えて同じ、CPU リファレンスの予想と一致、わざとずらして最初の刻み・ブロック・セル
bicameral_add_gpu_test(gpu_probe_trace_test)
target_link_libraries(gpu_probe_trace_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_trace gpu_probe_trace_test gpu)
bicameral_add_gpu_test_case(gpu_probe_trace_warp gpu_probe_trace_test warp --warp)
# WARP の仮の世界は重い(2026-10-03 の PC の debug で sim 130 s・trace 65 s)。CI の機械(コアが少ない)で 300 s を越えないよう長めに切る
set_tests_properties(gpu_probe_sim_warp gpu_probe_trace_warp PROPERTIES TIMEOUT 900)
# 覗き窓(T-0096): 影の鎖と抽出の覗きの欄が CPU とビット一致・覗いても世界のハッシュ列が変わらない
bicameral_add_gpu_test(gpu_probe_physics_test)  # 仮の世界の刻みに入れた物理と押すコマンドの GPU と CPU のビット一致(T-0098)
target_link_libraries(gpu_probe_physics_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_physics gpu_probe_physics_test gpu)
bicameral_add_gpu_test_case(gpu_probe_physics_warp gpu_probe_physics_test warp --warp --ticks 120)
bicameral_add_gpu_test_case(gpu_probe_physics_compute gpu_probe_physics_test gpu --broadphase-compute)

bicameral_add_gpu_test(gpu_probe_peek_test)
target_link_libraries(gpu_probe_peek_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_peek gpu_probe_peek_test gpu)
bicameral_add_gpu_test_case(gpu_probe_peek_warp gpu_probe_peek_test warp --warp)
# 木箱に火をつける(T-0089): 刻みごとのエネルギー・最後の元素・もう一度走らせて同じ・燃え広がる。
# ctest(debug・GBV)は 1 分(3600 刻み)。10 分(36,000 刻み)は release で: job.py run -Preset release -Exe gpu_probe_fire_test
bicameral_add_gpu_test(gpu_probe_fire_test)
target_link_libraries(gpu_probe_fire_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_fire gpu_probe_fire_test gpu --ticks 3600)

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
# 覗き窓(T-0096): 覗かずに記録した再生ファイルを、火をつけた木箱の壁を覗いて一番細かい段まで潜りながら再生してもハッシュ列が同じ(D-403)
add_test(NAME window_replay_peek
         COMMAND bicameral --replay ${WINDOW_REPLAY_FILE} --peek 28,32,32 --peek-depth 9 --view slice)
set_tests_properties(window_replay_peek PROPERTIES LABELS gpu FIXTURES_REQUIRED window_replay TIMEOUT 240)
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
