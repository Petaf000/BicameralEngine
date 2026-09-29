// singleton.cpp — SingletonFinalizer の実装(破棄の順番の管理)。使い方と元の実装からの変更点は singleton.h。
#include "core/singleton.h"

#include <algorithm>
#include <ranges>

namespace bicameral {

    namespace {
        // 登録できるシングルトンの数の上限。これを超えるならグローバルな状態を使いすぎている
        constexpr std::size_t MAX_FINALIZERS = 256;
    }  // namespace

    std::mutex SingletonFinalizer::s_mutex;
    std::vector<SingletonFinalizer::Entry> SingletonFinalizer::s_finalizers;

    // Singleton からだけ呼ばれる(friend)。外からは使わない
    void SingletonFinalizer::AddFinalizer(std::type_index type, FinalizerFunc finalize) {
        std::scoped_lock lock(s_mutex);
        assert(s_finalizers.size() < MAX_FINALIZERS);
        s_finalizers.push_back({.type = type, .finalize = finalize});
    }

    void SingletonFinalizer::Finalize() {
        // 一覧を取り出してから、鍵を持たずに破棄する。
        // デストラクタが別のシングルトンを使っても(AddFinalizer が鍵を取っても)止まらないように
        std::vector<Entry> entries;
        {
            std::scoped_lock lock(s_mutex);
            entries.swap(s_finalizers);
        }
        for (const Entry& entry : entries | std::views::reverse) {
            entry.finalize();
        }
    }

    void SingletonFinalizer::FinalizeType(std::type_index type) {
        FinalizerFunc finalize = nullptr;
        {
            std::scoped_lock lock(s_mutex);
            auto found = std::ranges::find(s_finalizers, type, &Entry::type);
            if (found == s_finalizers.end()) return;
            finalize = found->finalize;
            s_finalizers.erase(found);
        }
        finalize();
    }

}  // namespace bicameral
