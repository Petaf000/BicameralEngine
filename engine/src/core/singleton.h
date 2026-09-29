// singleton.h — mozc 式のシングルトン(ユーザー提供の singleton_template を規約 docs/style.md に合わせたもの)。
//
// 生成は最初の GetInstance()、破棄は SingletonFinalizer::Finalize() でまとめて行う(main の最後で 1 回呼ぶ)。
// 元の実装から変えた点:
//   - 破棄は「作った順の逆」。後から作られた物は先に作られた物に依存しうる(ログを使う物より先にログを壊さない)
//   - Finalize() を 2 回呼んでも二重に delete しない
//   - FinalizeClass<T>() の後に GetInstance() を呼ぶと作り直す(元は call_once 済みで nullptr を返していた)
//
// 注意(docs/style.md「分け方・つなぎ方」): グローバルな状態は避ける。使うのはログのように本当に 1 つしか要らず、
// どこからでも呼ぶものだけ。
#pragma once

#include <atomic>
#include <cassert>
#include <mutex>
#include <typeindex>
#include <vector>

namespace bicameral {

    class SingletonFinalizer {
    public:
        using FinalizerFunc = void (*)();

        // 作られたすべてのインスタンスを、作った順の逆に破棄する
        static void Finalize();

        // T のインスタンスだけを破棄する。作られていなければ何もしない
        template <typename T>
        static void FinalizeClass() {
            FinalizeType(typeid(T));
        }

    private:
        template <typename T>
        friend class Singleton;

        struct Entry {
            std::type_index type;
            FinalizerFunc finalize;
        };

        static void AddFinalizer(std::type_index type, FinalizerFunc finalize);
        static void FinalizeType(std::type_index type);

        static std::mutex s_mutex;
        static std::vector<Entry> s_finalizers;  // 作った順
    };

    template <typename T>
    class Singleton final {
    public:
        static T& GetInstance() {
            // 作成済みなら鍵を取らずに返す(ログのように頻繁に呼ばれるため)
            if (T* instance = s_instance.load(std::memory_order_acquire)) return *instance;

            std::scoped_lock lock(s_mutex);
            if (s_instance.load(std::memory_order_relaxed) == nullptr) Create();
            return *s_instance.load(std::memory_order_relaxed);
        }

    private:
        static void Create() {
            s_instance.store(new T, std::memory_order_release);
            SingletonFinalizer::AddFinalizer(typeid(T), &Singleton<T>::Destroy);
        }

        static void Destroy() {
            std::scoped_lock lock(s_mutex);
            delete s_instance.exchange(nullptr, std::memory_order_acq_rel);
        }

        static inline std::mutex s_mutex;
        static inline std::atomic<T*> s_instance = nullptr;
    };

}  // namespace bicameral
