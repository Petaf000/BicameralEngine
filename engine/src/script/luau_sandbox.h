// luau_sandbox.h — Luau を CPU で安全に動かす殻(サンドボックス。T-0020、13 §2・D-318・D-320)。
//
// ゲームを作る人(ユーザー・仲間・Mod を作る人)が書く Luau は、全部この殻の中で動かす。
//   見せるもの: 決まった標準ライブラリ(base の一部・coroutine・table・string・math・utf8・bit32・buffer・vector)
//               + ホストが Seal() の前に登録した関数だけ(1 つの表にまとめる。例: `host.emit(x)`)。
//   見せないもの: ファイル・OS・時計・ネットワーク・require・debug・環境の書き換え(getfenv/setfenv)・メモリの量(gcinfo)。
// 実行には上限がある: 安全点(ループの折り返し・関数の呼び出し)の数と、使ってよいメモリの量。超えたら止めて理由を返す。
// 決定性: 同じ殻の設定・同じソース・同じ実行の順番なら、同じ結果になる(math.random は実行ごとに決まった種から。13 §2・ADR-0030)。
//   ただし表を「関数や表をキーにして」pairs で回す順番はアドレスで決まるので、ベイクの結果にその順番を使わない(13 §2)。
// データの流れ: エディタ・ツール → Create → RegisterFunction(何個でも)→ Seal → Run(ソースごと)→ 戻り値の文字列・ホスト関数の副作用。
//   パッケージ(luau_package.*。T-0138)は Run に RunOptions を渡す: require をそのパッケージの中だけで見せ、戻り値を ScriptValue で受け取る。
// ここは CPU だけ(GPU もシミュの状態も知らない。原則 1・2)。シミュに効かせるときは、ホスト関数が表やコマンドを作る(13 §1)。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "script/script_value.h"

struct lua_State;

namespace bicameral::script {

    // ホストが Luau に見せる関数。引数と戻り値は Luau の C API(lua.h)で読み書きする。
    // 登録した時の context は HostContext(state) で取り出せる
    using HostFunction = int (*)(lua_State* state);

    struct SandboxLimits {
        // --- 実行の上限(1 回の Run ごと)---
        uint64_t safepointLimit = 50'000'000;      // 安全点(ループの折り返し・呼び出し)の数。無限ループを止める
        uint64_t memoryLimitBytes = 256ull << 20;  // 殻全体で使ってよいメモリ(バイト)

        // --- 決定性 ---
        int32_t randomSeed = 0;  // 各 Run の始めに math.randomseed(randomSeed) する

        // --- 出力 ---
        // print の行き先。空ならログ(Channel::Tool)へ
        std::function<void(std::string_view chunkName, std::string_view text)> printSink;
    };

    enum class ScriptErrorKind : uint8_t {
        Compile,         // 文法の誤り
        Runtime,         // 実行中の error()・型の誤りなど
        SafepointLimit,  // 安全点の上限を超えた(無限ループなど)
        Memory,          // メモリの上限を超えた
        Usage,           // 殻の使い方の誤り(Seal の前に Run した・Seal の後に登録した)
    };

    struct ScriptError {
        ScriptErrorKind kind = ScriptErrorKind::Runtime;
        std::string message;  // "chunk:行: 内容" の形(Luau の書式)
    };

    // require で読むモジュール(パッケージの中のファイル。T-0138)
    struct ModuleFile {
        std::string chunkName;  // エラーに出る名前で、同じ Run の中でのキャッシュの鍵(例 "core/sub/util.luau")
        std::string source;
    };

    // require("名前") の名前をファイルにする。無い・許さない名前なら理由を返す(呼んだスクリプトに error として届く)
    using ModuleResolver = std::function<std::expected<ModuleFile, std::string>(std::string_view name)>;

    // Run の追加の指定(既定は T-0020 の殻のまま: require なし・戻り値は文字列だけ)
    struct RunOptions {
        const ModuleResolver*
            modules = nullptr;       // あれば、この Run の中だけ require が見える。1 つのモジュールは 1 回だけ走る
        bool captureValues = false;  // 戻り値を ScriptValue でも返す(表は中まで。関数などは error)
    };

    // 1 回の Run の結果
    struct RunResult {
        std::vector<std::string> returns;  // チャンクが return した値を tostring したもの
        std::vector<ScriptValue> values;   // captureValues のときだけ: 戻り値の木
        uint64_t safepointCount = 0;       // 使った安全点の数(重さの目安・決定性の検査)
    };

    class LuauSandbox {
    public:
        [[nodiscard]] static std::expected<std::unique_ptr<LuauSandbox>, std::string> Create(SandboxLimits limits);
        ~LuauSandbox();

        LuauSandbox(const LuauSandbox&) = delete;
        LuauSandbox& operator=(const LuauSandbox&) = delete;

        // --- 組み立て(Seal の前だけ)---

        // グローバルの表 tableName の name に関数を置く(表が無ければ作る)。context は関数の中で HostContext() で取り出す
        [[nodiscard]] std::expected<void, ScriptError> RegisterFunction(const char* tableName, const char* name,
                                                                        HostFunction function, void* context);

        // グローバルと標準ライブラリを読み取り専用にする。これ以降は Run だけ
        void Seal();

        // --- 実行(Seal の後)---

        // ソースをコンパイルして、新しいスレッド(グローバルは書き込める自分用の表。ほかの Run と混ざらない)で最後まで走らせる
        [[nodiscard]] std::expected<RunResult, ScriptError> Run(std::string_view chunkName, std::string_view source,
                                                                const RunOptions& options = {});

        // --- 状態 ---
        [[nodiscard]] bool IsSealed() const { return m_sealed; }
        [[nodiscard]] uint64_t MemoryBytes() const { return m_memoryBytes; }

        // ホスト関数の中から、登録した時の context を取り出す
        [[nodiscard]] static void* HostContext(lua_State* state);

    private:
        explicit LuauSandbox(SandboxLimits limits);

        [[nodiscard]] std::expected<void, std::string> OpenLibraries();
        void ReseedRandom();

        static void* Allocate(void* userData, void* pointer, size_t oldSize, size_t newSize);
        static void Interrupt(lua_State* state, int gc);
        static int Print(lua_State* state);
        static int Require(lua_State* state);

        [[nodiscard]] std::expected<RunResult, ScriptError> RunThread(lua_State* thread, std::string_view chunkName,
                                                                      const char* bytecode, size_t bytecodeSize,
                                                                      const RunOptions& options);
        [[nodiscard]] std::expected<RunResult, ScriptError> CollectResults(lua_State* thread,
                                                                           std::string_view chunkName,
                                                                           const RunOptions& options);

        void BeginModules(lua_State* thread, const ModuleResolver* modules);
        void EndModules();
        [[nodiscard]] std::expected<void, std::string> LoadModule(lua_State* state, std::string_view name);
        [[nodiscard]] std::expected<void, std::string> RunModule(lua_State* state, const std::string& chunkName,
                                                                 std::string_view source);

        SandboxLimits m_limits;
        lua_State* m_state = nullptr;
        bool m_sealed = false;

        // --- 上限の数え方 ---
        uint64_t m_memoryBytes = 0;
        uint64_t m_safepointCount = 0;
        bool m_safepointExceeded = false;
        bool m_memoryExceeded = false;
        std::string m_currentChunk;  // print の行き先に渡す名前

        // --- require(Run の間だけ)---
        const ModuleResolver* m_modules = nullptr;
        int m_moduleCacheRef = 0;                  // chunkName → モジュールの戻り値 の表(Luau の registry)
        std::vector<std::string> m_moduleLoading;  // 読み込み中の chunkName(循環を見つける)
    };

}  // namespace bicameral::script
