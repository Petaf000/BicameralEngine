# シェーダーのビルド(T-0011、docs/design/04-numerics-determinism.md §4)。
#
#   shaders/sim/     シミュ(世界の状態と因果)。浮動小数点は禁止(D-205)。コンパイルのたびに DXIL を検査し、あればビルドを落とす
#   shaders/render/  描画。浮動小数点を使ってよい
#   shaders/common/  sim から読む共通のヘッダ(C++ からも読む)。ここも浮動小数点は禁止。描画だけの共通部品は shaders/render/ に置く
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

# --- bicameral_add_shader(<shaders/ からの相対パス> PROFILE <cs_6_8|lib_6_8|...> [ENTRY <関数名>]) ---
#   ENTRY は compute などの入口。Work Graphs のライブラリ(lib_6_8)は省く。
#   パスの最初のフォルダ(sim / render)で、検査の有無と出力先が決まる。
function(bicameral_add_shader source)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "PROFILE;ENTRY" "")
  if(NOT arg_PROFILE)
    message(FATAL_ERROR "bicameral_add_shader(${source}): PROFILE がない")
  endif()
  string(REGEX MATCH "^[^/]+" kind "${source}")
  if(NOT kind STREQUAL "sim" AND NOT kind STREQUAL "render")
    message(FATAL_ERROR "bicameral_add_shader(${source}): シェーダーは shaders/sim/ か shaders/render/ に置く")
  endif()

  get_filename_component(name "${source}" NAME_WE)
  set(input ${CMAKE_CURRENT_SOURCE_DIR}/${source})
  set(cso ${BICAMERAL_SHADER_OUT}/${kind}/${name}.cso)
  set(asm ${BICAMERAL_SHADER_ASM}/${kind}/${name}.asm)
  set(entryFlags "")
  if(arg_ENTRY)
    set(entryFlags -E ${arg_ENTRY})
  endif()

  # シミュのシェーダーだけ、コンパイルの直後に DXIL の浮動小数点を検査する(違反なら .cso を消して失敗)
  set(checkCommand "")
  if(kind STREQUAL "sim")
    set(checkCommand COMMAND ${Python3_EXECUTABLE} ${BICAMERAL_FLOAT_CHECK_DIR}/dxil_float_check.py
                             ${asm} --source ${source} --remove ${cso})
  endif()

  add_custom_command(
    OUTPUT ${cso} ${asm}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BICAMERAL_SHADER_OUT}/${kind} ${BICAMERAL_SHADER_ASM}/${kind}
    COMMAND ${DIRECTX_DXC_TOOL} -HV 2021 -T ${arg_PROFILE} ${entryFlags} -WX ${BICAMERAL_DXC_CONFIG_FLAGS}
            -I ${CMAKE_CURRENT_SOURCE_DIR} -Fo ${cso} -Fc ${asm} ${input}
    ${checkCommand}
    DEPENDS ${input} ${BICAMERAL_SHADER_HEADERS} ${BICAMERAL_FLOAT_CHECK_DIR}/dxil_float_check.py
    COMMENT "DXC: ${source} (${arg_PROFILE})"
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

get_property(BICAMERAL_SHADER_OUTPUTS GLOBAL PROPERTY BICAMERAL_SHADER_OUTPUTS)
add_custom_target(bicameral_shaders ALL DEPENDS ${BICAMERAL_SHADER_OUTPUTS} ${BICAMERAL_FLOAT_SOURCE_STAMP})
