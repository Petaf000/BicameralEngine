// pch.h — プリコンパイルヘッダ。めったに変わらない外部のヘッダだけを集める(docs/style.md「ビルド」)。
// CMake の target_precompile_headers が各 .cpp の先頭に自動で入れるので、.cpp 側では include しなくてよい。
// 自分たちのヘッダはここに入れない(変えるたびに全部のビルドがやり直しになる)。
#pragma once

// --- Windows / DirectX ---
#include <windows.h>
#include <winver.h>  // GetFileVersionInfoW(読み込んだ WARP の版。gpu/device.cpp)

#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>  // debug layer の報告(ID3D12InfoQueue1)と DRED
#include <directx/d3dx12.h>          // 状態オブジェクト(Work Graphs)の組み立て
#include <dxgi1_6.h>
#include <wrl/client.h>

// --- 標準ライブラリ ---
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
