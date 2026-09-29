# シェーダーのビルド。今は整数の数学ライブラリの自己テスト 1 本だけ(T-0010)。
# sim / render の分離・DXIL の浮動小数点の検査・一般化したコンパイルの規則は T-0011(docs/design/04-numerics-determinism.md §4)。
set(BICAMERAL_SHADER_OUT ${CMAKE_BINARY_DIR}/bin/shaders)
file(GLOB BICAMERAL_COMMON_SHADER_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/common/*.hlsli)

add_custom_command(
  OUTPUT ${BICAMERAL_SHADER_OUT}/fixed_selftest.cso
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BICAMERAL_SHADER_OUT}
  COMMAND ${DIRECTX_DXC_TOOL} -HV 2021 -T cs_6_8 -E Main -WX
          -I ${CMAKE_CURRENT_SOURCE_DIR}
          -Fo ${BICAMERAL_SHADER_OUT}/fixed_selftest.cso
          ${CMAKE_CURRENT_SOURCE_DIR}/sim/fixed_selftest.hlsl
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/sim/fixed_selftest.hlsl ${BICAMERAL_COMMON_SHADER_HEADERS}
  COMMENT "DXC: sim/fixed_selftest.hlsl (cs_6_8)"
  VERBATIM)
add_custom_target(bicameral_shaders ALL DEPENDS ${BICAMERAL_SHADER_OUT}/fixed_selftest.cso)
