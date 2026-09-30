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
  if(kind STREQUAL "sim")
    set(kindFlags ${BICAMERAL_DXC_SIM_DEBUG_FLAGS})
    set(checkCommand COMMAND ${Python3_EXECUTABLE} ${BICAMERAL_FLOAT_CHECK_DIR}/dxil_float_check.py
                             ${asm} --source ${source} --remove ${cso})
  endif()

  add_custom_command(
    OUTPUT ${cso} ${asm}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BICAMERAL_SHADER_OUT}/${kind} ${BICAMERAL_SHADER_ASM}/${kind}
    COMMAND ${DIRECTX_DXC_TOOL} -HV 2021 -T ${arg_PROFILE} ${entryFlags} -WX ${BICAMERAL_DXC_CONFIG_FLAGS} ${kindFlags}
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
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY Extract NAME probe_tick_extract)
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY Busy NAME probe_tick_busy)
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY HashCells NAME probe_tick_hash_cells)
bicameral_add_shader(sim/probe_tick.hlsl PROFILE cs_6_8 ENTRY FlushEvents NAME probe_tick_flush_events)
bicameral_add_shader(sim/probe_conduct.hlsl PROFILE lib_6_8)  # 伝導の Work Graph(T-0005)
bicameral_add_shader(render/probe_view.hlsl PROFILE vs_6_8 ENTRY VSMain NAME probe_view_vs)
bicameral_add_shader(render/probe_view.hlsl PROFILE ps_6_8 ENTRY PSMain NAME probe_view_ps)

# 整数のルーチンの費用のマイクロベンチ(T-0010、04 §6)。1 つのソースを計測する演算ごとにコンパイルする。
# 一覧は tests/gpu_fixed_bench.cpp の BENCH_OPERATIONS と同じ順・同じ名前にする(番号 = BENCH_OP)。
# 浮動小数点の変種は -Gis(IEEE に厳密)で、DXC が掛け算の連鎖をまとめ直さないようにする。
set(BICAMERAL_BENCH_OPERATIONS
  base32 base64 add32 mul32 div32 add64 mul64 div64 fadd fmul fdiv
  mulshift32 mulshift64 mulfull128 divs64 divshift64 div128 recip32 recip64 recips64 makerecip64
  sqrt32 sqrt64 exp2 log2 exp ln sincos hash64)
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

get_property(BICAMERAL_SHADER_OUTPUTS GLOBAL PROPERTY BICAMERAL_SHADER_OUTPUTS)
add_custom_target(bicameral_shaders ALL DEPENDS ${BICAMERAL_SHADER_OUTPUTS} ${BICAMERAL_FLOAT_SOURCE_STAMP})
