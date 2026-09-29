// hresult.cpp — HRESULT の名前と説明(FormatMessageW)を作り、失敗をログに出す。
//
// 名前の表は D3D12・DXGI でよく出るものだけ。説明文は OS の言語で返る(日本語の Windows なら日本語)。
#include "core/hresult.h"

#include <windows.h>

#include <array>
#include <type_traits>

#include "core/unicode.h"

static_assert(std::is_same_v<HRESULT, bicameral::HResult>, "HResult は HRESULT と同じ型でなければならない");

namespace bicameral {
    namespace {

        struct NamedResult {
            HRESULT code;
            std::string_view name;
        };

#define BICAMERAL_NAMED_RESULT(code) \
    NamedResult {                    \
        code, #code                  \
    }
        constexpr std::array NAMED_RESULTS = {
            // --- 一般 ---
            BICAMERAL_NAMED_RESULT(E_FAIL),
            BICAMERAL_NAMED_RESULT(E_INVALIDARG),
            BICAMERAL_NAMED_RESULT(E_OUTOFMEMORY),
            BICAMERAL_NAMED_RESULT(E_NOTIMPL),
            BICAMERAL_NAMED_RESULT(E_NOINTERFACE),
            BICAMERAL_NAMED_RESULT(E_POINTER),
            BICAMERAL_NAMED_RESULT(E_ACCESSDENIED),
            BICAMERAL_NAMED_RESULT(E_ABORT),
            BICAMERAL_NAMED_RESULT(E_UNEXPECTED),
            // --- DXGI ---
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_ACCESS_LOST),
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_DEVICE_HUNG),
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_DEVICE_REMOVED),
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_DEVICE_RESET),
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_DRIVER_INTERNAL_ERROR),
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_INVALID_CALL),
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_MORE_DATA),
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_NOT_CURRENTLY_AVAILABLE),
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_NOT_FOUND),
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_SDK_COMPONENT_MISSING),
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_UNSUPPORTED),
            BICAMERAL_NAMED_RESULT(DXGI_ERROR_WAS_STILL_DRAWING),
            // --- D3D12 ---
            BICAMERAL_NAMED_RESULT(D3D12_ERROR_ADAPTER_NOT_FOUND),
            BICAMERAL_NAMED_RESULT(D3D12_ERROR_DRIVER_VERSION_MISMATCH),
        };
#undef BICAMERAL_NAMED_RESULT

        std::string_view NameOf(HRESULT result) {
            for (const NamedResult& named : NAMED_RESULTS) {
                if (named.code == result) return named.name;
            }
            return {};
        }

        // OS が持っている説明文。末尾の改行と句点の後の空白を落とす。無ければ空
        std::string SystemMessageOf(HRESULT result) {
            wchar_t* buffer = nullptr;
            const DWORD flags =
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
            const DWORD length = FormatMessageW(flags, nullptr, static_cast<DWORD>(result), 0,
                                                reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
            if (length == 0 || buffer == nullptr) return {};

            std::wstring_view text(buffer, length);
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
                text.remove_suffix(1);
            }
            std::string message = ToUtf8(text);
            LocalFree(buffer);
            return message;
        }

    }  // namespace

    std::string DescribeHresult(HResult result) {
        const std::string_view name = NameOf(result);
        const std::string message = SystemMessageOf(result);
        std::string text = name.empty() ? std::format("0x{:08X}", static_cast<unsigned long>(result))
                                        : std::format("{} (0x{:08X})", name, static_cast<unsigned long>(result));
        if (!message.empty()) text += ": " + message;
        return text;
    }

    bool CheckHresult(HResult result, Channel channel, std::string_view expression,
                      const std::source_location& location) {
        if (SUCCEEDED(result)) return true;
        Logger& logger = GetLogger();
        if (logger.IsEnabled(Level::Error)) {
            logger.Write(channel, Level::Error, std::format("{} が失敗: {}", expression, DescribeHresult(result)),
                         location);
        }
        return false;
    }

}  // namespace bicameral
