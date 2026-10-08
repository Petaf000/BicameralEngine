// luau_sandbox.cpp — Luau の殻の実装(T-0020、13 §2・ADR-0030)。
//
// 組み立ての順番: lua_newstate(上限つきの確保)→ 許した標準ライブラリだけを開く → 見せない関数を消す・print を差し替える
//   → ホストの関数を登録 → luaL_sandbox(グローバルと標準ライブラリを読み取り専用に)。
// 実行の順番: math の種を戻す → コンパイル(luau_compile)→ 自分用のスレッド(luaL_sandboxthread)で読み込み → lua_pcall → 戻り値を文字列に。
// 上限: 安全点ごとに Interrupt が数え、超えたら error を投げる。確保は Allocate が数え、超えたら確保に失敗する(Luau がメモリの error にする)。
// require(T-0138): RunOptions::modules がある Run だけ、そのスレッドのグローバルに置く。モジュールは自分用の環境(読むときは Run のグローバルへ
//   抜ける表)で 1 回だけ走り、戻り値を Run の間だけキャッシュする。名前からファイルへの解決は呼び手(パッケージ)が決める。
// Luau のヘッダは小さいので pch.h に入れず、ここだけで include する(Luau を知るのはこのファイルだけ)。
#include "script/luau_sandbox.h"

#include <algorithm>
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

        // --- 見せる標準ライブラリ(13 §2。os・debug は開かない。require は RunOptions::modules がある Run だけ。T-0138)---
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

        // --- Luau の値 → ScriptValue(表は中まで。鍵は数か文字列。関数などは持てない)---
        constexpr size_t MAX_VALUE_DEPTH = 64;

        std::expected<ScriptValue, std::string> ReadValue(lua_State* state, int index, std::vector<const void*>& path);

        std::expected<ScriptValue, std::string> ReadKey(lua_State* state, int index) {
            if (lua_type(state, index) == LUA_TSTRING) {
                size_t length = 0;
                const char* text = lua_tolstring(state, index, &length);

                return ScriptValue::MakeString(std::string(text, length));
            }

            if (lua_type(state, index) == LUA_TNUMBER)
                return ScriptValue::MakeNumber(lua_tonumber(state, index));  // NaN は Luau の表の鍵になれない

            return std::unexpected(
                std::format("表の鍵にできるのは数と文字列だけ({} があった)", luaL_typename(state, index)));
        }

        std::expected<ScriptValue, std::string> ReadTable(lua_State* state, int index, std::vector<const void*>& path) {
            const void* identity = lua_topointer(state, index);
            if (path.size() >= MAX_VALUE_DEPTH)
                return std::unexpected(std::format("表が深すぎる({} 段まで)", MAX_VALUE_DEPTH));
            if (std::ranges::find(path, identity) != path.end())
                return std::unexpected("表が自分自身を含んでいる(循環)");
            if (!lua_checkstack(state, 3))
                return std::unexpected("Luau のスタックが足りない");

            path.push_back(identity);
            std::vector<ScriptField> fields;

            // lua_next の順番はアドレスと挿入の履歴で決まるので、集めてから並べ直す(MakeTable)
            lua_pushnil(state);
            while (lua_next(state, index) != 0) {
                auto key = ReadKey(state, -2);
                auto value = key ? ReadValue(state, lua_gettop(state), path)
                                 : std::expected<ScriptValue, std::string>{};
                if (!key || !value) {
                    lua_pop(state, 2);
                    path.pop_back();

                    return std::unexpected(!key ? key.error() : value.error());
                }

                fields.push_back(ScriptField{.key = std::move(*key), .value = std::move(*value)});
                lua_pop(state, 1);
            }

            path.pop_back();

            return ScriptValue::MakeTable(std::move(fields));
        }

        std::expected<ScriptValue, std::string> ReadValue(lua_State* state, int index, std::vector<const void*>& path) {
            switch (lua_type(state, index)) {
                case LUA_TNIL: return ScriptValue{};
                case LUA_TBOOLEAN: return ScriptValue::MakeBoolean(lua_toboolean(state, index) != 0);
                case LUA_TSTRING: return ReadKey(state, index);
                case LUA_TTABLE: return ReadTable(state, index, path);
                case LUA_TNUMBER: {
                    const double number = lua_tonumber(state, index);
                    if (number != number)
                        return std::unexpected("NaN は表にできない(ビット列が機種で変わる)");

                    return ScriptValue::MakeNumber(number);
                }
                default:
                    return std::unexpected(
                        std::format("{} は表にできない(数・文字列・真偽・表だけ)", luaL_typename(state, index)));
            }
        }

        // --- コンパイル(luau_compile は文法の誤りもバイトコードに入れて返し、luau_load が失敗する)---
        using Bytecode = std::unique_ptr<char, decltype(&std::free)>;

        Bytecode Compile(std::string_view source, size_t& bytecodeSize) {
            lua_CompileOptions options{};
            options.optimizationLevel = 1;
            options.debugLevel = 1;  // エラーに行番号を付ける

            return Bytecode(luau_compile(source.data(), source.size(), &options, &bytecodeSize), &std::free);
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

    std::expected<RunResult, ScriptError> LuauSandbox::Run(std::string_view chunkName, std::string_view source,
                                                           const RunOptions& options) {
        if (!m_sealed)
            return std::unexpected(ScriptError{.kind = ScriptErrorKind::Usage, .message = "Seal の前に Run した"});

        // --- 実行ごとに戻すもの(決定性)。数を戻してから種を戻す(前の Run が上限を超えたままだと種の呼び出しが止められる)---
        m_safepointCount = 0;
        m_safepointExceeded = false;
        m_memoryExceeded = false;
        m_currentChunk = chunkName;
        ReseedRandom();
        m_safepointCount = 0;

        size_t bytecodeSize = 0;
        const Bytecode bytecode = Compile(source, bytecodeSize);
        if (!bytecode)
            return std::unexpected(
                ScriptError{.kind = ScriptErrorKind::Memory, .message = "コンパイルの結果を確保できない"});

        // --- 自分用のスレッドで読み込む(グローバルへの書き込みはこのスレッドの表に入り、ほかの Run と混ざらない)---
        lua_State* thread = lua_newthread(m_state);  // m_state のスタックに積まれ、終わるまで GC から守られる
        luaL_sandboxthread(thread);
        BeginModules(thread, options.modules);

        auto result = RunThread(thread, chunkName, bytecode.get(), bytecodeSize, options);

        EndModules();
        lua_pop(m_state, 1);  // スレッド

        // 上限で止まった時のごみをすぐ片付ける(Luau は確保の失敗で GC を走らせないので、次の Run が巻き添えで止まらないように)
        if (!result && result.error().kind == ScriptErrorKind::Memory)
            lua_gc(m_state, LUA_GCCOLLECT, 0);

        return result;
    }

    std::expected<RunResult, ScriptError> LuauSandbox::RunThread(lua_State* thread, std::string_view chunkName,
                                                                 const char* bytecode, size_t bytecodeSize,
                                                                 const RunOptions& options) {
        const std::string luauChunkName = "=" + std::string(chunkName);
        if (luau_load(thread, luauChunkName.c_str(), bytecode, bytecodeSize, 0) != LUA_OK)
            return std::unexpected(ScriptError{.kind = ScriptErrorKind::Compile, .message = ValueToText(thread, -1)});

        const int status = lua_pcall(thread, 0, LUA_MULTRET, 0);
        if (status != LUA_OK) {
            const ScriptErrorKind kind = ClassifyError(status, m_safepointExceeded, m_memoryExceeded);

            return std::unexpected(ScriptError{.kind = kind, .message = ValueToText(thread, -1)});
        }

        return CollectResults(thread, chunkName, options);
    }

    std::expected<RunResult, ScriptError> LuauSandbox::CollectResults(lua_State* thread, std::string_view chunkName,
                                                                      const RunOptions& options) {
        RunResult result;
        result.safepointCount = m_safepointCount;

        const int returnCount = lua_gettop(thread);
        for (int index = 1; index <= returnCount; ++index) {
            result.returns.push_back(ValueToText(thread, index));
            if (!options.captureValues)
                continue;

            std::vector<const void*> path;
            auto value = ReadValue(thread, index, path);
            if (!value) {
                std::string message = std::format("{}: 戻り値 {}: {}", chunkName, index, value.error());

                return std::unexpected(ScriptError{.kind = ScriptErrorKind::Runtime, .message = std::move(message)});
            }

            result.values.push_back(std::move(*value));
        }

        return result;
    }

    // --- require(T-0138。RunOptions::modules がある Run の間だけ)---

    void LuauSandbox::BeginModules(lua_State* thread, const ModuleResolver* modules) {
        m_modules = modules;
        m_moduleLoading.clear();
        if (!modules)
            return;

        lua_newtable(m_state);
        m_moduleCacheRef = lua_ref(m_state, -1);
        lua_pop(m_state, 1);

        // Run のスレッドのグローバル(書き込める自分用の表)にだけ置く。殻のグローバルには無いまま
        lua_pushcfunction(thread, &LuauSandbox::Require, "require");
        lua_setglobal(thread, "require");
    }

    void LuauSandbox::EndModules() {
        if (m_moduleCacheRef != 0)
            lua_unref(m_state, m_moduleCacheRef);

        m_moduleCacheRef = 0;
        m_modules = nullptr;
        m_moduleLoading.clear();
    }

    int LuauSandbox::Require(lua_State* state) {
        const char* name = luaL_checkstring(state, 1);
        auto* self = static_cast<LuauSandbox*>(lua_callbacks(state)->userdata);

        // C++ のものを片付けてから error を投げる
        bool failed = false;
        {
            auto loaded = self->LoadModule(state, name);
            if (!loaded) {
                lua_pushlstring(state, loaded.error().data(), loaded.error().size());
                failed = true;
            }
        }

        if (failed)
            lua_error(state);

        return 1;
    }

    std::expected<void, std::string> LuauSandbox::LoadModule(lua_State* state, std::string_view name) {
        if (!m_modules)
            return std::unexpected("require はパッケージの中だけで使える");

        auto file = (*m_modules)(name);
        if (!file)
            return std::unexpected(std::format("require(\"{}\"): {}", name, file.error()));

        // --- 読み込み済みならキャッシュから ---
        lua_getref(state, m_moduleCacheRef);  // [cache]
        lua_getfield(state, -1, file->chunkName.c_str());
        if (!lua_isnil(state, -1)) {
            lua_remove(state, -2);

            return {};
        }

        lua_pop(state, 1);  // [cache]
        if (std::ranges::find(m_moduleLoading, file->chunkName) != m_moduleLoading.end()) {
            lua_pop(state, 1);
            std::string chain;
            for (const std::string& loading : m_moduleLoading)
                chain += loading + " → ";

            return std::unexpected(std::format("require の循環: {}{}", chain, file->chunkName));
        }

        return RunModule(state, file->chunkName, file->source);
    }

    std::expected<void, std::string> LuauSandbox::RunModule(lua_State* state, const std::string& chunkName,
                                                            std::string_view source) {
        // スタック: [cache] → 成功なら [モジュールの戻り値]
        size_t bytecodeSize = 0;
        const Bytecode bytecode = Compile(source, bytecodeSize);
        if (!bytecode) {
            lua_pop(state, 1);

            return std::unexpected("コンパイルの結果を確保できない");
        }

        // --- モジュール用の環境: 書き込みはここに入り、読むときは Run のグローバルへ抜ける ---
        lua_newtable(state);  // [cache, env]
        lua_newtable(state);  // [cache, env, meta]
        lua_pushvalue(state, LUA_GLOBALSINDEX);
        lua_setfield(state, -2, "__index");
        lua_setreadonly(state, -1, true);
        lua_setmetatable(state, -2);

        const std::string luauChunkName = "=" + chunkName;
        if (luau_load(state, luauChunkName.c_str(), bytecode.get(), bytecodeSize, lua_gettop(state)) != LUA_OK) {
            std::string message = ValueToText(state, -1);
            lua_pop(state, 3);

            return std::unexpected(std::move(message));
        }

        lua_remove(state, -2);  // [cache, function]

        // --- 走らせる(失敗しても読み込み中の印を外せるよう pcall で)---
        m_moduleLoading.push_back(chunkName);
        const int status = lua_pcall(state, 0, 1, 0);  // [cache, result | error]
        m_moduleLoading.pop_back();

        if (status != LUA_OK || lua_isnil(state, -1)) {
            std::string message = status != LUA_OK ? ValueToText(state, -1)
                                                   : std::format("{}: モジュールは nil 以外の値を 1 つ返す", chunkName);
            lua_pop(state, 2);

            return std::unexpected(std::move(message));
        }

        lua_pushvalue(state, -1);                    // [cache, result, result]
        lua_setfield(state, -3, chunkName.c_str());  // cache[chunkName] = result
        lua_remove(state, -2);                       // [result]

        return {};
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
