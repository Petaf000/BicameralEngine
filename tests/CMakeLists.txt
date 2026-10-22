# テスト。GPU カーネルごとに「CPU リファレンス実装との一致」テストを置く方針(docs/design/02-reaction-system.md)。

# 試験の反応表(T-0169): 中身は data/packages/combustion_test だけに書き、テストはそれを読む(reaction_test_table.h)。
# 試験の表を使うテストは全部これをリンクする(ファイルの末尾の一覧)
add_library(bicameral_test_reaction_table STATIC reaction_test_table.cpp)
target_link_libraries(bicameral_test_reaction_table PUBLIC bicameral_reaction_script bicameral_core)
target_compile_definitions(bicameral_test_reaction_table PRIVATE
  BICAMERAL_DATA_PACKAGES_DIR="${PROJECT_SOURCE_DIR}/data/packages"
  BICAMERAL_DATA_STANDARD_DIR="${PROJECT_SOURCE_DIR}/data/standard")  # 試験の表は標準パッケージに依存する(T-0498)

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

# 重さの検査(T-0413・ADR-0204): 予算・スローの窓の判定と、単位の GPU 時間を刻みにまとめる部品(GPU を使わない)
add_executable(weight_check_test weight_check_test.cpp)
target_link_libraries(weight_check_test PRIVATE bicameral_frame bicameral_sim)
target_precompile_headers(weight_check_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
add_test(NAME weight_check COMMAND weight_check_test)

add_executable(time_control_test time_control_test.cpp)
target_link_libraries(time_control_test PRIVATE bicameral_editor)
add_test(NAME time_control COMMAND time_control_test)

add_executable(brush_stroke_test brush_stroke_test.cpp)
target_link_libraries(brush_stroke_test PRIVATE bicameral_editor)
add_test(NAME brush_stroke COMMAND brush_stroke_test)
add_executable(brush_preview_test brush_preview_test.cpp)  # カーソルの下の筆の形の線(T-0279)
target_link_libraries(brush_preview_test PRIVATE bicameral_editor)
add_test(NAME brush_preview COMMAND brush_preview_test)
add_executable(brush_undo_test brush_undo_test.cpp)
target_link_libraries(brush_undo_test PRIVATE bicameral_editor)
add_test(NAME brush_undo COMMAND brush_undo_test)
add_executable(edit_table_test edit_table_test.cpp)
target_link_libraries(edit_table_test PRIVATE bicameral_editor)
add_test(NAME edit_table COMMAND edit_table_test)
add_executable(surface_pick_test surface_pick_test.cpp)  # 光線が最初に当たる物のセルの手前(T-0332)
target_link_libraries(surface_pick_test PRIVATE bicameral_editor)
add_test(NAME surface_pick COMMAND surface_pick_test)

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
# 相の表(sim::MakeSpeciesPhases。固体の動き方の欄が写るか。T-0819)のために bicameral_multires も
target_link_libraries(reaction_package_test PRIVATE bicameral_reaction_script bicameral_multires bicameral_core)
target_compile_definitions(reaction_package_test PRIVATE
  BICAMERAL_DATA_PACKAGES_DIR="${PROJECT_SOURCE_DIR}/data/packages")
add_dependencies(reaction_package_test bicameral_data)  # ランタイムの入り口は exe の横の data/packages を読む(T-0157)
add_test(NAME reaction_package COMMAND reaction_package_test)
# 放射の熱の交換の CPU リファレンス(T-0583・ADR-0252): 解析解・保存・坂を下る・行き過ぎの上限・物質の欄
add_executable(radiation_reference_test radiation_reference_test.cpp)
target_link_libraries(radiation_reference_test PRIVATE bicameral_test_reaction_table bicameral_reaction_script bicameral_core)
add_test(NAME radiation_reference COMMAND radiation_reference_test)
# 放射の対を平行な線の束で見つける(T-0584・ADR-0262): 平行な板の解析解・陰・受け手の分け前・保存・膜
# 太陽の影の柱の CPU リファレンス(T-0910・23 §10 R-R0): 光の値の欄・壁の陰・保存・細かさ・温室・多重解像度・決定的
add_executable(sun_column_test sun_column_test.cpp)
target_link_libraries(sun_column_test PRIVATE bicameral_reaction_script bicameral_multires bicameral_core)
add_test(NAME sun_column COMMAND sun_column_test)
# 日の光を物で集める CPU リファレンス(研究 R-R2・T-0878): 屈折の値の欄・焦点の距離・燃え始め・細かさ・保存・決定的・費用
add_executable(sun_lens_test sun_lens_test.cpp)
target_link_libraries(sun_lens_test PRIVATE bicameral_reaction_script bicameral_multires bicameral_core)
add_test(NAME sun_lens COMMAND sun_lens_test)
add_executable(radiation_lines_test radiation_lines_test.cpp)
target_link_libraries(radiation_lines_test PRIVATE bicameral_reaction bicameral_core)
add_test(NAME radiation_lines COMMAND radiation_lines_test)
# 草原の延焼の場面を放射の段つきの CPU リファレンスで測る(T-0585・ADR-0263)。ctest は 2 秒の煙の試し。測り方は .cpp の先頭
add_executable(radiation_scene_measure radiation_scene_measure.cpp)
target_link_libraries(radiation_scene_measure PRIVATE bicameral_sim bicameral_reaction_script bicameral_core)
target_precompile_headers(radiation_scene_measure PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_compile_definitions(radiation_scene_measure PRIVATE
  BICAMERAL_DATA_STANDARD_DIR="${PROJECT_SOURCE_DIR}/data/standard"
  BICAMERAL_DATA_SCENES_DIR="${PROJECT_SOURCE_DIR}/data/scenes"
  BICAMERAL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_dependencies(radiation_scene_measure bicameral_data)
add_test(NAME radiation_scene COMMAND radiation_scene_measure)
set_tests_properties(radiation_scene PROPERTIES TIMEOUT 3000)
add_executable(environment_package_test environment_package_test.cpp)  # 世界の環境の値(分類 environment。T-0400・ADR-0176)
target_link_libraries(environment_package_test PRIVATE bicameral_reaction_script bicameral_core)
target_compile_definitions(environment_package_test PRIVATE
  BICAMERAL_DATA_PACKAGES_DIR="${PROJECT_SOURCE_DIR}/data/packages")
add_dependencies(environment_package_test bicameral_data)  # 型の定義は exe の横の data/types を読む
add_test(NAME environment_package COMMAND environment_package_test)
add_executable(standard_packages_test standard_packages_test.cpp)  # 標準パッケージと環境ミキサーの足す一覧(T-0373・ADR-0182・ADR-0183)
target_link_libraries(standard_packages_test PRIVATE bicameral_reaction_script bicameral_core)
target_compile_definitions(standard_packages_test PRIVATE
  BICAMERAL_DATA_PACKAGES_DIR="${PROJECT_SOURCE_DIR}/data/packages"
  BICAMERAL_DATA_STANDARD_DIR="${PROJECT_SOURCE_DIR}/data/standard"
  BICAMERAL_DATA_SCENES_DIR="${PROJECT_SOURCE_DIR}/data/scenes")  # ⑤ の場面がゲーム本体として読めるか(T-0557)
add_dependencies(standard_packages_test bicameral_data)  # 型の定義は exe の横の data/types を読む
add_test(NAME standard_packages COMMAND standard_packages_test)

add_executable(world_atmosphere_test world_atmosphere_test.cpp)  # 基準の大気の高さの分布(T-0499・ADR-0182 の改訂)
target_link_libraries(world_atmosphere_test PRIVATE bicameral_reaction_script bicameral_gas bicameral_core)
target_compile_definitions(world_atmosphere_test PRIVATE
  BICAMERAL_DATA_STANDARD_DIR="${PROJECT_SOURCE_DIR}/data/standard"
  BICAMERAL_ATMOSPHERE_PACKAGES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/packages_atmosphere")
add_dependencies(world_atmosphere_test bicameral_data)  # 型の定義は exe の横の data/types を読む
add_test(NAME world_atmosphere COMMAND world_atmosphere_test)
add_executable(reaction_hot_reload_test reaction_hot_reload_test.cpp)  # 反応表のホットリロード(T-0139)
target_link_libraries(reaction_hot_reload_test PRIVATE bicameral_reaction_script bicameral_core)
target_compile_definitions(reaction_hot_reload_test PRIVATE
  BICAMERAL_DATA_PACKAGES_DIR="${PROJECT_SOURCE_DIR}/data/packages")
add_dependencies(reaction_hot_reload_test bicameral_data)  # 型の定義は exe の横の data/types を読む
add_test(NAME reaction_hot_reload COMMAND reaction_hot_reload_test)
set_tests_properties(reaction_hot_reload PROPERTIES TIMEOUT 300)
# 反応表のパネルの中身(T-0219): Luau のリテラルを探して書き戻す → 読み直すと同じ表(変えた値だけ違う)・保存則の検査
add_executable(reaction_table_edit_test reaction_table_edit_test.cpp)
target_link_libraries(reaction_table_edit_test PRIVATE bicameral_reaction_script bicameral_core)
target_compile_definitions(reaction_table_edit_test PRIVATE
  BICAMERAL_DATA_PACKAGES_DIR="${PROJECT_SOURCE_DIR}/data/packages"
  BICAMERAL_DATA_STANDARD_DIR="${PROJECT_SOURCE_DIR}/data/standard")
add_dependencies(reaction_table_edit_test bicameral_data)
add_test(NAME reaction_table_edit COMMAND reaction_table_edit_test)
set_tests_properties(reaction_table_edit PROPERTIES TIMEOUT 300)
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
# ブランクの世界(T-0371・D-455): 空の表のベイクと作り直し・何刻み進めても何も起きない・再生ファイルの世界の種類(CPU リファレンス)
add_executable(blank_world_test blank_world_test.cpp)
target_precompile_headers(blank_world_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(blank_world_test PRIVATE bicameral_reaction_script bicameral_sim bicameral_save bicameral_core)
add_test(NAME blank_world COMMAND blank_world_test)

# 物理で失った力学的エネルギーを仮の世界のセルの熱へ返す(T-0542。CPU の物理 + CPU リファレンスの反応の世界)
add_executable(physics_heat_test physics_heat_test.cpp)
target_precompile_headers(physics_heat_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(physics_heat_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME physics_heat COMMAND physics_heat_test)

# 仮の世界に気体を載せる CPU リファレンス(T-0590・ADR-0256): 静かな大気・燃える木箱の煙が上がる・帳簿込みの保存・決定性
add_executable(probe_gas_test probe_gas_test.cpp)
target_precompile_headers(probe_gas_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(probe_gas_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME probe_gas COMMAND probe_gas_test)
set_tests_properties(probe_gas PROPERTIES TIMEOUT 1800)

add_executable(probe_liquid_test probe_liquid_test.cpp)
target_precompile_headers(probe_liquid_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(probe_liquid_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME probe_liquid COMMAND probe_liquid_test)
set_tests_properties(probe_liquid PROPERTIES TIMEOUT 1800)

# 乱した気体・液体が眠りに戻るか(T-0723。研究): 静止の丸めを入れても保存と決定性・活性のブロックの推移(--long は打ち切り条件の場面)
add_executable(rest_return_test rest_return_test.cpp)
target_precompile_headers(rest_return_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(rest_return_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME rest_return COMMAND rest_return_test)
set_tests_properties(rest_return PROPERTIES TIMEOUT 1800)

add_executable(probe_phase_test probe_phase_test.cpp)  # 水の相変化を反応表の規則で(T-0611・ADR-0272。CPU だけ)
target_precompile_headers(probe_phase_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(probe_phase_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME probe_phase COMMAND probe_phase_test)
set_tests_properties(probe_phase PROPERTIES TIMEOUT 1800)
add_test(NAME probe_phase_porous_stone COMMAND probe_phase_test --stone-without-density)  # 場所を取らない石へ水が染み込む時の保存(T-0654)
set_tests_properties(probe_phase_porous_stone PROPERTIES TIMEOUT 1800)
# 1 セルの体積を相で分け合う(T-0593・T-0612・ADR-0268): 砂の壁が気体と水を塞ぐ・少しなら通る・燃えて減ると通る・水が空気を押し出す
add_executable(cell_volume_test cell_volume_test.cpp)
target_precompile_headers(cell_volume_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(cell_volume_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME cell_volume COMMAND cell_volume_test)

add_executable(cell_view_test cell_view_test.cpp)  # セルの見立てが今の熱・空き・液体・気体の値とビット一致(T-0713)
target_precompile_headers(cell_view_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)
target_link_libraries(cell_view_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME cell_view COMMAND cell_view_test)

add_executable(transport_apply_test transport_apply_test.cpp)  # 当てる段の上限と伝導の種類が今の式とビット一致(T-0714)
target_precompile_headers(transport_apply_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)
target_link_libraries(transport_apply_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME transport_apply COMMAND transport_apply_test)

add_executable(multires_liquid_test multires_liquid_test.cpp)  # 多重解像度の 1 段の液体が仮の世界の液体と毎刻みビット一致(T-0717)
target_precompile_headers(multires_liquid_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(multires_liquid_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME multires_liquid COMMAND multires_liquid_test)
add_executable(multires_gas_test multires_gas_test.cpp)  # 多重解像度の 1 段の気体が仮の世界の気体と毎刻みビット一致(T-0720)
target_precompile_headers(multires_gas_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(multires_gas_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME multires_gas COMMAND multires_gas_test)

set_tests_properties(cell_volume PROPERTIES TIMEOUT 1800)
set_tests_properties(physics_heat PROPERTIES TIMEOUT 900)
set_tests_properties(reaction_replay_table PROPERTIES TIMEOUT 600)
# 世界の組み立て(T-0398・ADR-0172): 試験のパッケージの world_cells・world_bodies が前の C++ の組み立て(木箱・積み木)とビット一致・
# ランタイムの入り口と再生ファイルの中身から同じ・ブランクは空・Mod で足せる・誤りの出し方(CPU)
add_executable(world_layout_test world_layout_test.cpp)
target_precompile_headers(world_layout_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(world_layout_test PRIVATE bicameral_reaction_script bicameral_sim bicameral_core)
target_compile_definitions(world_layout_test PRIVATE
  BICAMERAL_DATA_PACKAGES_DIR="${PROJECT_SOURCE_DIR}/data/packages")
add_dependencies(world_layout_test bicameral_data)  # 型の定義は exe の横の data/types を読む
add_test(NAME world_layout COMMAND world_layout_test)
add_executable(play_metric_test play_metric_test.cpp)  # 遊びの指標の形(分類 play_metrics・差・値のファイル。T-0432・ADR-0206)
target_precompile_headers(play_metric_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(play_metric_test PRIVATE bicameral_reaction_script bicameral_sim bicameral_core)
add_dependencies(play_metric_test bicameral_data)  # 型の定義は exe の横の data/types を読む
add_test(NAME play_metric COMMAND play_metric_test)
# 坂の検査の物差し(D-487・T-0491): 燃える木箱で面の熱の流れと反応ごとのエントロピーの生成を数える(CPU。`--burn [刻み]` で燃え方の計器)
add_executable(slope_check_test slope_check_test.cpp)
target_precompile_headers(slope_check_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(slope_check_test PRIVATE bicameral_test_reaction_table bicameral_reaction_script bicameral_sim
                      bicameral_core)  # 試験の表(末尾の一覧に足さない: 並走のほかの枝と重ならないように)
add_dependencies(slope_check_test bicameral_data)  # 型の定義は exe の横の data/types を読む
add_test(NAME slope_check COMMAND slope_check_test)
set_tests_properties(slope_check PROPERTIES TIMEOUT 900)
# 物質を足す・消す表の差し替えで、セルの物質 ID を名前で付け替える(T-0223・ADR-0065。CPU リファレンス。GPU は T-0242)
add_executable(reaction_species_remap_test reaction_species_remap_test.cpp)
target_precompile_headers(reaction_species_remap_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(reaction_species_remap_test PRIVATE bicameral_reaction_script bicameral_sim bicameral_core)
add_test(NAME reaction_species_remap COMMAND reaction_species_remap_test)
set_tests_properties(reaction_species_remap PROPERTIES TIMEOUT 600)
# 表が読めない・検査で落ちる時、ランタイムは窓を開く前に止まる(T-0157。GPU を使わない)
add_test(NAME reaction_package_runtime_stops
         COMMAND bicameral --world sample --frames 1 --packages ${CMAKE_CURRENT_SOURCE_DIR}/packages_broken)
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
set_tests_properties(gas_reference PROPERTIES TIMEOUT 1800)  # debug で約 12 分(T-0226 で小刻みが 6 倍)
add_executable(gas_projection_test gas_projection_test.cpp)  # 気体の圧力を刻みの中で解く投影の CPU 模型(T-0503 研究)
target_link_libraries(gas_projection_test PRIVATE bicameral_gas_projection bicameral_core)
add_test(NAME gas_projection COMMAND gas_projection_test)
set_tests_properties(gas_projection PROPERTIES TIMEOUT 1800)
add_test(NAME gas_projection_plates COMMAND gas_projection_test --plates)  # T-0788: 最速 × 小さい空きの板の警告の閾値の根拠
set_tests_properties(gas_projection_plates PROPERTIES TIMEOUT 1800)
# 案 B の試作の箱(B0。T-0866)と場面・計器(B1。T-0867)。docs/design/21 §5.2(CPU)
# 表面の細かい形(研究 R-T1・T-0879。23 §7・§10): 当たり・開き具合の費用・整数と小数のずれ・溝に水が流れる・差分のセーブ
add_executable(surface_detail_test surface_detail_test.cpp)
target_precompile_headers(surface_detail_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(surface_detail_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME surface_detail COMMAND surface_detail_test)
set_tests_properties(surface_detail PROPERTIES TIMEOUT 900)
add_executable(face_opening_test face_opening_test.cpp)  # セルの面の開き具合の欄を液体が読む・溝に沿って流れる(T-0937)
target_precompile_headers(face_opening_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(face_opening_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME face_opening COMMAND face_opening_test)
set_tests_properties(face_opening PROPERTIES TIMEOUT 900)

add_executable(case_b_box_test case_b_box_test.cpp)
target_link_libraries(case_b_box_test PRIVATE bicameral_case_b bicameral_core)
add_test(NAME case_b_box COMMAND case_b_box_test)
add_test(NAME case_b_gauge COMMAND case_b_box_test --gauge)
set_tests_properties(case_b_box case_b_gauge PROPERTIES TIMEOUT 600)
# 釣り合う家族の共通部品(T-0853): 陰の圧力を部品の上へ移して T-0845 の CPU の答えと毎刻みビット一致・保存・決定性・閉じた領域
add_executable(equilibrium_test equilibrium_test.cpp)
target_link_libraries(equilibrium_test PRIVATE bicameral_gas_projection bicameral_core)
add_test(NAME equilibrium COMMAND equilibrium_test)
set_tests_properties(equilibrium PROPERTIES TIMEOUT 3000)

# 部品の温めた始まりを C-1 の欄に置く: 保存点から続けてハッシュ一致・セーブの大きさと費用(T-0922・ADR-0427。CPU だけ)
add_executable(warm_start_save_test warm_start_save_test.cpp)
target_link_libraries(warm_start_save_test PRIVATE bicameral_gas_projection bicameral_core)
add_test(NAME warm_start_save COMMAND warm_start_save_test)
set_tests_properties(warm_start_save PROPERTIES TIMEOUT 1800)

# 支えの法則の試作(研究 R-B1・T-0871。C-0 の使い手。CPU だけ)
add_executable(support_law_test support_law_test.cpp)
target_link_libraries(support_law_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME support_law COMMAND support_law_test)
set_tests_properties(support_law PROPERTIES TIMEOUT 3000)
# 案 B の研究 R1(T-0868): 硬い液体を気体と同じ陰の圧力で。1 本目(本物の水)と液体の段 (2) 柔らかく・(1) 回数を増やす を同じ試験で測る
add_executable(case_b_r1_test case_b_r1_test.cpp)
target_link_libraries(case_b_r1_test PRIVATE bicameral_case_b bicameral_core)
add_test(NAME case_b_r1 COMMAND case_b_r1_test)
set_tests_properties(case_b_r1 PROPERTIES TIMEOUT 1800)

# 案 B の研究 R2(T-0869): 引きずりを陰の圧力と一緒に解く(水中の砂・空気中の粉・Darcy・1 mg)。CPU だけ・研究の記録
add_executable(case_b_r2_test case_b_r2_test.cpp)
target_link_libraries(case_b_r2_test PRIVATE bicameral_case_b bicameral_core)
add_test(NAME case_b_r2 COMMAND case_b_r2_test)
set_tests_properties(case_b_r2 PROPERTIES TIMEOUT 1800)
# 引きずりの一般の形の模型(D-487 ④・T-0493): 交差の引きずり(結合の強さ q)と、気体の箱の中の燃える木箱で煙が熱を運ぶ(CPU)
add_executable(drag_model_test drag_model_test.cpp)
target_link_libraries(drag_model_test PRIVATE bicameral_gas bicameral_reaction bicameral_core)
add_test(NAME drag_model COMMAND drag_model_test)
set_tests_properties(drag_model PROPERTIES TIMEOUT 1800)
# 坂を上らせる構造(押す)の一般の形の模型(D-487 ⑤・T-0495): ポンプ・発電機・熱機関を同じ交差の形(結合の強さ q)で(CPU)
add_executable(coupling_table_test coupling_table_test.cpp)  # 分類 drags・machines の読み手・ベイク(|q| の押さえ)・ホットリロード(T-0536)
target_link_libraries(coupling_table_test PRIVATE bicameral_reaction_script bicameral_core)
add_dependencies(coupling_table_test bicameral_data)  # 型の定義は exe の横の data/types を読む
add_test(NAME coupling_table COMMAND coupling_table_test)
set_tests_properties(coupling_table PROPERTIES TIMEOUT 300)

add_executable(push_model_test push_model_test.cpp)
target_link_libraries(push_model_test PRIVATE bicameral_reaction bicameral_fixed)
add_test(NAME push_model COMMAND push_model_test)
set_tests_properties(push_model PROPERTIES TIMEOUT 600)
add_executable(gas_tree_test gas_tree_test.cpp)  # 気体の多重解像度の木の CPU リファレンス(T-0208)
target_link_libraries(gas_tree_test PRIVATE bicameral_gas bicameral_core)
add_test(NAME gas_tree COMMAND gas_tree_test)
set_tests_properties(gas_tree PROPERTIES TIMEOUT 1800)

add_executable(multires_test multires_test.cpp)
target_link_libraries(multires_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires COMMAND multires_test)
add_executable(multires_state_test multires_state_test.cpp)  # 状態の登録・欄・ハッシュと保存点が登録を回る(T-0711)
target_link_libraries(multires_state_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires_state COMMAND multires_state_test)
add_executable(field_storage_test field_storage_test.cpp)  # 欄の持ち方: 真実 / 導かれる値 / 温めた始まり・眠らせて起こす往復(T-0854)
target_link_libraries(field_storage_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME field_storage COMMAND field_storage_test)
# 研究 R-T2(T-0856): 刻みの長さを状態が変わる速さで選ぶ CPU の試作(捨てる試作なのでエンジンのライブラリに入れず、試験に直接入れる)
add_executable(time_step_lab_test time_step_lab_test.cpp ${PROJECT_SOURCE_DIR}/engine/src/sim/time_step_lab.cpp)
target_link_libraries(time_step_lab_test PRIVATE bicameral_fixed bicameral_core)
add_test(NAME time_step_lab COMMAND time_step_lab_test)
set_tests_properties(time_step_lab PROPERTIES TIMEOUT 1800)
add_executable(multires_sparse_test multires_sparse_test.cpp)  # 欄をある所にだけ持つ(疎な頁)と密で毎刻みビット一致・1 セルのバイト(T-0921)
target_precompile_headers(multires_sparse_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/src/pch.h)  # probe_sim.h が D3D12 の型を使う
target_link_libraries(multires_sparse_test PRIVATE bicameral_test_reaction_table bicameral_sim bicameral_core)
add_test(NAME multires_sparse COMMAND multires_sparse_test)
add_executable(multires_wide_test multires_wide_test.cpp)  # 溢れを使う世界の影・畳む・端数・静かな葉(T-0199)
target_link_libraries(multires_wide_test PRIVATE bicameral_multires bicameral_core)
add_test(NAME multires_wide COMMAND multires_wide_test)

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

add_executable(multires_edge_test multires_edge_test.cpp)  # 世界の端(open / wall・外のセル・帳簿。T-0721。環境から作る端 T-0772)
target_link_libraries(multires_edge_test PRIVATE bicameral_reaction_script bicameral_sim bicameral_multires bicameral_core)
target_compile_definitions(multires_edge_test PRIVATE
  BICAMERAL_DATA_STANDARD_DIR="${PROJECT_SOURCE_DIR}/data/standard"
  BICAMERAL_ATMOSPHERE_PACKAGES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/packages_atmosphere")
add_dependencies(multires_edge_test bicameral_data)  # 型の定義は exe の横の data/types を読む
add_test(NAME multires_edge COMMAND multires_edge_test)

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
add_test(NAME physics_gravity COMMAND physics_test gravity)  # 重力を変えた世界(無重力・月・上限。T-0423)
add_test(NAME physics_energy COMMAND physics_test energy)  # 物理で失った運動エネルギーの帳簿(T-0519)
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
  add_test(NAME physics_gravity_checked COMMAND physics_checked_test gravity)  # 重力の上限で桁あふれしない(T-0423)
  add_test(NAME physics_energy_checked COMMAND physics_checked_test energy)  # 散逸の帳簿の数えが桁あふれしない(T-0519)
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
bicameral_add_gpu_test_case(gpu_reaction_gas_room gpu_reaction_test gpu --queue compute --gas-room)  # セルの気体の器(T-0622)
bicameral_add_gpu_test_case(gpu_reaction_gas_room_warp gpu_reaction_test warp --warp --queue compute --gas-room)

bicameral_add_gpu_test(gpu_multires_test)  # 多重解像度の入れ子の GPU と CPU のビット一致(T-0017)
target_link_libraries(gpu_multires_test PRIVATE bicameral_sim)
# HW は鎖の場面の 3 つの組(gpu_multires・_stress・_limits)と溢れの場面の 6 つの組(gpu_multires_wide_*)に分ける: debug の GPU-based validation で 1 本では 1200 s に届いた
# (T-0124・T-0263)。debug は 2 回目の実行と計測も省く(鎖の場面 約 400 s)。溢れの場面は GBV を切る(debug layer と FX_ASSERT は残る):
# GBV ありの debug では 1 場面 210〜290 s × 7 で 1200 s に入らない・切ると 7 場面で約 80 s(2026-10-11。release は元から GBV なし)。
# 溢れの場面を 1 本(gpu_multires_wide)で流すと 285〜1148 s かかり、GPU の順番待ち(D-460)で他のランナーを長く止めたので、
# 1 本を 10 分に収めるよう 6 つの組(1 組 3 場面まで)に分けた(T-0426。組の中身は gpu_multires_test の RunWideScenes の上。
# CPU リファレンスとの比べ方は変えていない)。重かったのは場面ごとのパイプライン(Compute 35 本 + Work Graph)の作成: ctest から起動した
# プロセスはドライバのシェーダーのキャッシュに当たらず(原因は ctest ではなく、test.ps1 の VS の開発者環境が化けさせた環境変数。T-0455 で直した)、作るたびにコンパイルしていた
# (debug 1 回 約 65 s)。試験の中でパイプラインを使い回す(gpu::PipelineCache。--no-pipeline-cache で毎回作る。T-0427)。上限は 900 s のまま
# 鎖の場面も 3 つの組に分ける(debug の HW で 1 本 441〜777 s だった。T-0426): 本物の鎖・影の鎖(release は計測も)/ たくさんの要求 /
# 粗くすると入りきらない子・上限(溢れなし)
bicameral_add_gpu_test_case(gpu_multires gpu_multires_test gpu --queue compute --part chains-real)
bicameral_add_gpu_test_case(gpu_multires_stress gpu_multires_test gpu --queue compute --part chains-stress)
bicameral_add_gpu_test_case(gpu_multires_limits gpu_multires_test gpu --queue compute --part chains-limits)
set_tests_properties(gpu_multires gpu_multires_stress gpu_multires_limits PROPERTIES TIMEOUT 900)
foreach(_part limits overflow many many_conduct coarsen uniform)
  string(REPLACE "_" "-" _arg "wide-${_part}")
  bicameral_add_gpu_test_case(gpu_multires_wide_${_part} gpu_multires_test gpu --queue compute --part ${_arg} --no-gbv)
  set_tests_properties(gpu_multires_wide_${_part} PROPERTIES TIMEOUT 900)
endforeach()
# WARP は exe の横の NuGet の版(T-0097)で動く。OS の WARP では Work Graph を作る所で落ちる(ADR-0013)。鎖と溢れの 2 本(T-0426)
bicameral_add_gpu_test_case(gpu_multires_warp gpu_multires_test warp --warp --queue compute --part chains)
bicameral_add_gpu_test_case(gpu_multires_wide_warp gpu_multires_test warp --warp --queue compute --part wide)
set_tests_properties(gpu_multires_warp gpu_multires_wide_warp PROPERTIES TIMEOUT 900)

bicameral_add_gpu_test(gpu_multires_implicit_test)  # 細かいレベルの熱の陰解法の GPU と CPU のビット一致(T-0117)
target_link_libraries(gpu_multires_implicit_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_implicit gpu_multires_implicit_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_implicit_warp gpu_multires_implicit_test warp --warp --queue compute)
set_tests_properties(gpu_multires_implicit gpu_multires_implicit_warp PROPERTIES TIMEOUT 900)
bicameral_add_gpu_test(gpu_multires_implicit_tree_test)  # 木につないだ陰解法の系を GPU で解いて CPU とビット一致(T-0127)
target_link_libraries(gpu_multires_implicit_tree_test PRIVATE bicameral_sim)
# HW は場面で 2 本に(1 本 270〜894 s だった。T-0426): 熱い点・鎖 / たくさんの要求。WARP は 1 本 120〜305 s なので 1 本のまま
bicameral_add_gpu_test_case(gpu_multires_implicit_tree gpu_multires_implicit_tree_test gpu --queue compute --scene hot,chain)
bicameral_add_gpu_test_case(gpu_multires_implicit_tree_stress gpu_multires_implicit_tree_test gpu --queue compute --scene stress)
bicameral_add_gpu_test_case(gpu_multires_implicit_tree_warp gpu_multires_implicit_tree_test warp --warp --queue compute)
set_tests_properties(gpu_multires_implicit_tree gpu_multires_implicit_tree_stress gpu_multires_implicit_tree_warp
  PROPERTIES TIMEOUT 900)
bicameral_add_gpu_test(gpu_multires_implicit_build_test)  # 陰解法の系(未知数・境のセル・面)を GPU で作って CPU とビット一致(T-0129)
target_link_libraries(gpu_multires_implicit_build_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_implicit_build gpu_multires_implicit_build_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_implicit_build_warp gpu_multires_implicit_build_test warp --warp --queue compute)
set_tests_properties(gpu_multires_implicit_build gpu_multires_implicit_build_warp PROPERTIES TIMEOUT 1200)
bicameral_add_gpu_test(gpu_multires_implicit_conduction_test)  # GPU の伝導の段から陰解法を呼んで全部・活性の刻みで CPU とビット一致(T-0132)
target_link_libraries(gpu_multires_implicit_conduction_test PRIVATE bicameral_sim)
# 上限の形(--squeeze)と場面(--scenes)で分ける(1 本を GPU の順番待ち〔D-460〕に合う 10 分に収める。T-0426): 1 本では release で
# 690〜1112 s。形ごとでも debug の HW は 1 つの形で 900 s を超え・WARP は 2 つの形で 606〜745 s(2026-10-13)。
# debug の HW は 1 場面(全部・活性)で 300〜340 s かかる(刻みの GPU の仕事。作成は 1 回 約 40 s)ので、HW は形 × 場面の 12 本
# (名前の後ろなし = 熱い点・_chain・_stress)、WARP は形ごとの 4 本。計測(release の HW)は none の 3 本だけ(その場面だけ)
foreach(_squeeze none blocks levels cheapest)
  if(_squeeze STREQUAL "none")
    set(_name gpu_multires_implicit_conduction)
  else()
    set(_name gpu_multires_implicit_conduction_${_squeeze})
  endif()
  bicameral_add_gpu_test_case(${_name} gpu_multires_implicit_conduction_test gpu --queue compute --squeeze ${_squeeze}
    --scenes hot)
  bicameral_add_gpu_test_case(${_name}_chain gpu_multires_implicit_conduction_test gpu --queue compute --squeeze ${_squeeze}
    --scenes chain)
  bicameral_add_gpu_test_case(${_name}_stress gpu_multires_implicit_conduction_test gpu --queue compute --squeeze ${_squeeze}
    --scenes stress)
  bicameral_add_gpu_test_case(${_name}_warp gpu_multires_implicit_conduction_test warp --warp --queue compute --squeeze ${_squeeze})
  set_tests_properties(${_name} ${_name}_chain ${_name}_stress ${_name}_warp PROPERTIES TIMEOUT 900)
endforeach()

bicameral_add_gpu_test(gpu_multires_activity_test)  # 木の上の活性の GPU と CPU のビット一致(T-0100)
target_link_libraries(gpu_multires_activity_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_activity gpu_multires_activity_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_activity_warp gpu_multires_activity_test warp --warp --queue compute)

bicameral_add_gpu_test(gpu_multires_quiet_test)  # 静かなブロックを粗くする要求の GPU と CPU のビット一致(T-0101)
target_link_libraries(gpu_multires_quiet_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_multires_quiet gpu_multires_quiet_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_multires_quiet_warp gpu_multires_quiet_test warp --warp --queue compute)

bicameral_add_gpu_test(gpu_sun_column_test)  # 太陽の影の柱の GPU と CPU のビット一致・費用(T-0910)
target_link_libraries(gpu_sun_column_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_sun_column gpu_sun_column_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_sun_column_warp gpu_sun_column_test warp --warp --queue compute)
set_tests_properties(gpu_sun_column gpu_sun_column_warp PROPERTIES TIMEOUT 900)

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
# 細かいレベルの熱の刻み(T-0109)。CPU の小刻み 64 回が重いので、鎖とたくさんの要求の 2 本に分ける(1 本 621〜1163 s だった。T-0426)
bicameral_add_gpu_test_case(gpu_multires_subcycle gpu_multires_conduction_test gpu --queue compute --subcycle --scenes chain)
bicameral_add_gpu_test_case(gpu_multires_subcycle_stress gpu_multires_conduction_test gpu --queue compute --subcycle --scenes stress)
bicameral_add_gpu_test_case(gpu_multires_subcycle_warp gpu_multires_conduction_test warp --warp --queue compute --subcycle --scenes chain)
bicameral_add_gpu_test_case(gpu_multires_subcycle_stress_warp gpu_multires_conduction_test warp --warp --queue compute --subcycle
  --scenes stress)
set_tests_properties(gpu_multires_subcycle gpu_multires_subcycle_stress gpu_multires_subcycle_warp
  gpu_multires_subcycle_stress_warp PROPERTIES TIMEOUT 1200)
bicameral_add_gpu_test_case(gpu_multires_near_fold gpu_multires_conduction_test gpu --queue compute --near-fold)  # 許容差つきで畳む(T-0112)
bicameral_add_gpu_test_case(gpu_multires_near_fold_warp gpu_multires_conduction_test warp --warp --queue compute --near-fold)
set_tests_properties(gpu_multires_near_fold gpu_multires_near_fold_warp PROPERTIES TIMEOUT 2400)  # 静かになるまで約 2000 刻み

bicameral_add_gpu_test(gpu_lab_box_test)  # 実験室の箱の GPU と CPU のビット一致・記録と再生(T-0142)
target_link_libraries(gpu_lab_box_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_lab_box gpu_lab_box_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_lab_box_warp gpu_lab_box_test warp --warp --queue compute)
set_tests_properties(gpu_lab_box gpu_lab_box_warp PROPERTIES TIMEOUT 900)  # 並走中は debug のパイプラインの作成に数分

bicameral_add_gpu_test(gpu_gas_test)  # 気体の流れの GPU と CPU の毎刻みのビット一致・G1・G2 の合格の条件(T-0185)
target_link_libraries(gpu_gas_test PRIVATE bicameral_sim)
# GPU の順番待ち(D-460)に合わせて場面を 3 組に分ける(T-0426。1 本 10 分以内。比べ方は同じ)。release の WARP の目安(2026-10-12):
# 箱の組(閉じた箱・泡・重さの違い)約 390 秒・流れの組(風・管・爆発・熱いセル〔T-0269〕・静止大気の重さの違い)約 360 秒(2026-10-13 の T-0269 の後: 箱 513 秒・流れ 359 秒。負荷あり)。
# 流れの組は debug の WARP で 630 秒(2026-10-14。環境の変化〔T-0444〕の場面が増えた)なので、風・管・音・衝撃波と、爆発・熱いセル・静止大気・開いた環境の変化に分けた
set(GPU_GAS_BOX_SCENES closed,periodic,single_layer,static,bubble,closed_zero_gravity,closed_moon,closed_max_gravity,bubble_zero_gravity,closed_environment_change)
set(GPU_GAS_FLOW_SCENES wind_mc,wind_upwind,wind_minmod_floor,trace,sound,shock,shock_isothermal)
set(GPU_GAS_BLAST_SCENES blast,hot_cell,blast_hot,static_moon,static_max_gravity,open_environment_change)
set(GPU_GAS_SPECIES_SCENES six_wind,six_blast,viscous_closed,viscous_wind,viscous_blast)  # 成分 6 つ(T-0598。GPU の成分の欄に上限なし)・粘り(T-0775)
bicameral_add_gpu_test_case(gpu_gas gpu_gas_test gpu --queue compute --scene ${GPU_GAS_BOX_SCENES})
bicameral_add_gpu_test_case(gpu_gas_flow gpu_gas_test gpu --queue compute --scene ${GPU_GAS_FLOW_SCENES})
bicameral_add_gpu_test_case(gpu_gas_blast gpu_gas_test gpu --queue compute --scene ${GPU_GAS_BLAST_SCENES})
bicameral_add_gpu_test_case(gpu_gas_warp gpu_gas_test warp --warp --queue compute --scene ${GPU_GAS_BOX_SCENES})
bicameral_add_gpu_test_case(gpu_gas_flow_warp gpu_gas_test warp --warp --queue compute --scene ${GPU_GAS_FLOW_SCENES})
bicameral_add_gpu_test_case(gpu_gas_blast_warp gpu_gas_test warp --warp --queue compute --scene ${GPU_GAS_BLAST_SCENES})
bicameral_add_gpu_test_case(gpu_gas_species gpu_gas_test gpu --queue compute --scene ${GPU_GAS_SPECIES_SCENES})
bicameral_add_gpu_test_case(gpu_gas_species_warp gpu_gas_test warp --warp --queue compute --scene ${GPU_GAS_SPECIES_SCENES})
set_tests_properties(gpu_gas gpu_gas_flow gpu_gas_blast gpu_gas_species gpu_gas_warp gpu_gas_flow_warp gpu_gas_blast_warp gpu_gas_species_warp
  PROPERTIES TIMEOUT 900)  # 毎刻み読み戻して CPU と比べる(debug の CPU リファレンスが重い)
bicameral_add_gpu_test(gpu_gas_tree_test)  # 気体の多重解像度の木の GPU と CPU(gas_tree)の毎刻みのビット一致(T-0209)
target_link_libraries(gpu_gas_tree_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_gas_tree gpu_gas_tree_test gpu --queue compute)
bicameral_add_gpu_test_case(gpu_gas_tree_warp gpu_gas_tree_test warp --warp --queue compute)
set_tests_properties(gpu_gas_tree gpu_gas_tree_warp PROPERTIES TIMEOUT 1800)  # 毎刻み読み戻して CPU と比べる(debug の CPU リファレンスが重い)

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
# 重力を変えた世界(T-0423・D-480): 無重力(積んだまま動かない)・月・上限 100 m/s² で GPU と CPU がビット一致
bicameral_add_gpu_test_case(gpu_physics_stack_zero_gravity gpu_physics_test gpu --scene stack --gravity 0 --ticks 300)
bicameral_add_gpu_test_case(gpu_physics_pile_moon gpu_physics_test gpu --scene pile --gravity 1620000)
# 環境の値のホットリロード(T-0444): 落ちている途中で無重力 → 月 → 上限 100 m/s² に替えても GPU と CPU がビット一致
bicameral_add_gpu_test_case(gpu_physics_pile_gravity_change gpu_physics_test gpu --scene pile
                            --gravity-change 40:0,80:1620000,120:100000000 --ticks 180)
bicameral_add_gpu_test_case(gpu_physics_stack_max_gravity gpu_physics_test gpu --scene stack --gravity 100000000 --ticks 600)

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
# GPU の順番待ち(D-460)に合わせて 2 本ずつに分ける(T-0426。debug で 1 本 10 分を超えた): 分け方・イベント・記録と再生・遅い起こし /
# 表の差し替え・物質の付け替え・反応の上限(_tables)
bicameral_add_gpu_test_case(gpu_probe_sim gpu_probe_sim_test gpu --no-gbv --part framings)
bicameral_add_gpu_test_case(gpu_probe_sim_tables gpu_probe_sim_test gpu --no-gbv --part tables)
bicameral_add_gpu_test_case(gpu_probe_sim_warp gpu_probe_sim_test warp --warp --part framings)
bicameral_add_gpu_test_case(gpu_probe_sim_tables_warp gpu_probe_sim_test warp --warp --part tables)
# 世界に物を置く筆(T-0222): 置く・足す・はみ出す・当たらない値のコマンドを流して CPU と毎刻みビット一致(分け方を変えても)
bicameral_add_gpu_test(gpu_probe_place_test)
target_link_libraries(gpu_probe_place_test PRIVATE bicameral_sim bicameral_save)
bicameral_add_gpu_test_case(gpu_probe_place gpu_probe_place_test gpu --no-gbv)
bicameral_add_gpu_test_case(gpu_probe_place_warp gpu_probe_place_test warp --warp)
# 伝導の連鎖のトレース(T-0087): 2 回・分け方を変えて同じ、CPU リファレンスの予想と一致、わざとずらして最初の刻み・ブロック・セル
bicameral_add_gpu_test(gpu_probe_trace_test)
target_link_libraries(gpu_probe_trace_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_trace gpu_probe_trace_test gpu --no-gbv)
bicameral_add_gpu_test_case(gpu_probe_trace_warp gpu_probe_trace_test warp --warp)
# WARP の仮の世界は重い(2026-10-03 の PC の debug で sim 130 s・trace 65 s)。CI の機械(コアが少ない)で 300 s を越えないよう長めに切る
set_tests_properties(gpu_probe_sim gpu_probe_sim_tables gpu_probe_sim_warp gpu_probe_sim_tables_warp gpu_probe_trace_warp PROPERTIES TIMEOUT 900)  # gpu_probe_sim は debug で 300 秒を超える(T-0139 で表の差し替えを足した)
# 覗き窓(T-0096): 影の鎖と抽出の覗きの欄が CPU とビット一致・覗いても世界のハッシュ列が変わらない
bicameral_add_gpu_test(gpu_probe_physics_test)  # 仮の世界の刻みに入れた物理と押すコマンドの GPU と CPU のビット一致(T-0098)
target_link_libraries(gpu_probe_physics_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_physics gpu_probe_physics_test gpu)
bicameral_add_gpu_test_case(gpu_probe_physics_warp gpu_probe_physics_test warp --warp --ticks 120)
bicameral_add_gpu_test_case(gpu_probe_physics_compute gpu_probe_physics_test gpu --broadphase-compute)
# 環境の値のホットリロード(T-0444): 表の差し替えの刻みから物理の重力が替わり、CPU の物理(同じ刻みに SetGravity)とビット一致
bicameral_add_gpu_test_case(gpu_probe_physics_gravity_swap gpu_probe_physics_test gpu --gravity-swap)

# 巻き戻し(T-0143): 保存点へ写して戻し、コマンドを足し直すと最初から流した時と同じハッシュ列
bicameral_add_gpu_test(gpu_probe_rewind_test)
target_link_libraries(gpu_probe_rewind_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_rewind gpu_probe_rewind_test gpu)
# 物理のパイプラインの作成が、ほかの作業ツリーと GPU を分けている間は debug で 4 分を超えることがある
set_tests_properties(gpu_probe_rewind PROPERTIES TIMEOUT 900)
# 区画のある世界(T-0620): 気体と液体の区画の持ち越す欄も保存点が写し、状態のハッシュに入る
bicameral_add_gpu_test_case(gpu_probe_rewind_regions gpu_probe_rewind_test gpu --regions)
set_tests_properties(gpu_probe_rewind_regions PROPERTIES TIMEOUT 900)

# 世界の写しの早送り(T-0412・ADR-0198): 写しを取っても本物のハッシュ列が変わらない・同じコマンドなら写し = 本物 = CPU リファレンス・
# 使い直した写しが条件(木の量)で CPU と同じ刻みに止まる
bicameral_add_gpu_test(gpu_world_copy_test)
target_link_libraries(gpu_world_copy_test PRIVATE bicameral_sim bicameral_frame)
bicameral_add_gpu_test_case(gpu_world_copy gpu_world_copy_test gpu)
set_tests_properties(gpu_world_copy PROPERTIES TIMEOUT 900)
bicameral_add_gpu_test(gpu_play_metric_test)  # 遊びの指標を写しの早送りで測る(T-0432・ADR-0206)
target_link_libraries(gpu_play_metric_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_play_metric gpu_play_metric_test gpu)
set_tests_properties(gpu_play_metric PROPERTIES TIMEOUT 900)

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
# 仮の世界の GPU の刻みに気体の区画(T-0592): 静かな大気と燃える木箱が CPU リファレンス(probe_gas)と毎刻みビット一致
bicameral_add_gpu_test(gpu_probe_gas_test)
target_link_libraries(gpu_probe_gas_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_gas gpu_probe_gas_test gpu --no-gbv)
bicameral_add_gpu_test_case(gpu_probe_gas_warp gpu_probe_gas_test warp --warp)
# 仮の世界の GPU の刻みに液体の区画(T-0613): 池に溜まる・堤を消すと抜ける・ダムの水が押し寄せる・移流なしが CPU リファレンス(probe_liquid)と毎刻みビット一致
# 表面の細かい形の整数の関数を GPU で(R-T1・T-0879): 当たりと開き具合が CPU と 1 値ずつビット一致・費用
bicameral_add_gpu_test(gpu_surface_detail_test)
target_link_libraries(gpu_surface_detail_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_surface_detail gpu_surface_detail_test gpu)
bicameral_add_gpu_test_case(gpu_surface_detail_warp gpu_surface_detail_test warp --warp)

bicameral_add_gpu_test(gpu_probe_liquid_test)
target_link_libraries(gpu_probe_liquid_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_probe_liquid gpu_probe_liquid_test gpu --no-gbv)
bicameral_add_gpu_test_case(gpu_probe_liquid_warp gpu_probe_liquid_test warp --warp)
# ブランクの世界(T-0371・D-455): 空の表・物の無い世界の GPU の刻みが CPU とハッシュ一致し、刻み 0 のまま変わらない
bicameral_add_gpu_test(gpu_probe_blank_test)
target_link_libraries(gpu_probe_blank_test PRIVATE bicameral_sim bicameral_reaction_script)
add_dependencies(gpu_probe_blank_test bicameral_data)
# HW は gpu_probe_sim と同じく GBV を切る(--no-gbv): GBV ありでは 15 分以上返らなかった(計装が遅いだけ。--no-gbv は 27 秒で通る)
bicameral_add_gpu_test_case(gpu_probe_blank gpu_probe_blank_test gpu --no-gbv)
bicameral_add_gpu_test_case(gpu_probe_blank_warp gpu_probe_blank_test warp --warp)

# 伝導の Work Graph の時間を伝播の規模ごとに測る(T-0005。ctest には登録しない。docs/perf.md)。
# 走らせ方: job.py run -Preset release -Exe gpu_conduct_bench
bicameral_add_gpu_test(gpu_conduct_bench)
target_link_libraries(gpu_conduct_bench PRIVATE bicameral_sim)

# 一様な箱の陰解法の熱を、箱の大きさ・伝わる速さごとに測る(T-0494。D-467。ctest には登録しない。docs/perf.md)。
# 走らせ方: job.py run -Preset release -Exe gpu_implicit_box_bench -- --no-gbv
bicameral_add_gpu_test(gpu_implicit_box_bench)
target_link_libraries(gpu_implicit_box_bench PRIVATE bicameral_sim)

# 気体の圧力の投影(T-0531)の GPU 版と CPU の模型(gas_projection)のビット一致。今は解く部分(GpuImplicit::RecordSolve)。
# 成分を持つ節の釣り合いの GPU 版と CPU のビット一致(T-0970。支えの場面・k = 1・2・3・6 の箱・逆順)。
# 計測は job.py run -Preset release -Exe gpu_equilibrium_block_test -- --no-gbv --measure
bicameral_add_gpu_test(gpu_equilibrium_block_test)
target_link_libraries(gpu_equilibrium_block_test PRIVATE bicameral_sim)
bicameral_add_gpu_test_case(gpu_equilibrium_block gpu_equilibrium_block_test gpu --queue compute --no-gbv)
set_tests_properties(gpu_equilibrium_block PROPERTIES TIMEOUT 900)

# 計測は job.py run -Preset release -Exe gpu_gas_projection_test -- --no-gbv --measure --edge 32
bicameral_add_gpu_test(gpu_gas_projection_test)
target_link_libraries(gpu_gas_projection_test PRIVATE bicameral_sim bicameral_gas_projection)
bicameral_add_gpu_test_case(gpu_gas_projection gpu_gas_projection_test gpu --queue compute)
# 釣り合う家族の部品の GPU 版(T-0915): CPU の部品と 32³ × 5 場面 × 300 刻み毎刻みビット一致・前の GPU の道のハッシュ・保存・決定性
bicameral_add_gpu_test_case(gpu_gas_projection_t0915 gpu_gas_projection_test gpu --queue compute --no-gbv --t0915 --ticks 300)
set_tests_properties(gpu_gas_projection_t0915 PROPERTIES TIMEOUT 1800)

# 置くコマンドが 1 刻みにたくさん来た時の適用の単位の時間(T-0280。ctest には登録しない。docs/perf.md)。
# 走らせ方: job.py run -Preset release -Exe gpu_place_bench
bicameral_add_gpu_test(gpu_place_bench)
target_link_libraries(gpu_place_bench PRIVATE bicameral_sim)

# 窓の操作の記録 → 再生(T-0086、06「テスト」の 2 つ目・15 §2)。ランタイム(bicameral)を 2 回走らせる:
#   1 回目: 自動クリックで窓の操作を入れて記録する。2 回目: 別の分け方(重さを分けた単位・目標 fps)で再生し、刻みごとのハッシュが全部一致すれば 0。
# 窓を開くのでハードウェアの GPU と画面が要る(ラベル gpu。CI は外す)。
set(WINDOW_REPLAY_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_replay.bcreplay)
add_test(NAME window_replay_record
         COMMAND bicameral --world sample --frames 300 --auto-click --auto-push --record ${WINDOW_REPLAY_FILE})
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
# 巻き戻し(T-0143): 40 刻みごとに保存点へ写しながら再生し、保存点が 2 つできたら古い方(刻み 0 の保存点。T-0411)へ戻して再生し直しても、
# 戻した後のハッシュ列が記録と同じ(保存点 + 再生。ADR-0036)。戻せなかったら 1
add_test(NAME window_replay_rewind
         COMMAND bicameral --replay ${WINDOW_REPLAY_FILE} --editor --auto-rewind --save-interval 40)
set_tests_properties(window_replay_rewind PROPERTIES LABELS gpu FIXTURES_REQUIRED window_replay TIMEOUT 240)
# ブランクの世界(既定。T-0371・D-455): セルは全部空・積み木なし・表は空。窓とエディタが起動し、記録 → 別の起動の再生で
# ハッシュ列が一致する(再生ファイルが世界の種類を持つ。何も起きないので全部の刻みが同じハッシュ)
set(WINDOW_BLANK_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_blank.bcreplay)
add_test(NAME window_blank_record COMMAND bicameral --editor --play --frames 120 --record ${WINDOW_BLANK_FILE})
add_test(NAME window_blank_play COMMAND bicameral --replay ${WINDOW_BLANK_FILE} --target-fps 30)
set_tests_properties(window_blank_record PROPERTIES LABELS gpu FIXTURES_SETUP window_blank TIMEOUT 600
  PASS_REGULAR_EXPRESSION "世界: ブランクの世界")
set_tests_properties(window_blank_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_blank TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生: OK")
# 環境ミキサー(T-0452・D-457): ブランクの世界に標準パッケージ(data/standard)を全部足す → 次の刻みの境界で当たる → 元素を外す
# (依存するものも外れる)→ 初めの表に戻る。記録して、別の起動の再生でハッシュ列と表の差し替えが同じ(再生は棚なしで表を作る)
set(WINDOW_MIXER_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_mixer.bcreplay)
add_test(NAME window_mixer_record COMMAND bicameral --editor --play --auto-mixer --frames 3000 --record ${WINDOW_MIXER_FILE})
add_test(NAME window_mixer_play COMMAND bicameral --replay ${WINDOW_MIXER_FILE} --target-fps 30)
set_tests_properties(window_mixer_record PROPERTIES LABELS gpu FIXTURES_SETUP window_mixer TIMEOUT 600
  PASS_REGULAR_EXPRESSION "--auto-mixer: 外して初めの表")
set_tests_properties(window_mixer_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_mixer TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生: OK")
# 世界を作り直す(T-0453・ADR-0192): ブランクの世界に環境ミキサーで空気で満たす(と依存)を足す → 当たる → 「世界を作り直す」で
# 今の表で刻み 0 から(--mix で起動したのと同じ表)→ 60 刻み。記録は作り直した所からで、別の起動の再生でハッシュ列が同じ
set(WINDOW_REBUILD_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_rebuild.bcreplay)
add_test(NAME window_rebuild_record COMMAND bicameral --editor --play --auto-rebuild --frames 3000 --record ${WINDOW_REBUILD_FILE})
add_test(NAME window_rebuild_play COMMAND bicameral --replay ${WINDOW_REBUILD_FILE} --target-fps 30)
set_tests_properties(window_rebuild_record PROPERTIES LABELS gpu FIXTURES_SETUP window_rebuild TIMEOUT 600
  PASS_REGULAR_EXPRESSION "--auto-rebuild: 作り直した世界.*OK")
set_tests_properties(window_rebuild_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_rebuild TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生: OK.*一致 [1-9]")
# 世界の写しの早送り(T-0412・ADR-0198): 火を付けたサンプルの世界で刻み 60 に写しを取り 600 刻み早送りし、写しの刻みごとのハッシュが
# 本物の同じ刻みと全部同じ(--auto-copy)。記録を写し無しの別の起動で再生して同じハッシュ列 = 写しを取っても本物は変わらない(D-403)
set(WINDOW_COPY_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_copy.bcreplay)
add_test(NAME window_copy_record
         COMMAND bicameral --world sample --editor --play --auto-ignite --auto-copy --frames 20000 --record ${WINDOW_COPY_FILE})
add_test(NAME window_copy_play COMMAND bicameral --replay ${WINDOW_COPY_FILE} --target-fps 30)
set_tests_properties(window_copy_record PROPERTIES LABELS gpu FIXTURES_SETUP window_copy TIMEOUT 600
  PASS_REGULAR_EXPRESSION "--auto-copy: OK")
set_tests_properties(window_copy_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_copy TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生: OK.*一致 [1-9]")
# 止めてある(D-521・ADR-0342): 仮の世界は気体・液体が世界いっぱいで流れ、地球の環境に依る世界の 1 刻み(約 85 ms)がフレームの予算全体を超える。
# 写しは本物の余った予算でだけ進む(D-468)ので 1 刻みも進まず、刻みを減らしても通らない。サンプルの世界の 1 刻みが予算に入ったら戻す
#(仮の世界を退かせる時〔D-513〕)
set_tests_properties(window_copy_record window_copy_play PROPERTIES DISABLED TRUE)
# 重さの見張り(安全網の警告。T-0413・ADR-0204・D-468): 結果に入らない重さ(約 22 ms/刻み)を足すと、本物の刻みが等速の予算を超えた警告が出る
add_test(NAME window_weight_warn COMMAND bicameral --frames 240 --sim-load 5200000 --sim-split 16)
set_tests_properties(window_weight_warn PROPERTIES LABELS gpu TIMEOUT 300
  PASS_REGULAR_EXPRESSION "刻みが等速の予算 [0-9.]+ ms を超えた")

# 遊びの指標(T-0432・ADR-0206): エディタが起動の初めにサンプルの世界の指標を本物の刻みの合間に測り終える(--auto-metrics)。
# 記録を指標無しの別の起動で再生して同じハッシュ列 = 指標を測っても本物は変わらない(D-403)
set(WINDOW_METRICS_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_metrics.bcreplay)
add_test(NAME window_metrics_record
         COMMAND bicameral --world sample --editor --play --auto-ignite --auto-metrics --frames 20000 --record ${WINDOW_METRICS_FILE})
add_test(NAME window_metrics_play COMMAND bicameral --replay ${WINDOW_METRICS_FILE} --target-fps 30)
# 記録の長さは指標の写しが測り終えるまで(予算の余りで回るので PC の混み具合で 75〜300 秒。2026-10-15 に並走のビルド中 301 秒)。
# 再生はその刻みを等速で流すので、上限は記録と同じにする(240 秒で切れた)
set_tests_properties(window_metrics_record PROPERTIES LABELS gpu FIXTURES_SETUP window_metrics TIMEOUT 600
  PASS_REGULAR_EXPRESSION "--auto-metrics: .*OK")
set_tests_properties(window_metrics_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_metrics TIMEOUT 600
  PASS_REGULAR_EXPRESSION "再生: OK.*一致 [1-9]")
# 止めてある(D-521・ADR-0342): 指標の写しも本物の余った予算でだけ進む(D-468)。サンプルの世界の 1 刻み(約 85 ms。気体・液体が世界いっぱい)が
# フレームの予算全体を超えるので進まない。サンプルの世界の 1 刻みが予算に入ったら戻す(仮の世界を退かせる時〔D-513〕)
set_tests_properties(window_metrics_record window_metrics_play PROPERTIES DISABLED TRUE)
# 標準パッケージを足して起動(--mix。T-0373): ブランクの世界に全部を足すと、空気で満たしたセルと地球の重力の世界が刻める
add_test(NAME window_mix_start
         COMMAND bicameral --editor --play --frames 120
                 --mix real_elements,real_matter,real_reactions,earth_environment,earth_air)
set_tests_properties(window_mix_start PROPERTIES LABELS gpu TIMEOUT 600
  PASS_REGULAR_EXPRESSION "反応表: パッケージ .*earth_air")
# ブランクの世界の刻み 0 の保存点(T-0411): 再生しながら刻み 0 と 40 で写し、刻み 0 へ戻して再生し直しても記録と同じハッシュ列
add_test(NAME window_blank_rewind
         COMMAND bicameral --replay ${WINDOW_BLANK_FILE} --editor --auto-rewind --save-interval 40)
set_tests_properties(window_blank_rewind PROPERTIES LABELS gpu FIXTURES_REQUIRED window_blank TIMEOUT 240)
# 実験室(T-0142): パネルの箱に木を置いて火を付け 60 刻み、GPU と CPU が毎刻み一致し、記録から再生しても同じ。通らなければ 1
add_test(NAME window_lab COMMAND bicameral --world sample --editor --play --auto-lab --frames 20)
set_tests_properties(window_lab PROPERTIES LABELS gpu TIMEOUT 900)  # 並走中は debug のパイプラインの作成に数分
# ブランクの世界の実験室(T-0371): 材料の無い表なので空のセルの箱を作って 60 刻み、GPU と CPU が一致・空のまま・再生しても同じ
add_test(NAME window_lab_blank COMMAND bicameral --editor --play --auto-lab --frames 20)
set_tests_properties(window_lab_blank PROPERTIES LABELS gpu TIMEOUT 900)
add_test(NAME window_lab_compare COMMAND bicameral --world sample --editor --play --auto-lab-compare --frames 20)  # 計器と比べる画面(T-0221)
set_tests_properties(window_lab_compare PROPERTIES LABELS gpu TIMEOUT 900)
# 世界に物を置く筆(T-0222): パネルの筆で置く・足す・はみ出す・当たらない値を投げて火を付け、当たるはずのものが全部 GPU で
# 当たったか(--auto-place。通らなければ 1)を記録し、別の起動で再生して同じハッシュ列になる(置くコマンドも再生ファイルだけで再現)。
# 元に戻す(T-0278・ADR-0101): 円柱・直方体のドラッグの運びを 1 つずつ元に戻し(保存点へ戻して除いて流し直す。戻す前と違う状態になる)、
# 記録は元に戻した後の流れ。再生(別の起動・その運びが無い列だけ)が同じハッシュ列 = 元に戻した世界は「置かなかった世界」と同じ。
# 刻みが遅い(debug で 1 フレーム 1 刻み未満)ので、保存点は 2 刻みごとに 8 個
set(WINDOW_PLACE_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_place.bcreplay)
add_test(NAME window_place_record
         COMMAND bicameral --world sample --editor --play --auto-place --save-points 8 --save-interval 2 --frames 120
                 --record ${WINDOW_PLACE_FILE})
add_test(NAME window_place_play COMMAND bicameral --replay ${WINDOW_PLACE_FILE} --target-fps 30)
set_tests_properties(window_place_record PROPERTIES LABELS gpu FIXTURES_SETUP window_place TIMEOUT 600
  PASS_REGULAR_EXPRESSION "--auto-place: .*OK")
set_tests_properties(window_place_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_place TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生: OK")
# 刻み 0 の保存点(T-0411・D-474): 保存点の間隔を十分長くして、保存点が刻み 0 の 1 つだけの時に同じ --auto-place を流す。
# 起動直後(最初の回す保存点より前)の運びを元に戻す = 刻み 0 へ戻してその運びを除いて流し直す。記録 → 別の起動の再生が同じハッシュ列
set(WINDOW_PLACE_TICK0_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_place_tick0.bcreplay)
add_test(NAME window_place_tick0_record
         COMMAND bicameral --world sample --editor --play --auto-place --save-interval 1000000 --frames 120
                 --record ${WINDOW_PLACE_TICK0_FILE})
add_test(NAME window_place_tick0_play COMMAND bicameral --replay ${WINDOW_PLACE_TICK0_FILE} --target-fps 30)
set_tests_properties(window_place_tick0_record PROPERTIES LABELS gpu FIXTURES_SETUP window_place_tick0 TIMEOUT 600
  PASS_REGULAR_EXPRESSION "--auto-place: .*OK")
set_tests_properties(window_place_tick0_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_place_tick0 TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生: OK")
# エディタは UE と同じ形(T-0497・D-491): 起動は止まった編集の状態。置く → 元に戻す・やり直す → プレイ → プレイ中に置く → やめると
# プレイ前の世界に戻る(2 回のプレイのハッシュ列が同じ)。記録(最後の流れ)を別の起動で再生して同じハッシュ列
set(WINDOW_PLAY_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_play.bcreplay)
add_test(NAME window_play_record
         COMMAND bicameral --world sample --editor --auto-play --frames 1200 --record ${WINDOW_PLAY_FILE})
add_test(NAME window_play_play COMMAND bicameral --replay ${WINDOW_PLAY_FILE} --target-fps 30)
set_tests_properties(window_play_record PROPERTIES LABELS gpu FIXTURES_SETUP window_play TIMEOUT 600
  PASS_REGULAR_EXPRESSION "--auto-play: .*OK")
set_tests_properties(window_play_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_play TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生: OK")
# ⑤ の場面(T-0672。T-0724 から区画なし = 気体・液体は世界いっぱいで流れる。ADR-0342)で --auto-play(置く物は世界の真ん中)。
# プレイ前の世界へ戻すのも、2 回のプレイのハッシュ列が同じなのも、区画の欄(保存点・状態のハッシュ。ADR-0300)まで含む。記録を別の起動で再生して同じハッシュ列
set(WINDOW_REGIONS_SCENE ${PROJECT_SOURCE_DIR}/data/scenes/grass_fire_river)
set(WINDOW_REGIONS_PLAY_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_regions_play.bcreplay)
add_test(NAME window_regions_play_record
         COMMAND bicameral --packages ${WINDOW_REGIONS_SCENE} --editor --auto-play --frames 1200 --record ${WINDOW_REGIONS_PLAY_FILE})
add_test(NAME window_regions_play_play COMMAND bicameral --replay ${WINDOW_REGIONS_PLAY_FILE} --target-fps 30)
set_tests_properties(window_regions_play_record PROPERTIES LABELS gpu FIXTURES_SETUP window_regions_play TIMEOUT 600
  PASS_REGULAR_EXPRESSION "--auto-play: .*OK")
set_tests_properties(window_regions_play_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_regions_play TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生: OK")
# ⑤ の場面の写しの早送り(T-0672): 刻み 60 に写しを取り早送りし、写しの刻みごとのハッシュ(気体・液体の欄の寄与を含む)が本物と同じ(--auto-copy)
set(WINDOW_REGIONS_COPY_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_regions_copy.bcreplay)
add_test(NAME window_regions_copy_record
         COMMAND bicameral --packages ${WINDOW_REGIONS_SCENE} --editor --play --auto-ignite --auto-copy --frames 20000
                 --record ${WINDOW_REGIONS_COPY_FILE})
add_test(NAME window_regions_copy_play COMMAND bicameral --replay ${WINDOW_REGIONS_COPY_FILE} --target-fps 30)
set_tests_properties(window_regions_copy_record PROPERTIES LABELS gpu FIXTURES_SETUP window_regions_copy TIMEOUT 600
  PASS_REGULAR_EXPRESSION "--auto-copy: OK")
set_tests_properties(window_regions_copy_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_regions_copy TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生: OK.*一致 [1-9]")
# 止めてある(D-521・ADR-0342): 仮の世界は気体・液体が世界いっぱいで流れ、地球の環境に依る世界の 1 刻み(約 85 ms)がフレームの予算全体を超える。
# 写しは本物の余った予算でだけ進む(D-468)ので 1 刻みも進まず、刻みを減らしても通らない。⑤ の場面の 1 刻みが予算に入ったら戻す
#(多重解像度の世界へ移した後。T-0725〜T-0727)
set_tests_properties(window_regions_copy_record window_regions_copy_play PROPERTIES DISABLED TRUE)
# 表の差し替えを編集として扱う(T-0548・ADR-0246): パッケージの写しの表(物質と重力を足す)を、編集の状態で書き換えると時間を進めずに
# 当たり・元に戻す・やり直すでも残り・プレイをやめるとプレイ前の世界とファイルの今の中身の表に戻る(--auto-play-table。手順は frame/auto_play.h)。
# 記録(最後の流れ。刻み 0 の始めに表の印)を、写しのフォルダが無くても再生ファイルの表だけで再生して同じハッシュ列
set(WINDOW_PLAY_TABLE_PACKAGES ${CMAKE_CURRENT_BINARY_DIR}/play_table_packages)
set(WINDOW_PLAY_TABLE_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_play_table.bcreplay)
add_test(NAME window_play_table_copy
         COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data/packages ${WINDOW_PLAY_TABLE_PACKAGES})
add_test(NAME window_play_table_record
         COMMAND bicameral --world sample --editor --auto-play-table --packages ${WINDOW_PLAY_TABLE_PACKAGES} --frames 2400
                 --record ${WINDOW_PLAY_TABLE_FILE})
add_test(NAME window_play_table_play
         COMMAND bicameral --replay ${WINDOW_PLAY_TABLE_FILE} --packages ${CMAKE_CURRENT_BINARY_DIR}/no_such_packages)
set_tests_properties(window_play_table_copy PROPERTIES FIXTURES_SETUP window_play_table_packages)
set_tests_properties(window_play_table_record PROPERTIES LABELS gpu TIMEOUT 600
  FIXTURES_REQUIRED window_play_table_packages FIXTURES_SETUP window_play_table PASS_REGULAR_EXPRESSION "--auto-play: .*OK")
set_tests_properties(window_play_table_play PROPERTIES LABELS gpu FIXTURES_REQUIRED window_play_table TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生ファイルの表を使う.*再生: OK")
# 覗き窓(T-0096): 覗かずに記録した再生ファイルを、火をつけた木箱の壁を覗いて一番細かい段まで潜りながら再生してもハッシュ列が同じ(D-403)
add_test(NAME window_replay_peek
         COMMAND bicameral --replay ${WINDOW_REPLAY_FILE} --peek 28,32,32 --peek-depth 9 --view slice)
set_tests_properties(window_replay_peek PROPERTIES LABELS gpu FIXTURES_REQUIRED window_replay TIMEOUT 240)
# 表の中身(T-0170・T-0193): 再生ファイルに残った反応表だけで、パッケージのフォルダが無くても同じハッシュ列で再生できる
add_test(NAME window_replay_no_packages
         COMMAND bicameral --replay ${WINDOW_REPLAY_FILE} --packages ${CMAKE_CURRENT_BINARY_DIR}/no_such_packages)
set_tests_properties(window_replay_no_packages PROPERTIES LABELS gpu FIXTURES_REQUIRED window_replay TIMEOUT 240
  PASS_REGULAR_EXPRESSION "再生ファイルの表を使う.*再生: OK")
# ホットリロード(T-0139・T-0194・T-0195): パッケージの写しの反応表を窓の中で壊す → 古い表のまま → 速度を直す → 刻みの境界で
# 差し替わる、を火をつけた木箱の壁を覗きながら記録する(--auto-reload が確かめ、通らなければ 1)。再生は写しのフォルダが無くても
# 再生ファイルの表だけで、同じ刻みに差し替えて同じハッシュ列になる(覗き窓も同じ表へ替わる)。
# --species-remap: 続けて species.luau にオゾンを足す → 消す(物質の数が変わる差し替え。名前で付け替える。T-0242・T-0254)。再生は差し替え 3 回
set(WINDOW_HOT_RELOAD_PACKAGES ${CMAKE_CURRENT_BINARY_DIR}/hot_reload_packages)
set(WINDOW_HOT_RELOAD_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_hot_reload.bcreplay)
# CMake の正規表現は {n} の繰り返しを持たないので、差し替えの行を 3 回並べる
set(WINDOW_HOT_RELOAD_SWAP "反応表を刻み [0-9]+ の始めに版 [0-9a-f]+ へ差し替える")
add_test(NAME window_hot_reload_copy
         COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data/packages ${WINDOW_HOT_RELOAD_PACKAGES})
add_test(NAME window_hot_reload_record
         COMMAND bicameral --world sample --editor --play --auto-reload --species-remap --auto-ignite --packages ${WINDOW_HOT_RELOAD_PACKAGES}
                 --peek 28,32,32 --peek-depth 9 --frames 3000 --record ${WINDOW_HOT_RELOAD_FILE})
add_test(NAME window_hot_reload_play
         COMMAND bicameral --replay ${WINDOW_HOT_RELOAD_FILE} --peek 28,32,32 --peek-depth 9
                 --packages ${CMAKE_CURRENT_BINARY_DIR}/no_such_packages)
set_tests_properties(window_hot_reload_copy PROPERTIES FIXTURES_SETUP window_hot_reload_packages)
set_tests_properties(window_hot_reload_record PROPERTIES LABELS gpu TIMEOUT 600
  FIXTURES_REQUIRED window_hot_reload_packages FIXTURES_SETUP window_hot_reload)
set_tests_properties(window_hot_reload_play PROPERTIES LABELS gpu TIMEOUT 240 FIXTURES_REQUIRED window_hot_reload
  PASS_REGULAR_EXPRESSION "再生ファイルの表を使う.*${WINDOW_HOT_RELOAD_SWAP}.*${WINDOW_HOT_RELOAD_SWAP}.*${WINDOW_HOT_RELOAD_SWAP}.*再生: OK")
# 通常のゲームの起動(--editor なし)でもファイルを見る(T-0243・D-438): 写しの表を壊す → 古い表のまま → 速度を直す → 差し替わる。
# プレイ中に法則が変わった世界は「改造された世界」(D-437)。読み直した表の印は記録の表にも残り、再生の差し替えの行に出る
# --species-remap: 通常の起動でも物質を足す・消す表を名前で付け替えて当てる(既定の Remap。D-438・T-0266)
# --auto-rewind: 最初の差し替えの後に、いちばん古い保存点(刻み 0。差し替えより前)へ 1 回戻す(表も戻り、差し替えの印を足し直して
# また当たる。T-0548)。記録は戻した刻みで切り詰めて書き直すので、再生の一致が「戻した後の世界 = その時の表で刻んだ世界」を確かめる(T-0299)
set(WINDOW_GAME_RELOAD_PACKAGES ${CMAKE_CURRENT_BINARY_DIR}/game_reload_packages)
set(WINDOW_GAME_RELOAD_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_hot_reload_game.bcreplay)
add_test(NAME window_hot_reload_game_copy
         COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data/packages ${WINDOW_GAME_RELOAD_PACKAGES})
add_test(NAME window_hot_reload_game_record
         COMMAND bicameral --world sample --auto-reload --species-remap --auto-ignite --packages ${WINDOW_GAME_RELOAD_PACKAGES} --frames 3000
                 --save-points 4 --save-interval 4 --auto-rewind --record ${WINDOW_GAME_RELOAD_FILE})
add_test(NAME window_hot_reload_game_play
         COMMAND bicameral --replay ${WINDOW_GAME_RELOAD_FILE} --packages ${CMAKE_CURRENT_BINARY_DIR}/no_such_packages)
set_tests_properties(window_hot_reload_game_copy PROPERTIES FIXTURES_SETUP window_hot_reload_game_packages)
set_tests_properties(window_hot_reload_game_record PROPERTIES LABELS gpu TIMEOUT 600
  FIXTURES_REQUIRED window_hot_reload_game_packages FIXTURES_SETUP window_hot_reload_game)
set_tests_properties(window_hot_reload_game_play PROPERTIES LABELS gpu TIMEOUT 240 FIXTURES_REQUIRED window_hot_reload_game
  PASS_REGULAR_EXPRESSION "再生ファイルの表を使う.*反応表を刻み [0-9]+ の始めに版 [0-9a-f]+ へ差し替える\\(改造された世界\\).*再生: OK")
# 環境の値のホットリロード(T-0444・ADR-0194): --auto-reload で速度を直す時に環境の重力も月にする。差し替えの刻みから物理の重力が替わり、
# --check-physics(CPU の物理も同じ刻みに重力を替える)と食い違わない(食い違うと「物理の突き合わせ」の行が出る)。
# 再生は写しのフォルダが無くても再生ファイルの表だけで、同じ刻みに重力が替わって同じハッシュ列になる
set(WINDOW_GRAVITY_RELOAD_PACKAGES ${CMAKE_CURRENT_BINARY_DIR}/gravity_reload_packages)
set(WINDOW_GRAVITY_RELOAD_FILE ${CMAKE_CURRENT_BINARY_DIR}/window_gravity_reload.bcreplay)
add_test(NAME window_gravity_reload_copy
         COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data/packages ${WINDOW_GRAVITY_RELOAD_PACKAGES})
add_test(NAME window_gravity_reload_record
         COMMAND bicameral --world sample --auto-reload --auto-reload-environment --check-physics
                 --packages ${WINDOW_GRAVITY_RELOAD_PACKAGES} --frames 3000 --record ${WINDOW_GRAVITY_RELOAD_FILE})
add_test(NAME window_gravity_reload_play
         COMMAND bicameral --replay ${WINDOW_GRAVITY_RELOAD_FILE} --packages ${CMAKE_CURRENT_BINARY_DIR}/no_such_packages)
set_tests_properties(window_gravity_reload_copy PROPERTIES FIXTURES_SETUP window_gravity_reload_packages)
set_tests_properties(window_gravity_reload_record PROPERTIES LABELS gpu TIMEOUT 600
  FIXTURES_REQUIRED window_gravity_reload_packages FIXTURES_SETUP window_gravity_reload
  PASS_REGULAR_EXPRESSION "から重力を 1620000 .*--auto-reload: 刻み [0-9]+ まで流した" FAIL_REGULAR_EXPRESSION "物理の突き合わせ")
set_tests_properties(window_gravity_reload_play PROPERTIES LABELS gpu TIMEOUT 240 FIXTURES_REQUIRED window_gravity_reload
  PASS_REGULAR_EXPRESSION "再生ファイルの表を使う.*から重力を 1620000 .*再生: OK")
# 反応表のパネル(T-0219): 写したパッケージの木の燃焼の速さをパネルの書き戻しで変える → ホットリロードで当たる
# → 元に戻す(ファイルは元の中身)→ 元の版の表に戻って終える(editor/reaction_table_panel。通らなければ終了コード 1)
set(WINDOW_TABLE_EDIT_PACKAGES ${CMAKE_CURRENT_BINARY_DIR}/table_edit_packages)
add_test(NAME window_table_edit_copy
         COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data/packages ${WINDOW_TABLE_EDIT_PACKAGES})
add_test(NAME window_table_edit
         COMMAND bicameral --world sample --editor --play --auto-table-edit --packages ${WINDOW_TABLE_EDIT_PACKAGES} --frames 3000)
set_tests_properties(window_table_edit_copy PROPERTIES FIXTURES_SETUP window_table_edit_packages)
set_tests_properties(window_table_edit PROPERTIES LABELS gpu TIMEOUT 600 FIXTURES_REQUIRED window_table_edit_packages
  PASS_REGULAR_EXPRESSION "--auto-table-edit: 元の版 [0-9a-f]+ の表に戻った。OK")

# 画像の比較(T-0025、16 §5): 決まった刻みの始めで世界を止め、S(その刻み) を描いた画面を基準の画像(tests/images/)と比べる。
# 写す(ランタイム)→ 比べる(tools/image_compare)の 2 つのテスト。実際の画像と差の画像はビルドのフォルダの images/ に出る。
# 基準の作り方・置き換え方は 16 §5。窓を開くのでラベル gpu(CI は外す)。--frames は刻みに届かなかった時の上限
set(IMAGE_ACTUAL_DIR ${CMAKE_CURRENT_BINARY_DIR}/images)
set(IMAGE_BASELINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/images)
set(IMAGE_COMPARE ${PROJECT_SOURCE_DIR}/tools/image_compare/image_compare.py)
function(bicameral_add_image_test name tick)
  add_test(NAME image_${name}_capture
           COMMAND bicameral --world sample --frames 3000 --screenshot-tick ${tick} --screenshot ${IMAGE_ACTUAL_DIR}/${name}.bmp ${ARGN})
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

# --- 試験の反応表(パッケージ)を使うテスト(T-0169。ファイルの先頭の bicameral_test_reaction_table)---
foreach(_name IN ITEMS
    reaction_package_test reaction_package_scene_test reaction_species_remap_test reaction_test reaction_contention_test
    reaction_wait_test multires_test multires_wide_test multires_activity_test multires_quiet_test multires_uniform_test
    multires_state_test field_storage_test sun_column_test sun_lens_test
    multires_conduction_test multires_edge_test multires_subcycle_test multires_implicit_test multires_implicit_tree_test lab_box_test
    gpu_reaction_test gpu_sun_column_test gpu_multires_test gpu_multires_activity_test gpu_multires_quiet_test gpu_multires_uniform_test
    gpu_multires_conduction_test gpu_multires_implicit_test gpu_multires_implicit_build_test gpu_multires_implicit_tree_test
    gpu_multires_implicit_conduction_test gpu_lab_box_test gpu_probe_sim_test gpu_probe_peek_test gpu_probe_place_test
    gpu_probe_trace_test gpu_probe_rewind_test gpu_world_copy_test gpu_probe_physics_test gpu_probe_fire_test
    gpu_conduct_bench gpu_implicit_box_bench gpu_place_bench blank_world_test gpu_probe_blank_test gpu_probe_gas_test
    gpu_probe_liquid_test world_layout_test play_metric_test
    gpu_play_metric_test environment_package_test)
  target_link_libraries(${_name} PRIVATE bicameral_test_reaction_table)
endforeach()
