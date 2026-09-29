// singleton_test.cpp — core/singleton の破棄の順番と作り直しを確かめる(CPU だけで走る)。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "core/singleton.h"

#include <cstdio>
#include <string>

namespace {

    std::string destroyedLog;  // 破棄された順に 1 文字ずつ積む

    struct First {
        ~First() { destroyedLog += 'A'; }
    };
    struct Second {
        ~Second() { destroyedLog += 'B'; }
    };

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition) return;
        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

}  // namespace

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

int main() {
    using bicameral::Singleton;
    using bicameral::SingletonFinalizer;

    // --- 同じインスタンスが返る ---
    First* first = &Singleton<First>::GetInstance();
    EXPECT(first == &Singleton<First>::GetInstance());
    Singleton<Second>::GetInstance();

    // --- 作った順の逆に破棄される ---
    SingletonFinalizer::Finalize();
    EXPECT(destroyedLog == "BA");

    // --- 2 回目の Finalize は何もしない(二重 delete しない)---
    SingletonFinalizer::Finalize();
    EXPECT(destroyedLog == "BA");

    // --- 1 つだけ破棄して、作り直せる ---
    destroyedLog.clear();
    Singleton<First>::GetInstance();
    SingletonFinalizer::FinalizeClass<First>();
    EXPECT(destroyedLog == "A");
    Singleton<First>::GetInstance();
    SingletonFinalizer::Finalize();
    EXPECT(destroyedLog == "AA");

    if (failureCount == 0) std::printf("singleton ok\n");
    return failureCount == 0 ? 0 : 1;
}
