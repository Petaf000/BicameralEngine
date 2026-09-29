// agility_sdk.cpp — DirectX 12 Agility SDK を使うための 2 つのエクスポート(ADR-0004)。
//
// OS の d3d12.dll は、起動した exe が次の 2 つをエクスポートしていれば、
// OS 標準のランタイムではなく D3D12SDKPath にある D3D12Core.dll を読み込む。
//   D3D12SDKVersion : 使う Agility SDK の版(ヘッダの D3D12_SDK_VERSION と一致させる)
//   D3D12SDKPath    : exe からの相対パス。D3D12Core.dll は CMake の POST_BUILD でここへコピーする
//
// こうすると、OS の更新に左右されずにヘッダと同じ版のランタイムで動き、
// Work Graphs が入っていない古い Windows でも同じ機能が使える。
#include <directx/d3d12.h>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
