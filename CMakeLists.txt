cmake_minimum_required(VERSION 3.25)
project(BicameralEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(BICAMERAL_GPU_VALIDATION "D3D12 debug layer と GPU-based validation を有効にする" OFF)
option(BICAMERAL_PIX "PIX イベントマーカーを有効にする" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /MP)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

find_package(directx-headers CONFIG REQUIRED)

add_subdirectory(engine)

enable_testing()
add_subdirectory(tests)
