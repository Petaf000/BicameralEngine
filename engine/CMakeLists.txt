# --- core: どのサブシステムからも使う小さな部品(シングルトンの破棄順・ログ・HRESULT・文字コード)。他のサブシステムを知らない ---
add_library(bicameral_core STATIC
  src/core/hresult.cpp
  src/core/log.cpp
  src/core/log_sinks.cpp
  src/core/singleton.cpp
  src/core/unicode.cpp
)
target_include_directories(bicameral_core PUBLIC src)

# ランタイム(ゲーム本体)。世界の状態は VRAM に常駐し、CPU は窓・入力・Present・音の出力・セーブ I/O だけを担う。
add_executable(bicameral
  src/main.cpp
  src/platform/agility_sdk.cpp
  src/platform/caps.cpp
)
target_include_directories(bicameral PRIVATE src)
target_precompile_headers(bicameral PRIVATE src/pch.h)  # 重い外部ヘッダをまとめる(docs/style.md)
target_link_libraries(bicameral PRIVATE
  bicameral_core
  Microsoft::DirectX-Headers Microsoft::DirectX-Guids
  Microsoft::DirectX12-Agility
  d3d12 dxgi)

# --- Agility SDK の配置(ADR-0004)-------------------------------------------------------------
# exe の横の D3D12\ に D3D12Core.dll を置く。exe は agility_sdk.cpp でその場所と版をエクスポートし、
# OS の d3d12.dll が起動時にこちらのランタイムを読み込む。デバッグレイヤーも同じ版を使う。
add_custom_command(TARGET bicameral POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:bicameral>/D3D12
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_PROPERTY:Microsoft::DirectX12-Core,IMPORTED_LOCATION_RELEASE>
          $<TARGET_PROPERTY:Microsoft::DirectX12-Layers,IMPORTED_LOCATION_DEBUG>
          $<TARGET_FILE_DIR:bicameral>/D3D12
  COMMAND_EXPAND_LISTS
  COMMENT "Agility SDK の D3D12Core.dll を bin/D3D12 にコピー")
if(BICAMERAL_GPU_VALIDATION)
  target_compile_definitions(bicameral PRIVATE BICAMERAL_GPU_VALIDATION=1)
endif()
if(BICAMERAL_PIX)
  target_compile_definitions(bicameral PRIVATE BICAMERAL_PIX=1)
endif()
