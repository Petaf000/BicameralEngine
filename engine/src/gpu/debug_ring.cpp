// debug_ring.cpp — シェーダーの printf / assert のリングを読み戻してログに出す(T-0003、docs/design/16-debug-test.md §1)。
// レコードのレイアウトは shaders/common/debug_ring.hlsli、書式の一覧は shaders/common/debug_formats.hlsli。
// 書式は std::format の書式だが、引数の型(符号の有無・32 / 64bit)は実行時に分かるので、DebugArgument の formatter が
// 書式の指定({:#x} など)を覚えておき、値を本来の型に戻してから当てはめる。
#include "gpu/debug_ring.h"

#include <array>

#include "gpu/resources.h"

namespace bicameral::gpu {

    // 1 つの引数: 64bit に広げた値と、元の型(DEBUG_ARG_*)
    struct DebugArgument {
        uint64_t bits = 0;
        uint32_t kind = DEBUG_ARG_U32;
    };

}  // namespace bicameral::gpu

// 書式の指定(":" の後ろ)を parse で覚え、format で元の型の値に同じ指定を当てはめる
template <>
struct std::formatter<bicameral::gpu::DebugArgument, char> {
    std::string_view spec;  // 書式の指定(":" の後ろ、"}" の前)

    // NOLINTNEXTLINE(readability-identifier-naming) std::formatter が要求する名前
    constexpr std::format_parse_context::iterator parse(std::format_parse_context& context) {
        const auto end = std::find(context.begin(), context.end(), '}');
        spec = std::string_view(context.begin(), end);

        return end;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) std::formatter が要求する名前
    std::format_context::iterator format(const bicameral::gpu::DebugArgument& argument,
                                         std::format_context& context) const {
        using namespace bicameral;
        const std::string pattern = std::format("{{:{}}}", spec);
        switch (argument.kind) {
            case DEBUG_ARG_U32: {
                auto value = static_cast<uint32_t>(argument.bits);
                return std::vformat_to(context.out(), pattern, std::make_format_args(value));
            }
            case DEBUG_ARG_I32: {
                auto value = static_cast<int32_t>(static_cast<uint32_t>(argument.bits));
                return std::vformat_to(context.out(), pattern, std::make_format_args(value));
            }
            case DEBUG_ARG_U64: {
                auto value = argument.bits;
                return std::vformat_to(context.out(), pattern, std::make_format_args(value));
            }
            default: {
                auto value = static_cast<int64_t>(argument.bits);
                return std::vformat_to(context.out(), pattern, std::make_format_args(value));
            }
        }
    }
};

namespace bicameral::gpu {
    namespace {

        // --- 書式の一覧(debug_formats.hlsli を C++ の表にする)---

        struct DebugFormatEntry {
            Channel channel;
            std::string_view where;
            std::string_view text;
        };

        constexpr DebugFormatEntry DEBUG_FORMATS[] = {
#define DEBUG_FORMAT(name, channel, where, text) {Channel::channel, where, text},
#include "common/debug_formats.hlsli"
#include "gpu/com_ptr.h"
#undef DEBUG_FORMAT
        };

        static_assert(std::size(DEBUG_FORMATS) == static_cast<size_t>(DebugFormat::Count),
                      "debug_formats.hlsli と DebugFormat の数が合わない");

        using DebugArguments = std::array<DebugArgument, DEBUG_RECORD_MAX_ARGS>;

        // 書式に当てはめられないときに、引数を型どおりに並べる
        std::string RawArguments(const DebugArguments& arguments, uint32_t argCount) {
            std::string text;
            for (uint32_t index = 0; index < argCount; ++index)
                text += std::format("{}{}", index == 0 ? "" : ", ", arguments[index]);

            return "[" + text + "]";
        }

        // シェーダーが書いた数だけの引数で当てはめる(書式より少なければ format_error。多いのは構わない)
        std::string VFormatArguments(std::string_view pattern, const DebugArguments& a, uint32_t argCount) {
            switch (argCount) {
                case 0: return std::vformat(pattern, std::make_format_args());
                case 1: return std::vformat(pattern, std::make_format_args(a[0]));
                case 2: return std::vformat(pattern, std::make_format_args(a[0], a[1]));
                case 3: return std::vformat(pattern, std::make_format_args(a[0], a[1], a[2]));
                case 4: return std::vformat(pattern, std::make_format_args(a[0], a[1], a[2], a[3]));
                case 5: return std::vformat(pattern, std::make_format_args(a[0], a[1], a[2], a[3], a[4]));
                default: return std::vformat(pattern, std::make_format_args(a[0], a[1], a[2], a[3], a[4], a[5]));
            }
        }

        std::string FormatArguments(std::string_view pattern, const DebugArguments& arguments, uint32_t argCount) {
            try {
                return VFormatArguments(pattern, arguments, argCount);
            } catch (const std::format_error& error) {
                return std::format("(書式 \"{}\" に当てはまらない: {}) {}", pattern, error.what(),
                                   RawArguments(arguments, argCount));
            }
        }

        // --- ログに出す ---

        void LogMessage(const DebugMessage& message) {
            Log(message.channel, message.isAssert ? Level::Error : Level::Info, "GPU {} {}:{} {}",
                message.isAssert ? "assert" : "print", message.where, message.line, message.text);
        }

        void LogSummary(const DebugRingContents& contents, uint32_t loggedCount) {
            const size_t notLogged = contents.messages.size() - loggedCount;
            if (notLogged > 0) {
                Log(Channel::Gpu, Level::Info,
                    "GPU のデバッグ出力: ほか {} 件はログに出していない(全 {} 件、assert {} 件)", notLogged,
                    contents.messages.size(), contents.assertCount);
            }

            if (contents.droppedCount > 0) {
                Log(Channel::Gpu, Level::Warning, "GPU のデバッグ出力: リングが溢れて {} 件を落とした(容量 {} 件)",
                    contents.droppedCount, DEBUG_RING_CAPACITY);
            }
        }

    }  // namespace

    // --- 1 レコードを読む ---

    DebugMessage DecodeDebugRecord(std::span<const uint32_t, DEBUG_RECORD_WORDS> record) {
        DebugMessage message;
        const uint32_t formatIndex = record[0];
        const uint32_t argCount = std::min((record[1] >> 4) & 0xFu, DEBUG_RECORD_MAX_ARGS);
        const uint32_t argKinds = record[1] >> 8;
        message.isAssert = (record[1] & 0xFu) == DEBUG_KIND_ASSERT;
        message.line = record[2];

        DebugArguments arguments{};
        for (uint32_t index = 0; index < argCount; ++index) {
            const uint32_t word = DEBUG_RECORD_ARG_WORD + index * 2;
            arguments[index] = {.bits = record[word] | (uint64_t{record[word + 1]} << 32),
                                .kind = (argKinds >> (index * 2)) & 0x3u};
        }

        if (formatIndex >= std::size(DEBUG_FORMATS)) {
            message.where = "?";
            message.text = std::format("(知らない書式の番号 {}) {}", formatIndex, RawArguments(arguments, argCount));

            return message;
        }

        const DebugFormatEntry& entry = DEBUG_FORMATS[formatIndex];
        message.format = static_cast<DebugFormat>(formatIndex);
        message.channel = entry.channel;
        message.where = entry.where;
        message.text = FormatArguments(entry.text, arguments, argCount);

        return message;
    }

    // --- DebugRing ---

    std::expected<DebugRing, std::string> DebugRing::Create(ID3D12Device* device, uint32_t slotCount) {
        auto ring = ReadbackRing::Create(device, DEBUG_RING_BYTES, DEBUG_RING_HEADER_BYTES, slotCount, L"DebugRing");
        if (!ring)
            return std::unexpected("デバッグのリングを作れない: " + ring.error());

        return DebugRing(std::move(*ring));
    }

    void DebugRing::RecordBegin(ID3D12GraphicsCommandList* list) const {
        m_ring.RecordBegin(list);
    }

    void DebugRing::RecordReadbackAndReset(ID3D12GraphicsCommandList* list, uint32_t slot) const {
        m_ring.RecordReadbackAndReset(list, slot);
    }

    DebugRingContents DebugRing::Drain(uint32_t maxLoggedMessages, uint32_t slot) const {
        DebugRingContents contents;
        // 見出しを先に読み、書かれた分だけを読む(ふつうは 0 件。毎フレーム 256 KiB を写さない)
        uint32_t requested = 0;
        if (!m_ring.Read(slot, std::as_writable_bytes(std::span(&requested, 1))))
            return contents;

        contents.requestedCount = requested;
        const uint32_t storedCount = std::min(contents.requestedCount, DEBUG_RING_CAPACITY);
        std::vector<uint32_t> words((DEBUG_RING_HEADER_BYTES + storedCount * DEBUG_RECORD_BYTES) / 4);
        if (storedCount > 0 && !m_ring.Read(slot, std::as_writable_bytes(std::span(words))))
            return contents;

        contents.droppedCount = contents.requestedCount - storedCount;
        contents.messages.reserve(storedCount);
        const std::span<const uint32_t> records = std::span(words).subspan(DEBUG_RING_HEADER_BYTES / 4);

        for (uint32_t index = 0; index < storedCount; ++index) {
            const auto record = records.subspan(size_t{index} * DEBUG_RECORD_WORDS).first<DEBUG_RECORD_WORDS>();
            DebugMessage message = DecodeDebugRecord(record);
            if (message.isAssert)
                ++contents.assertCount;

            contents.messages.push_back(std::move(message));
        }

        // assert を先に出す(print が多いと埋もれるので)
        uint32_t loggedCount = 0;
        for (const bool assertPass : {true, false}) {
            for (const DebugMessage& message : contents.messages) {
                if (message.isAssert != assertPass || loggedCount >= maxLoggedMessages)
                    continue;

                LogMessage(message);
                ++loggedCount;
            }
        }

        LogSummary(contents, loggedCount);

        return contents;
    }

}  // namespace bicameral::gpu
