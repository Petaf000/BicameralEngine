# ランタイム(ゲーム本体)。世界の状態は VRAM に常駐し、CPU は窓・入力・Present・音の出力・セーブ I/O だけを担う。
add_executable(bicameral
  src/main.cpp
  src/platform/caps.cpp
)
target_include_directories(bicameral PRIVATE src)
target_link_libraries(bicameral PRIVATE
  Microsoft::DirectX-Headers Microsoft::DirectX-Guids
  d3d12 dxgi)
if(BICAMERAL_GPU_VALIDATION)
  target_compile_definitions(bicameral PRIVATE BICAMERAL_GPU_VALIDATION=1)
endif()
if(BICAMERAL_PIX)
  target_compile_definitions(bicameral PRIVATE BICAMERAL_PIX=1)
endif()
