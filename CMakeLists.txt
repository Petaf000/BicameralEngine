cmake_minimum_required(VERSION 3.25)

# vcpkg のキャッシュはリポジトリの out/vcpkg に置く(CMakePresets.json の environment)。
# 既定の %LOCALAPPDATA% はユーザー名が日本語+空白だと vcpkg が扱えない。
# vcpkg はこれらのフォルダが存在することを要求し、project() の中で走るので、その前に作る。
# git-config: パッチを当てる port(luau など)の git が、日本語のユーザー名の %USERPROFILE%\.config\git\config を
# 化けた名前で開こうとして落ちるので、git の設定の場所を ASCII のフォルダに向ける(CMakePresets.json の XDG_CONFIG_HOME・GIT_CONFIG_GLOBAL。T-0020)。
foreach(_d IN ITEMS registries archives downloads git-config)
  file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/out/vcpkg/${_d}")
endforeach()

project(BicameralEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)  # MSVC では /std:c++latest になる(docs/style.md)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(BICAMERAL_GPU_VALIDATION "D3D12 debug layer と GPU-based validation を有効にする" OFF)
option(BICAMERAL_PIX "PIX イベントマーカーを有効にする" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /MP)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# --- 依存(すべて vcpkg。版は vcpkg.json の builtin-baseline で固定)-----------------------------
find_package(directx-headers CONFIG REQUIRED)    # d3d12.h など(Agility SDK と同じ版)
find_package(directx12-agility CONFIG REQUIRED)  # D3D12Core.dll / d3d12SDKLayers.dll(ADR-0004)
find_package(directx-dxc CONFIG REQUIRED)        # シェーダーコンパイラ。DIRECTX_DXC_TOOL に dxc.exe の場所が入る
find_package(imgui CONFIG REQUIRED)              # エディタの殻のパネル(Dear ImGui。Win32 と DX12 の backend。T-0023)
message(STATUS "DXC: ${DIRECTX_DXC_TOOL}")
find_package(unofficial-luau CONFIG REQUIRED)    # Luau(エディタ・ツール・ゲームのオーサリングのスクリプト。CPU だけ。13・T-0020)
# ビルドの中の検査(tools/float_check: シミュの浮動小数点の禁止、04 §4)に使う
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

# --- WARP(ソフトウェアの D3D12)の版を固定する(T-0097・ADR-0013)---------------------------------------
# OS の WARP は、Work Graph のノードが少し複雑になると状態オブジェクトを作る所で落ちる(ADR-0013)。
# NuGet の Microsoft.Direct3D.WARP の d3d10warp.dll を exe の横(bin/)に置くと、D3D12 は OS のものの代わりにこちらを読み込む。
# 版を上げるときは、版とハッシュ(nupkg の SHA-256)を両方変える。
set(BICAMERAL_WARP_VERSION 1.0.21)
set(BICAMERAL_WARP_SHA256 EA44B77EB30EEC14427193E20F40CDB2E4C31ED11AB39CB880538FDF4BAC2681)
set(BICAMERAL_WARP_DIR ${CMAKE_BINARY_DIR}/_deps/warp-${BICAMERAL_WARP_VERSION})
set(BICAMERAL_WARP_DLL ${BICAMERAL_WARP_DIR}/build/native/bin/x64/d3d10warp.dll)
if(NOT EXISTS ${BICAMERAL_WARP_DLL})
  set(_warpPackage ${BICAMERAL_WARP_DIR}.nupkg)
  file(DOWNLOAD
    https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.warp/${BICAMERAL_WARP_VERSION}/microsoft.direct3d.warp.${BICAMERAL_WARP_VERSION}.nupkg
    ${_warpPackage}
    EXPECTED_HASH SHA256=${BICAMERAL_WARP_SHA256}
    TLS_VERIFY ON)
  file(ARCHIVE_EXTRACT INPUT ${_warpPackage} DESTINATION ${BICAMERAL_WARP_DIR} PATTERNS build/native/bin/x64/d3d10warp.dll)
  if(NOT EXISTS ${BICAMERAL_WARP_DLL})
    message(FATAL_ERROR "WARP ${BICAMERAL_WARP_VERSION} の d3d10warp.dll を取り出せない: ${_warpPackage}")
  endif()
endif()
message(STATUS "WARP: ${BICAMERAL_WARP_VERSION}(${BICAMERAL_WARP_DLL})")

add_subdirectory(engine)
add_subdirectory(shaders)

enable_testing()
add_subdirectory(tests)

# 試作の道具(T-0016 の物理の試作)
add_subdirectory(tools/physics_lab)
