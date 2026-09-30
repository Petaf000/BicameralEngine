// screenshot.h — バックバッファを画像ファイル(BMP)に書く(T-0015。10「テスト」の画像比較の入口)。
//
// データの流れ: フレームのループが最後のフレームの描画の後・Present の前に Record() で「バックバッファ → 読み戻しのバッファ」を
// direct キューに積む → GPU が終わってから(フレームのループの終わりの Flush の後)WriteBmp() がファイルに書く。
// 外部のライブラリを使わないため BMP(32bit、上から下)。比較・変換は tools 側で行う。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace bicameral::render {

    class ScreenshotCapture {
    public:
        // backBuffer と同じ大きさの読み戻しのバッファと、写すためのコマンドリストを作る
        [[nodiscard]] static std::expected<ScreenshotCapture, std::string> Create(ID3D12Device* device,
                                                                                  ID3D12Resource* backBuffer);

        // PRESENT のバックバッファを写すリストを記録して返す(Present の前に direct キューへ投げる)
        [[nodiscard]] ID3D12CommandList* Record(ID3D12Resource* backBuffer);

        // 写したリストを GPU が終えた後に呼ぶ(RGBA8 → BMP の BGRA)
        [[nodiscard]] std::expected<void, std::string> WriteBmp(const std::filesystem::path& path) const;

    private:
        ScreenshotCapture() = default;

        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_footprint{};
        uint32_t m_width = 0;
        uint32_t m_height = 0;
    };

}  // namespace bicameral::render
