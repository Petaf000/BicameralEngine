// screenshot.cpp — バックバッファを BMP に書く。使い方は screenshot.h。
#include "render/screenshot.h"

#include <array>
#include <fstream>
#include <vector>

#include "gpu/resources.h"

namespace bicameral::render {
    namespace {

        constexpr uint32_t BYTES_PER_PIXEL = 4;
        constexpr uint32_t FILE_HEADER_BYTES = 14;
        constexpr uint32_t INFO_HEADER_BYTES = 40;

        // 小さい方から順に書く(BMP の数はすべてリトルエンディアン)
        void AppendLittleEndian(std::vector<uint8_t>& bytes, uint32_t value, uint32_t size) {
            for (uint32_t index = 0; index < size; ++index) {
                bytes.push_back(static_cast<uint8_t>(value >> (8 * index)));
            }
        }

        // BITMAPFILEHEADER + BITMAPINFOHEADER(高さを負にして上から下の並び)
        std::vector<uint8_t> BmpHeader(uint32_t width, uint32_t height) {
            const uint32_t pixelBytes = width * height * BYTES_PER_PIXEL;
            std::vector<uint8_t> header;
            header.push_back('B');
            header.push_back('M');
            AppendLittleEndian(header, FILE_HEADER_BYTES + INFO_HEADER_BYTES + pixelBytes, 4);
            AppendLittleEndian(header, 0, 4);  // 予約
            AppendLittleEndian(header, FILE_HEADER_BYTES + INFO_HEADER_BYTES, 4);
            AppendLittleEndian(header, INFO_HEADER_BYTES, 4);
            AppendLittleEndian(header, width, 4);
            AppendLittleEndian(header, static_cast<uint32_t>(-static_cast<int32_t>(height)), 4);
            AppendLittleEndian(header, 1, 2);   // 面
            AppendLittleEndian(header, 32, 2);  // bit/画素
            AppendLittleEndian(header, 0, 4);   // BI_RGB(圧縮なし)
            AppendLittleEndian(header, pixelBytes, 4);
            AppendLittleEndian(header, 2835, 4);  // 72 dpi
            AppendLittleEndian(header, 2835, 4);
            AppendLittleEndian(header, 0, 4);
            AppendLittleEndian(header, 0, 4);
            return header;
        }

    }  // namespace

    std::expected<ScreenshotCapture, std::string> ScreenshotCapture::Create(ID3D12Device* device,
                                                                            ID3D12Resource* backBuffer) {
        ScreenshotCapture capture;
        const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            return std::unexpected("画像に書けるのは RGBA8 のバックバッファだけ");
        }
        uint64_t totalBytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &capture.m_footprint, nullptr, nullptr, &totalBytes);
        capture.m_width = static_cast<uint32_t>(desc.Width);
        capture.m_height = desc.Height;
        capture.m_readback = gpu::CreateBuffer(device, totalBytes, gpu::BufferKind::Readback);
        if (!capture.m_readback ||
            FAILED(
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&capture.m_allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, capture.m_allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&capture.m_list)))) {
            return std::unexpected("画像の読み戻しを作れない");
        }
        capture.m_readback->SetName(L"Screenshot.readback");
        if (FAILED(capture.m_list->Close())) return std::unexpected("画像のリストを閉じられない");
        return capture;
    }

    ID3D12CommandList* ScreenshotCapture::Record(ID3D12Resource* backBuffer) {
        if (FAILED(m_allocator->Reset()) || FAILED(m_list->Reset(m_allocator.Get(), nullptr))) return nullptr;
        const D3D12_RESOURCE_BARRIER toCopy =
            gpu::Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_list->ResourceBarrier(1, &toCopy);
        const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = m_readback.Get(),
                                                      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                      .PlacedFootprint = m_footprint};
        const D3D12_TEXTURE_COPY_LOCATION source{
            .pResource = backBuffer, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
        m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        const D3D12_RESOURCE_BARRIER toPresent =
            gpu::Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        m_list->ResourceBarrier(1, &toPresent);
        return SUCCEEDED(m_list->Close()) ? m_list.Get() : nullptr;
    }

    std::expected<void, std::string> ScreenshotCapture::WriteBmp(const std::filesystem::path& path) const {
        void* mapped = nullptr;
        if (FAILED(m_readback->Map(0, nullptr, &mapped))) return std::unexpected("画像の読み戻しを開けない");
        const auto* source = static_cast<const uint8_t*>(mapped);
        std::vector<uint8_t> file = BmpHeader(m_width, m_height);
        file.reserve(file.size() + size_t{m_width} * m_height * BYTES_PER_PIXEL);
        for (uint32_t row = 0; row < m_height; ++row) {
            const uint8_t* line = source + m_footprint.Offset + size_t{row} * m_footprint.Footprint.RowPitch;
            for (uint32_t column = 0; column < m_width; ++column) {
                const uint8_t* pixel = line + size_t{column} * BYTES_PER_PIXEL;
                file.insert(file.end(), {pixel[2], pixel[1], pixel[0], 255});  // RGBA → BGRA
            }
        }
        const D3D12_RANGE noWrite{};
        m_readback->Unmap(0, &noWrite);

        std::ofstream stream(path, std::ios::binary);
        if (!stream) return std::unexpected("画像のファイルを開けない");
        stream.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        if (!stream) return std::unexpected("画像のファイルに書けない");
        return {};
    }

}  // namespace bicameral::render
