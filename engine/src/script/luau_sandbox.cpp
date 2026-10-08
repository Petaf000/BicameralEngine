// luau_sandbox.cpp — Luau の殻の実装(T-0020、13 §2・ADR-0030)。
//
// 組み立ての順番: lua_newstate(上限つきの確保)→ 許した標準ライブラリだけを開く → 見せない関数を消す・print を差し替える
//   → ホストの関数を登録 → luaL_sandbox(グローバルと標準ライブラリを読み取り専用に)。
// 実行の順番: math の種を戻す → コンパイル(luau_compile)→ 自分用のスレッド(luaL_sandboxthread)で読み込み → lua_pcall → 戻り値を文字列に。
// 上限: 安全点ごとに Interrupt が数え、超えたら error を投げる。確保は Allocate が数え、超えたら確保に失敗する(Luau がメモリの error にする)。
// Luau のヘッダは小さいので pch.h に入れず、ここだけで include する(Luau を知るのはこのファイルだけ)。
#include "script/luau_sandbox.h"

#include <cstdlib>
#include <format>
#include <string>
#include <utility>

#include "core/log.h"
#include "lua.h"
#include "luacode.h"
#include "lualib.h"

namespace bicameral::script {

    namespace {

        // --- 見せる標準ライブラリ(13 §2。os・debug は開かない。require は T-0138 のパッケージで足す)---
        struct LibraryEntry {
            const char* name;
            lua_CFunction open;
        };

        constexpr LibraryEntry ALLOWED_LIBRARIES[] = {
            {.name = "", .open = luaopen_base},
            {.name = LUA_COLIBNAME, .open = luaopen_coroutine},
            {.name = LUA_TABLIBNAME, .open = luaopen_table},
            {.name = LUA_STRLIBNAME, .open = luaopen_string},
            {.name = LUA_MATHLIBNAME, .open = luaopen_math},
            {.name = LUA_UTF8LIBNAME, .open = luaopen_utf8},
            {.name = LUA_BITLIBNAME, .open = luaopen_bit32},
            {.name = LUA_BUFFERLIBNAME, .open = luaopen_buffer},
            {.name = LUA_VECLIBNAME, .open = luaopen_vector},
        };

        // base のうち見せないもの: メモリの量(実行の環境で変わりうる)・環境の書き換え・古い仕組み
        constexpr const char* HIDDEN_GLOBALS[] = {"gcinfo", "getfenv", "setfenv", "newproxy"};

        int OpenOneLibrary(lua_State* state) {
            const auto* entry = static_cast<const LibraryEntry*>(lua_tolightuserdata(state, 1));
            lua_pushcfunction(state, entry->open, entry->name);
            lua_pushstring(state, entry->name);
            lua_call(state, 1, 0);

            return 0;
        }

        // 戻り値を、アドレスに依らない文字列にする(表・関数は型の名前だけ。決定性のため)
        std::string ValueToText(lua_State* state, int index) {
            switch (lua_type(state, index)) {
                case LUA_TNIL: return "nil";
                case LUA_TBOOLEAN: return lua_toboolean(state, index) ? "true" : "false";
                case LUA_TNUMBER:
                case LUA_TSTRING: {
                    lua_pushvalue(state, index);  // lua_tolstring は数を文字列に書き換えるので、写しで変換する
                    size_t length = 0;
                    const char* text = lua_tolstring(state, -1, &length);
                    std::string result(text, length);
                    lua_pop(state, 1);

                    return result;
                }
                default: return std::string("<") + luaL_typename(state, index) + ">";
            }
        }

        ScriptErrorKind ClassifyError(int status, bool safepointExceeded, bool memoryExceeded) {
            if (safepointExceeded)
                return ScriptErrorKind::SafepointLimit;
            if (memoryExceeded || status == LUA_ERRMEM)
                return ScriptErrorKind::Memory;

            return ScriptErrorKind::Runtime;
        }

    }  // namespace

    // --- 作る・壊す ---

    LuauSandbox::LuauSandbox(SandboxLimits limits) : m_limits(std::move(limits)) {}

    LuauSandbox::~LuauSandbox() {
        if (m_state)
            lua_close(m_state);
    }

    std::expected<std::unique_ptr<LuauSandbox>, std::string> LuauSandbox::Create(SandboxLimits limits) {
        std::unique_ptr<LuauSandbox> sandbox(new LuauSandbox(std::move(limits)));

        sandbox->m_state = lua_newstate(&LuauSandbox::Allocate, sandbox.get());
        if (!sandbox->m_state)
            return std::unexpected("Luau の状態を作れない(メモリの上限が小さすぎる?)");

        lua_Callbacks* callbacks = lua_callbacks(sandbox->m_state);
        callbacks->userdata = sandbox.get();
        callbacks->interrupt = &LuauSandbox::Interrupt;

        if (auto opened = sandbox->OpenLibraries(); !opened)
            return std::unexpected(opened.error());

        return sandbox;
    }

    std::expected<void, std::string> LuauSandbox::OpenLibraries() {
        // 開く途中の error(メモリの上限)で落ちないよう、1 つずつ保護して呼ぶ
        for (const LibraryEntry& entry : ALLOWED_LIBRARIES) {
            lua_pushcfunction(m_state, &OpenOneLibrary, "OpenOneLibrary");
            lua_pushlightuserdata(m_state, const_cast<LibraryEntry*>(&entry));
            if (lua_pcall(m_state, 1, 0, 0) != LUA_OK) {
                std::string message = lua_tostring(m_state, -1) ? lua_tostring(m_state, -1) : "?";
                lua_pop(m_state, 1);

                return std::unexpected(std::format("標準ライブラリ '{}' を開けない: {}", entry.name, message));
            }
        }

        // --- 見せない関数を消し、print を差し替える ---
        for (const char* name : HIDDEN_GLOBALS) {
            lua_pushnil(m_state);
            lua_setglobal(m_state, name);
        }

        lua_pushcfunction(m_state, &LuauSandbox::Print, "print");
        lua_setglobal(m_state, "print");

        return {};
    }

    // --- 組み立て ---

    std::expected<void, ScriptError> LuauSandbox::RegisterFunction(const char* tableName, const char* name,
                                                                   HostFunction function, void* context) {
        if (m_sealed) {
            std::string message = std::format("Seal の後に {}.{} を登録しようとした", tableName, name);

            return std::unexpected(ScriptError{.kind = ScriptErrorKind::Usage, .message = std::move(message)});
        }

        // 表を探す(無ければ作ってグローバルに置く)
        lua_getglobal(m_state, tableName);
        if (!lua_istable(m_state, -1)) {
            lua_pop(m_state, 1);
            lua_newtable(m_state);
            lua_pushvalue(m_state, -1);
            lua_setglobal(m_state, tableName);
        }

        // context を上位値に持つ閉包として置く
        lua_pushlightuserdata(m_state, context);
        lua_pushcclosure(m_state, function, name, 1);
        lua_setfield(m_state, -2, name);
        lua_pop(m_state, 1);

        return {};
    }

    void LuauSandbox::Seal() {
        if (m_sealed)
            return;

        luaL_sandbox(m_state);
        m_sealed = true;
    }

    void* LuauSandbox::HostContext(lua_State* state) {
        return lua_tolightuserdata(state, lua_upvalueindex(1));
    }

    // --- 実行 ---

    void LuauSandbox::ReseedRandom() {
        // math.random は既定で時計とアドレスから種を作る(lmathlib.cpp)。実行ごとに決まった種へ戻す
        lua_getglobal(m_state, LUA_MATHLIBNAME);
        lua_getfield(m_state, -1, "randomseed");
        lua_pushinteger(m_state, m_limits.randomSeed);
        if (lua_pcall(m_state, 1, 0, 0) != LUA_OK)
            lua_pop(m_state, 1);  // 失敗しても種は決まった値のまま(メモリの上限くらいしか起きない)

        lua_pop(m_state, 1);  // math
    }

    std::expected<RunResult, ScriptError> LuauSandbox::Run(std::string_view chunkName, std::string_view source) {
        if (!m_sealed)
            return std::unexpected(ScriptError{.kind = ScriptErrorKind::Usage, .message = "Seal の前に Run した"});

        // --- 実行ごとに戻すもの(決定性)。数を戻してから種を戻す(前の Run が上限を超えたままだと種の呼び出しが止められる)---
        m_safepointCount = 0;
        m_safepointExceeded = false;
        m_memoryExceeded = false;
        m_currentChunk = chunkName;
        ReseedRandom();
        m_safepointCount = 0;

        // --- コンパイル ---
        lua_CompileOptions options{};
        options.optimizationLevel = 1;
        options.debugLevel = 1;  // エラーに行番号を付ける

        size_t bytecodeSize = 0;
        std::unique_ptr<char, decltype(&std::free)> bytecode(
            luau_compile(source.data(), source.size(), &options, &bytecodeSize), &std::free);
        if (!bytecode)
            return std::unexpected(
                ScriptError{.kind = ScriptErrorKind::Memory, .message = "コンパイルの結果を確保できない"});

        // --- 自分用のスレッドで読み込む(グローバルへの書き込みはこのスレッドの表に入り、ほかの Run と混ざらない)---
        lua_State* thread = lua_newthread(m_state);  // m_state のスタックに積まれ、終わるまで GC から守られる
        luaL_sandboxthread(thread);

        const std::string luauChunkName = "=" + std::string(chunkName);
        if (luau_load(thread, luauChunkName.c_str(), bytecode.get(), bytecodeSize, 0) != LUA_OK) {
            ScriptError error{.kind = ScriptErrorKind::Compile, .message = ValueToText(thread, -1)};
            lua_pop(m_state, 1);

            return std::unexpected(std::move(error));
        }

        // --- 走らせる ---
        const int status = lua_pcall(thread, 0, LUA_MULTRET, 0);
        if (status != LUA_OK) {
            ScriptError error{.kind = ClassifyError(status, m_safepointExceeded, m_memoryExceeded),
                              .message = ValueToText(thread, -1)};
            lua_pop(m_state, 1);

            // 上限で止まった時のごみをすぐ片付ける(Luau は確保の失敗で GC を走らせないので、次の Run が巻き添えで止まらないように)
            if (error.kind == ScriptErrorKind::Memory)
                lua_gc(m_state, LUA_GCCOLLECT, 0);

            return std::unexpected(std::move(error));
        }

        // --- 戻り値 ---
        RunResult result;
        result.safepointCount = m_safepointCount;

        const int returnCount = lua_gettop(thread);
        for (int index = 1; index <= returnCount; ++index)
            result.returns.push_back(ValueToText(thread, index));

        lua_pop(m_state, 1);  // スレッド

        return result;
    }

    // --- Luau から呼ばれるもの ---

    void* LuauSandbox::Allocate(void* userData, void* pointer, size_t oldSize, size_t newSize) {
        auto* self = static_cast<LuauSandbox*>(userData);
        const size_t previousSize = pointer ? oldSize : 0;

        if (newSize == 0) {
            std::free(pointer);
            self->m_memoryBytes -= previousSize;

            return nullptr;
        }

        // 増えるときだけ上限を見る(減らす・同じ大きさは必ず通す)
        const uint64_t nextBytes = self->m_memoryBytes - previousSize + newSize;
        if (newSize > previousSize && nextBytes > self->m_limits.memoryLimitBytes) {
            self->m_memoryExceeded = true;

            return nullptr;
        }

        void* resized = std::realloc(pointer, newSize);
        if (!resized)
            return nullptr;

        self->m_memoryBytes = nextBytes;

        return resized;
    }

    void LuauSandbox::Interrupt(lua_State* state, int gc) {
        if (gc >= 0)
            return;  // GC の段の通知(安全点ではない)

        auto* self = static_cast<LuauSandbox*>(lua_callbacks(state)->userdata);
        ++self->m_safepointCount;
        if (self->m_safepointCount <= self->m_limits.safepointLimit)
            return;

        // 超えた後は安全点ごとに投げ続ける(pcall で捕まえても外側のループで止まる)
        self->m_safepointExceeded = true;
        const std::string message = std::format("安全点の上限({})を超えた", self->m_limits.safepointLimit);
        luaL_error(state, "%s", message.c_str());
    }

    int LuauSandbox::Print(lua_State* state) {
        auto* self = static_cast<LuauSandbox*>(lua_callbacks(state)->userdata);

        std::string text;
        const int argumentCount = lua_gettop(state);
        for (int index = 1; index <= argumentCount; ++index) {
            if (index > 1)
                text += '\t';
            text += ValueToText(state, index);
        }

        if (self->m_limits.printSink)
            self->m_limits.printSink(self->m_currentChunk, text);
        else
            Log(Channel::Tool, Level::Info, "[luau {}] {}", self->m_currentChunk, text);

        return 0;
    }

}  // namespace bicameral::script
