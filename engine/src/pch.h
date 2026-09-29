// pch.h — プリコンパイルヘッダ。めったに変わらない外部のヘッダだけを集める(docs/style.md「ビルド」)。
// CMake の target_precompile_headers が各 .cpp の先頭に自動で入れるので、.cpp 側では include しなくてよい。
// 自分たちのヘッダはここに入れない(変えるたびに全部のビルドがやり直しになる)。
#pragma once

// --- Windows / DirectX ---
#include <windows.h>

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// --- 標準ライブラリ ---
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>
