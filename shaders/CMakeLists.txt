# シェーダーのビルド(T-0011、docs/design/04-numerics-determinism.md §4)。
#
#   shaders/sim/     シミュ(世界の状態と因果)。浮動小数点は禁止(D-205)。コンパイルのたびに DXIL を検査し、あればビルドを落とす
#   shaders/render/  描画。浮動小数点を使ってよい
#   shaders/common/  sim から読む共通のヘッダ(C++ からも読む)。ここも浮動小数点は禁止。描画だけの共通部品は shaders/render/ に置く
#   shaders/bench/   計測だけに使う(整数の費用を浮動小数点と比べるので、浮動小数点を使ってよい。シミュからは使わない)
#
# 成果物は bin/shaders/<sim|render>/<名前>.cso。逆アセンブル(検査に使う)は ビルドフォルダ/shaders/asm/ に残る。
# シェーダーを足すときは、このファイルの末尾に bicameral_add_shader() を 1 行足す。

set(BICAMERAL_SHADER_OUT ${CMAKE_BINARY_DIR}/bin/shaders)
set(BICAMERAL_SHADER_ASM ${CMAKE_CURRENT_BINARY_DIR}/asm)
set(BICAMERAL_FLOAT_CHECK_DIR ${PROJECT_SOURCE_DIR}/tools/float_check)
# 依存の取りこぼしを避けるため、どのシェーダーも shaders/ 以下のすべてのヘッダに依存させる(数が少ないうちはこれで十分)
file(GLOB_RECURSE BICAMERAL_SHADER_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.hlsli)

# Debug では最適化を切り、デバッグ情報を埋め込む(PIX でソースを見るため)。整数の結果は最適化で変わらない(04「未確認」、T-0013 で確かめる)
set(BICAMERAL_DXC_CONFIG_FLAGS $<$<CONFIG:Debug>:-Od$<SEMICOLON>-Zi$<SEMICOLON>-Qembed_debug>)
# ただしシミュのシェーダーは Debug でも最適化する(-O3。ADR-0079・T-0239): -Od では局所の配列(6×6 の行列など)がシェーダーの局所メモリに
# 残り、別の重いパイプライン(-Od の刻み)を同じキューで設定すると、後ろの物理の結果が起動ごとに CPU とずれた。PIX で 1 行ずつ追う時だけ
# BICAMERAL_SIM_SHADER_OD=ON で -Od に戻す(そのビルドの窓の再生・覗き窓は食い違うことがある)
option(BICAMERAL_SIM_SHADER_OD "Debug のシミュのシェーダーを -Od で作る(PIX で追う時だけ。T-0239)" OFF)
if(BICAMERAL_SIM_SHADER_OD)
  set(BICAMERAL_DXC_SIM_CONFIG_FLAGS ${BICAMERAL_DXC_CONFIG_FLAGS})
else()
  set(BICAMERAL_DXC_SIM_CONFIG_FLAGS $<$<CONFIG:Debug>:-O3$<SEMICOLON>-Zi$<SEMICOLON>-Qembed_debug>)
endif()
# Debug のシミュのシェーダーだけ、デバッグのリング(common/debug_ring.hlsli、T-0003)の DEBUG_PRINT・DEBUG_ASSERT・FX_ASSERT を有効にする。
# 計測(bench)は付けない(費用を測るのにデバッグの書き込みが混ざらないように)
set(BICAMERAL_DXC_SIM_DEBUG_FLAGS $<$<CONFIG:Debug>:-DBICAMERAL_GPU_DEBUG=1>)

# --- bicameral_add_shader(<shaders/ からの相対パス> PROFILE <cs_6_8|lib_6_8|...> [ENTRY <関数名>]
#                         [NAME <出力の名前>] [DEFINES <名前=値>...] [FLAGS <DXC の引数>...]) ---
#   ENTRY は compute などの入口。Work Graphs のライブラリ(lib_6_8)は省く。
#   NAME を付けると、同じソースを DEFINES 違いで何度もコンパイルできる(計測の変種など)。省けばソースの名前。
#   パスの最初のフォルダ(sim / render / bench)で、検査の有無と出力先が決まる。
function(bicameral_add_shader source)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "PROFILE;ENTRY;NAME" "DEFINES;FLAGS")
  if(NOT arg_PROFILE)
    message(FATAL_ERROR "bicameral_add_shader(${source}): PROFILE がない")
  endif()
  string(REGEX MATCH "^[^/]+" kind "${source}")
  if(NOT kind STREQUAL "sim" AND NOT kind STREQUAL "render" AND NOT kind STREQUAL "bench")
    message(FATAL_ERROR "bicameral_add_shader(${source}): シェーダーは shaders/sim/・render/・bench/ のどれかに置く")
  endif()

  get_filename_component(name "${source}" NAME_WE)
  if(arg_NAME)
    set(name ${arg_NAME})
  endif()
  set(input ${CMAKE_CURRENT_SOURCE_DIR}/${source})
  set(cso ${BICAMERAL_SHADER_OUT}/${kind}/${name}.cso)
  set(asm ${BICAMERAL_SHADER_ASM}/${kind}/${name}.asm)
  set(entryFlags "")
  if(arg_ENTRY)
    set(entryFlags -E ${arg_ENTRY})
  endif()
  set(defineFlags "")
  foreach(define IN LISTS arg_DEFINES)
    list(APPEND defineFlags -D ${define})
  endforeach()

  # シミュのシェーダーだけ、コンパイルの直後に DXIL の浮動小数点を検査する(違反なら .cso を消して失敗)
  set(checkCommand "")
  set(kindFlags "")
  set(configFlags ${BICAMERAL_DXC_CONFIG_FLAGS})
  if(kind STREQUAL "sim")
    set(kindFlags ${BICAMERAL_DXC_SIM_DEBUG_FLAGS})
    set(configFlags ${BICAMERAL_DXC_SIM_CONFIG_FLAGS})
    set(checkCommand COMMAND ${Python3_EXECUTABLE} ${BICAMERAL_FLOAT_CHECK_DIR}/dxil_float_check.py
                             ${asm} --source ${source} --remove ${cso})
  endif()

  add_custom_command(
    OUTPUT ${cso} ${asm}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BICAMERAL_SHADER_OUT}/${kind} ${BICAMERAL_SHADER_ASM}/${kind}
    COMMAND ${DIRECTX_DXC_TOOL} -HV 2021 -T ${arg_PROFILE} ${entryFlags} -WX ${configFlags} ${kindFlags}
            ${defineFlags} ${arg_FLAGS} -I ${CMAKE_CURRENT_SOURCE_DIR} -Fo ${cso} -Fc ${asm} ${input}
    ${checkCommand}
    DEPENDS ${input} ${BICAMERAL_SHADER_HEADERS} ${BICAMERAL_FLOAT_CHECK_DIR}/dxil_float_check.py
    COMMENT "DXC: ${source} (${arg_PROFILE}) → ${name}"
    COMMAND_EXPAND_LISTS
    VERBATIM)
  set_property(GLOBAL APPEND PROPERTY BICAMERAL_SHADER_OUTPUTS ${cso})
endfunction()

# --- シミュのソースの浮動小数点の検査(C++ 側も。04 §4)-----------------------------------------------
# DXIL の検査は HLSL としてコンパイルされた部分しか見ない。共通のヘッダの C++ だけの分岐や CPU リファレンスはここで見る。
set(BICAMERAL_FLOAT_SOURCE_ROOTS
  ${CMAKE_CURRENT_SOURCE_DIR}/common
  ${CMAKE_CURRENT_SOURCE_DIR}/sim
  ${PROJECT_SOURCE_DIR}/engine/src/sim)
file(GLOB_RECURSE BICAMERAL_FLOAT_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/common/*
  ${CMAKE_CURRENT_SOURCE_DIR}/sim/*
  ${PROJECT_SOURCE_DIR}/engine/src/sim/*)
set(BICAMERAL_FLOAT_SOURCE_STAMP ${CMAKE_CURRENT_BINARY_DIR}/source_float_check.stamp)
add_custom_command(
  OUTPUT ${BICAMERAL_FLOAT_SOURCE_STAMP}
  COMMAND ${Python3_EXECUTABLE} ${BICAMERAL_FLOAT_CHECK_DIR}/source_float_check.py
          ${BICAMERAL_FLOAT_SOURCE_ROOTS} --stamp ${BICAMERAL_FLOAT_SOURCE_STAMP}
  DEPENDS ${BICAMERAL_FLOAT_SOURCES} ${BICAMERAL_FLOAT_CHECK_DIR}/source_float_check.py
  COMMENT "浮動小数点の検査: shaders/common, shaders/sim, engine/src/sim"
  VERBATIM)

# --- シェーダーの一覧 ---------------------------------------------------------------------------------
bicameral_add_shader(sim/fixed_selftest.hlsl PROFILE cs_6_8 ENTRY Main)  # 整数の数学ライブラリの自己テスト(T-0010 / T-0013)
bicameral_add_shader(sim/work_graph_probe.hlsl PROFILE lib_6_8)  # Work Graphs が動くかの確認(T-0013)
bicameral_add_shader(sim/debug_ring_probe.hlsl PROFILE cs_6_8 ENTRY Main)  # デバッグのリングの確認(T-0003)
bicameral_add_shader(sim/debug_ring_graph_probe.hlsl PROFILE lib_6_8)    # 同じく Work Graphs のノードから(T-0003)
# 仮の刻みの単位と抽出・コマンドキュー(T-0004・T-0012・T-0086。フレームと刻みのループの確認。中身は T-0005 以降で本物に)。入口ごとに別の .cso
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY EnqueueCommands NAME probe_tick_enqueue)
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY ApplyCommands NAME probe_tick_apply)
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY WakeDueBlocks NAME probe_tick_wake_due)
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY AddPhysicsHeat NAME probe_tick_add_physics_heat)  # T-0561
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY FinishPhysicsHeat NAME probe_tick_finish_physics_heat)
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY RefreshTable NAME probe_tick_refresh_table)
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY RemapSpecies NAME probe_tick_remap_species)  # T-0223
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY Extract NAME probe_tick_extract)
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY Busy NAME probe_tick_busy)
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY HashCells NAME probe_tick_hash_cells)
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY FlushEvents NAME probe_tick_flush_events)
# 仮の世界の気体の区画(T-0592。適用の単位の中。呼ぶ順)
bicameral_add_shader(sim/probe_gas_tick.hlsl PROFILE cs_6_8 ENTRY GasBegin NAME probe_gas_begin)
bicameral_add_shader(sim/probe_gas_tick.hlsl PROFILE cs_6_8 ENTRY GasCollect NAME probe_gas_collect)
bicameral_add_shader(sim/probe_gas_tick.hlsl PROFILE cs_6_8 ENTRY GasPrepare NAME probe_gas_prepare)
bicameral_add_shader(sim/probe_gas_tick.hlsl PROFILE cs_6_8 ENTRY GasGather NAME probe_gas_gather)
bicameral_add_shader(sim/probe_gas_tick.hlsl PROFILE cs_6_8 ENTRY GasCheck NAME probe_gas_check)
bicameral_add_shader(sim/probe_gas_tick.hlsl PROFILE cs_6_8 ENTRY GasScatter NAME probe_gas_scatter)
bicameral_add_shader(sim/probe_gas_tick.hlsl PROFILE cs_6_8 ENTRY GasFinish NAME probe_gas_finish)
bicameral_add_shader(sim/probe_gas_tick.hlsl PROFILE cs_6_8 ENTRY GasHash NAME probe_gas_hash)
# 仮の世界の GPU の液体の段(T-0613)。sim/probe_sim_liquid.cpp が呼ぶ順
bicameral_add_shader(sim/probe_liquid_tick.hlsl PROFILE cs_6_8 ENTRY LiquidRead NAME probe_liquid_read)
bicameral_add_shader(sim/probe_liquid_tick.hlsl PROFILE cs_6_8 ENTRY LiquidPressure NAME probe_liquid_pressure)
bicameral_add_shader(sim/probe_liquid_tick.hlsl PROFILE cs_6_8 ENTRY LiquidForces NAME probe_liquid_forces)
bicameral_add_shader(sim/probe_liquid_tick.hlsl PROFILE cs_6_8 ENTRY LiquidOutgoing NAME probe_liquid_outgoing)
bicameral_add_shader(sim/probe_liquid_tick.hlsl PROFILE cs_6_8 ENTRY LiquidMoveFaces NAME probe_liquid_move_faces)
bicameral_add_shader(sim/probe_liquid_tick.hlsl PROFILE cs_6_8 ENTRY LiquidApply NAME probe_liquid_apply)
bicameral_add_shader(sim/probe_liquid_tick.hlsl PROFILE cs_6_8 ENTRY LiquidLeaving NAME probe_liquid_leaving)
bicameral_add_shader(sim/probe_liquid_tick.hlsl PROFILE cs_6_8 ENTRY LiquidAdvect NAME probe_liquid_advect)
bicameral_add_shader(sim/probe_liquid_tick.hlsl PROFILE cs_6_8 ENTRY LiquidSettleFaces NAME probe_liquid_settle_faces)
bicameral_add_shader(sim/probe_liquid_tick.hlsl PROFILE cs_6_8 ENTRY LiquidSettleCells NAME probe_liquid_settle_cells)
bicameral_add_shader(sim/probe_liquid_tick.hlsl PROFILE cs_6_8 ENTRY LiquidHash NAME probe_liquid_hash)
bicameral_add_shader(sim/probe_surface.hlsl PROFILE cs_6_8 ENTRY BuildSurfaceMask NAME probe_surface_mask)  # 筆の面に沿って置く物の印(T-0332)
bicameral_add_shader(sim/probe_conduct.hlsl PROFILE lib_6_8)  # 伝導の Work Graph(T-0005)
bicameral_add_shader(sim/work_graph_limits_probe.hlsl PROFILE lib_6_8)  # ノードのカウンタと上限の試験(T-0008)
bicameral_add_shader(sim/reaction_cells.hlsl PROFILE cs_6_8 ENTRY Main)  # 1 セルの反応の評価(T-0014)
# 物理の 1 刻みの Compute のパス(T-0090。呼ぶ順は engine/src/sim/gpu_physics.cpp)
foreach(_entry IN ITEMS Initialize BeginSubstep Broadphase Narrowphase PrepareManifolds ColorRound FinishColoring Recollide SolveColor UpdateDuals UpdateVelocity Finish BuildManifolds CollectHeat HeatSum HeatShare HeatFinish)
  string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" _snake "${_entry}")
  string(TOLOWER "${_snake}" _snake)
  bicameral_add_shader(sim/physics_step.hlsl PROFILE cs_6_8 ENTRY ${_entry} NAME physics_${_snake})
endforeach()
bicameral_add_shader(sim/physics_graph.hlsl PROFILE lib_6_8)  # 物理の広域の選別 → 接触の生成の Work Graph(T-0092)
bicameral_add_shader(sim/multires_step.hlsl PROFILE cs_6_8 ENTRY WakeDue NAME multires_step_wake)  # 起こす段: 見出しを全部なめる(T-0121)
bicameral_add_shader(sim/multires_step.hlsl PROFILE cs_6_8 ENTRY StepWait NAME multires_step_wait)  # 待ちの丸めで全部を刻む(T-0121)
bicameral_add_shader(sim/multires_step.hlsl PROFILE cs_6_8 ENTRY StepExpandedWaitPass NAME multires_step_expanded_wait)  # 待ちの丸めで頁に広げて刻む(T-0121)
bicameral_add_shader(sim/multires_step.hlsl PROFILE cs_6_8 ENTRY StepWait NAME multires_step_wide_wait DEFINES MR_WIDE_CELLS=1)  # 溢れを使う世界を全部刻む(T-0176)
bicameral_add_shader(sim/multires_step.hlsl PROFILE cs_6_8 ENTRY StepExpandedWaitPass NAME multires_step_wide_expanded_wait DEFINES MR_WIDE_CELLS=1)  # 溢れを使う世界の頁に広げて刻む(T-0176)
bicameral_add_shader(sim/multires_step.hlsl PROFILE cs_6_8 ENTRY WideAllocatePass NAME multires_step_wide_allocate DEFINES MR_WIDE_CELLS=1)  # 溢れの塊を配る(T-0236)
bicameral_add_shader(sim/multires_step.hlsl PROFILE cs_6_8 ENTRY WideRetryPass NAME multires_step_wide_retry DEFINES MR_WIDE_CELLS=1)  # 塊が足りなかったブロックを刻み直す(T-0236)
bicameral_add_shader(sim/multires_step.hlsl PROFILE cs_6_8 ENTRY WideDeferredPass NAME multires_step_wide_deferred DEFINES MR_WIDE_CELLS=1)  # 伝導の段が回した溢れるセルを刻む(T-0272)
bicameral_add_shader(sim/multires_step.hlsl PROFILE cs_6_8 ENTRY WideBigPass NAME multires_step_wide_big DEFINES MR_WIDE_CELLS=1 MR_WIDE_BIG_PASS=1 RX_CELL_ACCESSORS=1)  # 速い形に入らないセルを作業場の形で刻む(T-0362)
bicameral_add_shader(sim/multires_graph.hlsl PROFILE lib_6_8)  # 多重解像度の細かくする・粗くする・影の引き戻し(T-0017・T-0018)
bicameral_add_shader(sim/multires_graph.hlsl PROFILE lib_6_8 NAME multires_graph_wide DEFINES MR_WIDE_CELLS=1)  # 溢れを使う世界の細かくする(子の頁へ溢れを写す。T-0237)
bicameral_add_shader(sim/multires_activity_graph.hlsl PROFILE lib_6_8)  # 木の上の活性: 種と面の隣だけ刻む(T-0100)
bicameral_add_shader(sim/multires_conduct_graph.hlsl PROFILE lib_6_8)  # 熱の伝導の段の Work Graph 版(T-0107。D-302 で比べる)
foreach(_entry IN ITEMS TreeResolve TreeSettle TreeAllocate TreeRelease TreeClearIndex TreeFillIndex TreeQuiet TreeExpand TreeFoldCheck TreeFold TreeFractions TreeQuietCheck)  # 木の管理の段(T-0018・T-0101・T-0102・T-0103・T-0107・T-0113)
  string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" _snake "${_entry}")
  string(TOLOWER "${_snake}" _snake)
  bicameral_add_shader(sim/multires_tree.hlsl PROFILE cs_6_8 ENTRY ${_entry} NAME multires_${_snake})
endforeach()
bicameral_add_shader(sim/multires_tree.hlsl PROFILE cs_6_8 ENTRY TreeWideReserve NAME multires_tree_wide_reserve DEFINES MR_WIDE_CELLS=1)  # 細かくする子の頁・粗くする親の新しい面に溢れの塊を付ける(T-0237・T-0329)
bicameral_add_shader(sim/multires_tree.hlsl PROFILE cs_6_8 ENTRY TreeResolve NAME multires_tree_wide_resolve DEFINES MR_WIDE_CELLS=1)  # 溢れを使う世界は粗くする要求を「入りきらない」で断らない(T-0329)
bicameral_add_shader(sim/multires_tree.hlsl PROFILE cs_6_8 ENTRY TreeFoldCheck NAME multires_tree_wide_fold_check DEFINES MR_WIDE_CELLS=1)  # 畳む時に端数の溢れも帳簿へ返す(T-0347)
bicameral_add_shader(sim/multires_tree.hlsl PROFILE cs_6_8 ENTRY TreeWideTrim NAME multires_tree_wide_trim DEFINES MR_WIDE_CELLS=1)  # 端数の枠が要らない塊を返す(T-0347)
bicameral_add_shader(sim/multires_tree.hlsl PROFILE cs_6_8 ENTRY TreeExpand NAME multires_tree_wide_expand DEFINES MR_WIDE_CELLS=1)  # 頁に広げる時に一様の値の溢れを写す塊も配る(T-0384)
foreach(_entry IN ITEMS ConductBegin ConductMark ConductPrepare ConductFlows ConductApply ConductEnd)  # 熱の伝導の段(T-0107・小刻みの終わり T-0109)
  string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" _snake "${_entry}")
  string(TOLOWER "${_snake}" _snake)
  bicameral_add_shader(sim/multires_conduct.hlsl PROFILE cs_6_8 ENTRY ${_entry} NAME multires_${_snake})
  bicameral_add_shader(sim/multires_conduct.hlsl PROFILE cs_6_8 ENTRY ${_entry} NAME multires_wide_${_snake} DEFINES MR_WIDE_CELLS=1)  # 溢れを使う世界の伝導の段(T-0211)
endforeach()
foreach(_entry IN ITEMS Begin Start Smooth Restrict Prolong Converged CycleEnd Flows Mark LimitEnd LimitFaces Apply Tail PlanLevels PlanArgs SolveConverged SolveCheckEnd PlanColors)  # 細かいレベルの熱の陰解法の段(T-0117・Tail は T-0120・Plan は T-0136・Solve は一般の右辺 T-0531・PlanColors は T-0606)
  string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" _snake "${_entry}")
  string(TOLOWER "${_snake}" _snake)
  bicameral_add_shader(sim/implicit_conduct.hlsl PROFILE cs_6_8 ENTRY Im${_entry} NAME implicit_${_snake})
endforeach()
foreach(_entry IN ITEMS Clear CountBlocks ScanBlocks NumberUnknowns FaceEntries ScanEntriesLocal ScanEntriesGroups ScanEntriesAdd Boundary Cells Faces ScanCellsLocal ScanCellsGroups ScanCellsAdd FillLists SortLists Apply)  # 陰解法の系を GPU で作る段(T-0129・解いた変化を伝導の表へ足す Apply は T-0132)
  string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" _snake "${_entry}")
  string(TOLOWER "${_snake}" _snake)
  bicameral_add_shader(sim/implicit_build.hlsl PROFILE cs_6_8 ENTRY Build${_entry} NAME implicit_build_${_snake})
endforeach()
foreach(_entry IN ITEMS Clear Cells CellLinks NodeHash NodeFirst ParentScanLocal ParentScanGroups Parents LinkHash Count RowScanLocal RowScanGroups Place Sort AddCoefficients CoarseNodes Finish RoundEnd Tail)  # 陰解法の多重格子の段を GPU で作る段(T-0134・Tail は T-0135)
  string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" _snake "${_entry}")
  string(TOLOWER "${_snake}" _snake)
  bicameral_add_shader(sim/implicit_levels.hlsl PROFILE cs_6_8 ENTRY Lv${_entry} NAME implicit_levels_${_snake})
endforeach()
bicameral_add_shader(sim/multires_peek.hlsl PROFILE cs_6_8 ENTRY MirrorWorld NAME multires_peek_mirror)  # 覗き窓の世界の写し(T-0096)
bicameral_add_shader(sim/multires_peek.hlsl PROFILE cs_6_8 ENTRY ExtractShadow NAME multires_peek_extract)  # 覗き窓の抽出(T-0096)
bicameral_add_shader(sim/sun_column.hlsl PROFILE cs_6_8 ENTRY SunClear NAME sun_column_clear)  # 太陽の影の柱の出力を 0 に(T-0910)
bicameral_add_shader(sim/sun_column.hlsl PROFILE cs_6_8 ENTRY SunSweep NAME sun_column_sweep)  # 太陽の影の柱(T-0910・ADR-0396)
bicameral_add_shader(sim/sky_port.hlsl PROFILE cs_6_8 ENTRY SkyPortClear NAME sky_port_clear)  # 空との交換の口の出力を 0 に(T-1147)
bicameral_add_shader(sim/sky_port.hlsl PROFILE cs_6_8 ENTRY SkyPortSweep NAME sky_port_sweep)  # 空との交換の口を 1 回の起動で(T-1147・ADR-0474)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcInitState NAME cascade_light_init_state)  # 距離ごとの段の GPU: 状態を 0 に(T-0959)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcInitBags NAME cascade_light_init_bags)  # 距離ごとの段の GPU: 袋の溜めを空に・初めの持ち越し(T-0959)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcBeginTick NAME cascade_light_begin_tick)  # 距離ごとの段の GPU: 頭の語を 0 に(T-0959)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcSources NAME cascade_light_sources)  # 距離ごとの段の GPU: 熱の放つ量(T-0959)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcCarryIn NAME cascade_light_carry_in)  # 距離ごとの段の GPU: 持ち越しを放つ面へ(T-0959)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcClearFaces NAME cascade_light_clear_faces)  # 距離ごとの段の GPU: 放つ面の量と印を 0 に(T-1010)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcLevel0Prepare NAME cascade_light_level0_prepare)  # 距離ごとの段の GPU: 段 0 の分け方(T-0959)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcLevel0Walk NAME cascade_light_level0_walk)  # 距離ごとの段の GPU: 段 0 の光線(T-0959)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcLevelWalk NAME cascade_light_level_walk)  # 距離ごとの段の GPU: 段 k の袋の光線(T-0959)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcClearBags NAME cascade_light_clear_bags)  # 距離ごとの段の GPU: 袋の表を空に(T-0959)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcResetBags NAME cascade_light_reset_bags)  # 距離ごとの段の GPU: 袋の数を 0 に(T-0959)
bicameral_add_shader(sim/probe_radiation.hlsl PROFILE cs_6_8 ENTRY RadiationGrid NAME probe_radiation_grid)  # 仮の世界の GPU の放射: セル → 光の値・温度(T-1154)
bicameral_add_shader(sim/probe_radiation.hlsl PROFILE cs_6_8 ENTRY RadiationApply NAME probe_radiation_apply)  # 仮の世界の GPU の放射: セルへ当てる(T-1154)
bicameral_add_shader(sim/probe_radiation.hlsl PROFILE cs_6_8 ENTRY RadiationHash NAME probe_radiation_hash)  # 仮の世界の GPU の放射: 持ち越しのハッシュ(T-1154)
bicameral_add_shader(sim/cascade_light.hlsl PROFILE cs_6_8 ENTRY RcCarryOut NAME cascade_light_carry_out)  # 距離ごとの段の GPU: 照り返しを疎な表へ(T-0959)
# 物の熱(物の箱のボクセルの熱と反応・セルとの対の面の 4 つの口・ブロックごとに眠らせる。T-1064・T-1098・ADR-0434・ADR-0459。呼ぶ順は tests/gpu_object_heat_test.cpp)
bicameral_add_shader(sim/object_heat.hlsl PROFILE cs_6_8 ENTRY OhWakeBlocks NAME object_heat_wake)
bicameral_add_shader(sim/object_heat.hlsl PROFILE cs_6_8 ENTRY OhPrepare NAME object_heat_prepare)
bicameral_add_shader(sim/object_heat.hlsl PROFILE cs_6_8 ENTRY OhFlowStep NAME object_heat_flow)
bicameral_add_shader(sim/object_heat.hlsl PROFILE cs_6_8 ENTRY OhReduce NAME object_heat_reduce)
bicameral_add_shader(sim/object_heat.hlsl PROFILE cs_6_8 ENTRY OhApplyCells NAME object_heat_apply_cells)
bicameral_add_shader(sim/object_heat.hlsl PROFILE cs_6_8 ENTRY OhReact NAME object_heat_react)
# 起きている物の一覧(印・グループの和の前の和・一覧に書く・反応の後に物の起こす刻みを直す。T-1077)
bicameral_add_shader(sim/object_heat.hlsl PROFILE cs_6_8 ENTRY OhListMark NAME object_heat_list_mark)
bicameral_add_shader(sim/object_heat.hlsl PROFILE cs_6_8 ENTRY OhListTotals NAME object_heat_list_totals)
bicameral_add_shader(sim/object_heat.hlsl PROFILE cs_6_8 ENTRY OhListPlace NAME object_heat_list_place)
bicameral_add_shader(sim/object_heat.hlsl PROFILE cs_6_8 ENTRY OhFinishObjects NAME object_heat_finish_objects)
# 物の熱の囲むセルを多重解像度の世界と写し合う(多重解像度のルート署名 + 外の u4・u5。T-1077。呼ぶ順は tests/gpu_object_heat_nest_test.cpp)
bicameral_add_shader(sim/object_heat_nest.hlsl PROFILE cs_6_8 ENTRY OhNestGather NAME object_heat_nest_gather)
bicameral_add_shader(sim/object_heat_nest.hlsl PROFILE cs_6_8 ENTRY OhNestMarkExpand NAME object_heat_nest_mark_expand)
bicameral_add_shader(sim/object_heat_nest.hlsl PROFILE cs_6_8 ENTRY OhNestFill NAME object_heat_nest_fill)
bicameral_add_shader(sim/object_heat_nest.hlsl PROFILE cs_6_8 ENTRY OhNestScatter NAME object_heat_nest_scatter)
bicameral_add_shader(sim/lab_box.hlsl PROFILE cs_6_8 ENTRY ApplyLabCommands NAME lab_box_apply)  # 実験室の箱のコマンドを当てる(T-0142)
bicameral_add_shader(sim/lab_box.hlsl PROFILE cs_6_8 ENTRY RemapLabSpecies NAME lab_box_remap)  # 実験室の箱の物質の付け替え(T-0242)
bicameral_add_shader(render/probe_view.hlsl PROFILE vs_6_8 ENTRY VSMain NAME probe_view_vs)
bicameral_add_shader(render/probe_view.hlsl PROFILE ps_6_8 ENTRY PSMain NAME probe_view_ps)

# 整数のルーチンの費用のマイクロベンチ(T-0010、04 §6)。1 つのソースを計測する演算ごとにコンパイルする。
# 一覧は tests/gpu_fixed_bench.cpp の BENCH_OPERATIONS と同じ順・同じ名前にする(番号 = BENCH_OP)。
# 浮動小数点の変種は -Gis(IEEE に厳密)で、DXC が掛け算の連鎖をまとめ直さないようにする。
set(BICAMERAL_BENCH_OPERATIONS
  base32 base64 add32 mul32 div32 add64 mul64 div64 fadd fmul fdiv
  mulshift32 mulshift64 mulfull128 divs64 divshift64 div128 recip32 recip64 recips64 makerecip64
  sqrt32 sqrt64 exp2 log2 exp ln sincos hash64 solve6)
set(benchIndex 0)
foreach(operation IN LISTS BICAMERAL_BENCH_OPERATIONS)
  set(benchFlags "")
  if(operation MATCHES "^f")
    set(benchFlags -Gis)
  endif()
  bicameral_add_shader(bench/fixed_bench.hlsl PROFILE cs_6_8 ENTRY Main NAME fixed_bench_${operation}
                       DEFINES BENCH_OP=${benchIndex} FLAGS ${benchFlags})
  math(EXPR benchIndex "${benchIndex} + 1")
endforeach()

# 気体の流れの 1 小刻みの Compute の段(T-0185 G3 の第 1 段。呼ぶ順は engine/src/sim/gpu_gas.cpp)
foreach(_entry IN ITEMS GasDerive GasFaceForce GasApplyForce GasFaceFlow GasOutflow GasFaceMoved GasMovedSum GasFaceFinal GasTransfer
                      GasViscosity GasViscosityApply)
  string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" _snake "${_entry}")
  string(TOLOWER "${_snake}" _snake)
  bicameral_add_shader(sim/gas_step.hlsl PROFILE cs_6_8 ENTRY ${_entry} NAME ${_snake})
endforeach()

# 釣り合う家族の共通部品の GPU 版の段(T-0915。粗い段の重み・残差の計器。呼ぶ順は engine/src/sim/gpu_equilibrium.cpp)
foreach(_entry IN ITEMS EquilibriumLevel EquilibriumMeter)
  string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" _snake "${_entry}")
  string(TOLOWER "${_snake}" _snake)
  bicameral_add_shader(sim/equilibrium.hlsl PROFILE cs_6_8 ENTRY ${_entry} NAME ${_snake})
endforeach()

# 部品の段 0 を CPU が組んだ系の写しから仕上げる法則の段(T-1111。時間を飛ばす試作の列の系を GPU で解き比べる。engine/src/sim/gpu_equilibrium_upload.cpp)
bicameral_add_shader(sim/equilibrium_upload.hlsl PROFILE cs_6_8 ENTRY EquilibriumUploadNode NAME equilibrium_upload_node)
# 段 0 の時間の測りの変種(T-1141。1 = 右辺の項を割らない・2 = 読むだけ。値は壊れる。gpu_time_step_span_test --stage-variants)
bicameral_add_shader(sim/equilibrium_upload.hlsl PROFILE cs_6_8 ENTRY EquilibriumUploadNode NAME equilibrium_upload_node_m1 DEFINES EQ_UPLOAD_MEASURE=1)
bicameral_add_shader(sim/equilibrium_upload.hlsl PROFILE cs_6_8 ENTRY EquilibriumUploadNode NAME equilibrium_upload_node_m2 DEFINES EQ_UPLOAD_MEASURE=2)

# 成分を持つ節の釣り合いの GPU の段(T-0970。成分の数 k = 1・2・3・6 ごとに別々に作る。呼ぶ順は engine/src/sim/gpu_equilibrium_block.cpp)
foreach(_k IN ITEMS 1 2 3 6)
  # Conjugate*: 共役勾配(EquilibriumBlockOptions::lineSearch。T-1085・ADR-0455)
  foreach(_entry IN ITEMS Finish Coarsen Smooth Restrict Prolong ConjugateSave ConjugateResidual ConjugateBeta
                          ConjugateDirection ConjugatePush ConjugateAlpha ConjugateUpdate)
    string(TOLOWER "${_entry}" _lower)
    bicameral_add_shader(sim/equilibrium_block.hlsl PROFILE cs_6_8 ENTRY EquilibriumBlock${_entry}
                         NAME equilibrium_block_k${_k}_${_lower} DEFINES EQB_K=${_k})
  endforeach()
endforeach()

# 気体の圧力の投影の 1 刻みの段(T-0845。本物の音速の陰解法の式を作る・当てる。段の重みと V は部品〔T-0915〕。呼ぶ順は engine/src/sim/gpu_gas_projection.cpp)
foreach(_entry IN ITEMS GasProjectFace GasProjectCell GasProjectPressure GasProjectFlow)
  string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" _snake "${_entry}")
  string(TOLOWER "${_snake}" _snake)
  bicameral_add_shader(sim/gas_projection.hlsl PROFILE cs_6_8 ENTRY ${_entry} NAME ${_snake})
endforeach()

# 気体の多重解像度の木の 1 小刻みの Compute の段(T-0209。呼ぶ順は engine/src/sim/gpu_gas_tree.cpp)
foreach(_entry IN ITEMS GasTreeDerive GasTreeFaceForce GasTreeApplyForce GasTreeFaceFlow GasTreeOutflow GasTreeFaceMoved GasTreeMovedSum GasTreeFaceFinal GasTreeTransfer)
  string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" _snake "${_entry}")
  string(TOLOWER "${_snake}" _snake)
  bicameral_add_shader(sim/gas_tree_step.hlsl PROFILE cs_6_8 ENTRY ${_entry} NAME ${_snake})
endforeach()

# 表面の細かい形の整数の関数(研究 R-T1・T-0879。23 §7): 当たりと開き具合
bicameral_add_shader(sim/surface_detail.hlsl PROFILE cs_6_8 ENTRY Contacts NAME surface_detail_contacts)
bicameral_add_shader(sim/surface_detail.hlsl PROFILE cs_6_8 ENTRY Openness NAME surface_detail_openness)

get_property(BICAMERAL_SHADER_OUTPUTS GLOBAL PROPERTY BICAMERAL_SHADER_OUTPUTS)
add_custom_target(bicameral_shaders ALL DEPENDS ${BICAMERAL_SHADER_OUTPUTS} ${BICAMERAL_FLOAT_SOURCE_STAMP})
