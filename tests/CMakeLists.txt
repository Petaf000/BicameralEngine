# テスト。GPU カーネルごとに「CPU リファレンス実装との一致」テストを置く方針(docs/design/02-reaction-system.md)。
add_executable(smoke_test smoke_test.cpp)
add_test(NAME smoke COMMAND smoke_test)

add_executable(singleton_test singleton_test.cpp)
target_link_libraries(singleton_test PRIVATE bicameral_core)
add_test(NAME singleton COMMAND singleton_test)

add_executable(log_test log_test.cpp)
target_link_libraries(log_test PRIVATE bicameral_core)
add_test(NAME log COMMAND log_test)

add_executable(fixed_test fixed_test.cpp)
target_link_libraries(fixed_test PRIVATE bicameral_fixed)
add_test(NAME fixed COMMAND fixed_test)

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
