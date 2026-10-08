// luau_sandbox_test.cpp — script/luau_sandbox(Luau の殻。T-0020、13 §2)を CPU だけで確かめる。
// 見るもの: 戻り値とホスト関数 / 見せないもの(ファイル・OS・require など)に触れない / Run どうしが混ざらない /
//           安全点とメモリの上限で止まり、その後も使える / 文法の誤り / 決定性(別の殻・何度走らせても同じ結果)。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "script/luau_sandbox.h"

#include <cstdio>
#include <string>
#include <vector>

#include "lua.h"
#include "lualib.h"

namespace {

    using namespace bicameral::script;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 道具 ---

    // host.emit(x): x を文字列にして context の配列へ足す(ホスト関数の例)
    int HostEmit(lua_State* state) {
        auto* emitted = static_cast<std::vector<std::string>*>(LuauSandbox::HostContext(state));
        size_t length = 0;
        const char* text = luaL_checklstring(state, 1, &length);
        emitted->emplace_back(text, length);

        return 0;
    }

    std::unique_ptr<LuauSandbox> MakeSealed(SandboxLimits limits, std::vector<std::string>* emitted) {
        auto created = LuauSandbox::Create(std::move(limits));
        if (!created) {
            std::printf("Create に失敗: %s\n", created.error().c_str());
            ++failureCount;

            return nullptr;
        }

        auto sandbox = std::move(*created);
        if (emitted)
            EXPECT(sandbox->RegisterFunction("host", "emit", &HostEmit, emitted).has_value());
        sandbox->Seal();

        return sandbox;
    }

    // 1 つの値を返すソースを走らせて、その文字列を返す(失敗なら "ERROR: ...")
    std::string RunOne(LuauSandbox& sandbox, std::string_view source) {
        const auto result = sandbox.Run("test", source);
        if (!result)
            return "ERROR: " + result.error().message;
        if (result->returns.empty())
            return "";

        return result->returns[0];
    }

    // --- 戻り値とホスト関数 ---

    void TestReturnsAndHost() {
        std::vector<std::string> emitted;
        auto sandbox = MakeSealed({}, &emitted);
        if (!sandbox)
            return;

        const auto result = sandbox->Run(
            "returns", "host.emit('a'); host.emit(tostring(1 + 2)); return 1 + 2, 'x', true, nil, {}");
        EXPECT(result.has_value());
        if (!result)
            return;

        EXPECT((result->returns == std::vector<std::string>{"3", "x", "true", "nil", "<table>"}));
        EXPECT((emitted == std::vector<std::string>{"a", "3"}));
        EXPECT(result->safepointCount > 0);
    }

    void TestPrintSink() {
        std::vector<std::string> printed;
        SandboxLimits limits;
        limits.printSink = [&](std::string_view chunk, std::string_view text) {
            printed.push_back(std::string(chunk) + ":" + std::string(text));
        };
        auto sandbox = MakeSealed(std::move(limits), nullptr);
        if (!sandbox)
            return;

        EXPECT(sandbox->Run("hello", "print('a', 1, nil)").has_value());
        EXPECT((printed == std::vector<std::string>{"hello:a\t1\tnil"}));
    }

    // --- サンドボックス ---

    void TestHiddenGlobals() {
        auto sandbox = MakeSealed({}, nullptr);
        if (!sandbox)
            return;

        // ファイル・OS・時計・require・debug・環境の書き換え・コードの読み込みは、どれも無い
        const char* hidden[] = {"os",         "io",      "debug",   "require", "dofile",         "loadfile", "load",
                                "loadstring", "getfenv", "setfenv", "gcinfo",  "collectgarbage", "newproxy", "package"};
        for (const char* name : hidden) {
            const std::string source = std::string("return ") + name + " == nil";
            const bool absent = RunOne(*sandbox, source) == "true";
            if (!absent)
                std::printf("  見えてしまう: %s\n", name);
            EXPECT(absent);
        }

        // 使えるものは使える
        EXPECT(RunOne(*sandbox, "return string.format('%d-%s', 7, 'x') .. #table.concat({'a','b'})") == "7-x2");
        EXPECT(RunOne(*sandbox, "return bit32.band(0xF0, 0x3C)") == "48");

        // 標準ライブラリとグローバルは書き換えられない
        EXPECT(RunOne(*sandbox, "math.pi = 3; return math.pi").starts_with("ERROR:"));
        EXPECT(RunOne(*sandbox, "string.upper = nil; return 1").starts_with("ERROR:"));
        EXPECT(RunOne(*sandbox, "os.exit()").starts_with("ERROR:"));
    }

    void TestRunsAreIsolated() {
        auto sandbox = MakeSealed({}, nullptr);
        if (!sandbox)
            return;

        EXPECT(RunOne(*sandbox, "leaked = 5; print = nil; return leaked") == "5");
        EXPECT(RunOne(*sandbox, "return leaked") == "nil");
        EXPECT(RunOne(*sandbox, "return type(print)") == "function");
    }

    void TestUsageErrors() {
        auto created = LuauSandbox::Create({});
        EXPECT(created.has_value());
        if (!created)
            return;

        auto& sandbox = **created;
        const auto early = sandbox.Run("early", "return 1");
        EXPECT(!early && early.error().kind == ScriptErrorKind::Usage);

        sandbox.Seal();
        const auto late = sandbox.RegisterFunction("host", "emit", &HostEmit, nullptr);
        EXPECT(!late && late.error().kind == ScriptErrorKind::Usage);
    }

    // --- 上限 ---

    void TestSafepointLimit() {
        SandboxLimits limits;
        limits.safepointLimit = 100'000;
        auto sandbox = MakeSealed(limits, nullptr);
        if (!sandbox)
            return;

        const auto endless = sandbox->Run("endless", "while true do end");
        EXPECT(!endless && endless.error().kind == ScriptErrorKind::SafepointLimit);

        // pcall で捕まえても止まる
        const auto caught = sandbox->Run("caught", "while true do pcall(function() while true do end end) end");
        EXPECT(!caught && caught.error().kind == ScriptErrorKind::SafepointLimit);

        // 止まった後も普通に使える
        EXPECT(RunOne(*sandbox, "local s = 0 for i = 1, 100 do s += i end return s") == "5050");
    }

    void TestMemoryLimit() {
        SandboxLimits limits;
        limits.memoryLimitBytes = 4ull << 20;
        auto sandbox = MakeSealed(limits, nullptr);
        if (!sandbox)
            return;

        const auto hungry = sandbox->Run("hungry", "local t = {} for i = 1, 10000000 do t[i] = i end return #t");
        EXPECT(!hungry && hungry.error().kind == ScriptErrorKind::Memory);
        EXPECT(sandbox->MemoryBytes() <= limits.memoryLimitBytes);

        EXPECT(RunOne(*sandbox, "return 1 + 1") == "2");
    }

    void TestCompileError() {
        auto sandbox = MakeSealed({}, nullptr);
        if (!sandbox)
            return;

        const auto broken = sandbox->Run("broken.luau", "return +");
        EXPECT(!broken && broken.error().kind == ScriptErrorKind::Compile);
        if (!broken)
            EXPECT(broken.error().message.contains("broken.luau"));

        const auto thrown = sandbox->Run("thrown", "error('boom')");
        EXPECT(!thrown && thrown.error().kind == ScriptErrorKind::Runtime && thrown.error().message.contains("boom"));
    }

    // --- 決定性 ---

    // ベイクの形に近いもの: 乱数・文字列・並べ替え・小数の表示を混ぜ、キーを並べてから 1 本の文字列にする
    constexpr const char* DETERMINISM_SOURCE = R"(
        local rows = {}
        for i = 1, 200 do
            local name = string.format("elem%03d", math.random(1, 999))
            rows[name] = (rows[name] or 0) + math.random() * 1e3 / 7
        end

        local keys = {}
        for k in pairs(rows) do table.insert(keys, k) end
        table.sort(keys)

        local parts = {}
        for _, k in ipairs(keys) do
            table.insert(parts, k .. "=" .. tostring(rows[k]))
            host.emit(k)
        end
        return table.concat(parts, ";"), #keys
    )";

    void TestDeterminism() {
        std::vector<std::string> emittedA;
        std::vector<std::string> emittedB;
        SandboxLimits limits;
        limits.randomSeed = 1234;
        auto sandboxA = MakeSealed(limits, &emittedA);
        auto sandboxB = MakeSealed(limits, &emittedB);
        if (!sandboxA || !sandboxB)
            return;

        // 別の殻で 1 回ずつ・同じ殻で 2 回(2 回目も種が戻る)
        const auto first = sandboxA->Run("bake", DETERMINISM_SOURCE);
        const auto second = sandboxB->Run("bake", DETERMINISM_SOURCE);
        const auto again = sandboxA->Run("bake", DETERMINISM_SOURCE);
        EXPECT(first && second && again);
        if (!first || !second || !again)
            return;

        EXPECT(first->returns == second->returns);
        EXPECT(first->returns == again->returns);
        EXPECT(first->safepointCount == second->safepointCount);
        EXPECT(first->safepointCount == again->safepointCount);
        EXPECT(emittedB.size() * 2 == emittedA.size());
        EXPECT(first->returns.size() == 2 && first->returns[0].size() > 100);

        // 種を変えれば変わる(乱数が本当に種から来ている)
        SandboxLimits other = limits;
        other.randomSeed = 99;
        std::vector<std::string> emittedC;
        auto sandboxC = MakeSealed(other, &emittedC);
        if (!sandboxC)
            return;

        const auto changed = sandboxC->Run("bake", DETERMINISM_SOURCE);
        EXPECT(changed && changed->returns != first->returns);
    }

}  // namespace

int main() {
    TestReturnsAndHost();
    TestPrintSink();
    TestHiddenGlobals();
    TestRunsAreIsolated();
    TestUsageErrors();
    TestSafepointLimit();
    TestMemoryLimit();
    TestCompileError();
    TestDeterminism();

    if (failureCount != 0) {
        std::printf("luau_sandbox_test: %d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("luau_sandbox_test: OK\n");
    return 0;
}
