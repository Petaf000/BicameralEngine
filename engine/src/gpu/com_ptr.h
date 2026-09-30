// com_ptr.h — COM の参照を数える賢いポインタ(Microsoft::WRL::ComPtr)を ComPtr と書くための別名(docs/style.md「短い名前」)。
// D3D12・DXGI を使うファイルが読む。GPU を知らない所(core・bicameral_view)からは読まない。
#pragma once

#include <wrl/client.h>

namespace bicameral {

    using Microsoft::WRL::ComPtr;

}  // namespace bicameral
