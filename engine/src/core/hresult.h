// hresult.h — HRESULT の失敗を「呼んだ式・ファイル:行・エラー名と意味」つきでログに出す(T-0007)。
//
// 使い方:
//   if (!BICAMERAL_CHECK_HR(Channel::Gpu, device->CreateCommittedResource(...))) return false;
//   → [    0.120] E gpu       | device->CreateCommittedResource(...) が失敗: E_OUTOFMEMORY (0x8007000E): ...  (gpu.cpp:42)
//
// 失敗して当然の問い合わせ(古いランタイムでの CheckFeatureSupport など)にはマクロを使わず、
// DescribeHresult() で説明を作って Warning や Info で出す(Error はバグか環境の問題だけにする)。
// windows.h を include しないため、HRESULT と同じ型(long)を HResult として受け取る。
#pragma once

#include <source_location>
#include <string>
#include <string_view>

#include "core/log.h"

namespace bicameral {

    using HResult = long;  // HRESULT と同じ型(hresult.cpp で static_assert している)

    // "E_INVALIDARG (0x80070057): パラメーターが間違っています。" の形。名前を知らないコードは 16 進だけ
    [[nodiscard]] std::string DescribeHresult(HResult result);

    // 成功なら true。失敗なら Level::Error でログを出して false。場所は呼んだ所になる
    [[nodiscard]] bool CheckHresult(HResult result, Channel channel, std::string_view expression,
                                    const std::source_location& location = std::source_location::current());

}  // namespace bicameral

// 式を文字列にしてログに残すためにマクロにしている(関数では呼んだ API 名を手で書くことになり、ずれうる。ADR-0006)
#define BICAMERAL_CHECK_HR(channel, expression) ::bicameral::CheckHresult((expression), (channel), #expression)
