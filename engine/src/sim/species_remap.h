// species_remap.h — 物質を足す・消す反応表の差し替えで、世界のセルの物質 ID を名前で付け替える規則(T-0223・ADR-0065・D-438)。
//
// データの流れ: 今の表と新しい表(BakedReactionTable)→ BuildSpeciesRemap(表 2 つだけから決まる。世界を見ない)→ SpeciesRemap
//   → RemapReactionCell でセルを 1 つずつ付け替える(本体は common/species_remap.hlsli。CPU リファレンス: ProbeReference::Advance の
//   表の差し替え。GPU: ProbeSim が PackSpeciesRemap の語の列を上げ、probe_tick.hlsl の RemapSpecies)。
// 物質 ID は名前のバイト順(ADR-0032)なので、物質を 1 つ足す・消すだけで後ろの物質の ID がずれる。セルは物質を ID で持つので付け替える。
// 規則(ADR-0065):
//   - 名前と元素の組み立て(元素の名前ごとの原子の数)が同じ物質は同じ物質: ID だけ付け替える(セルの中の並びはそのまま)。
//   - 消えた物質(と、組み立てが変わった物質)は元素に分けて戻す: 元素ごとに新しい表の「単体」(その元素だけでできた物質のうち、
//     原子 1 つあたりの化学のエネルギー h0 が一番低い = 一番安定な形。同じなら原子の数が少ない方・ID が小さい方)へ、原子の数を保って移す。
//     単体の原子の数で割り切れない端数(O2 に奇数個の O)と、セルの成分の上限(RX_MAX_CELL_SPECIES)に入らない分は失う(報告する)。
//   - 熱は保つ: 分けた分の化学のエネルギーの差をセルのエネルギーに足す(mJ に切り上げるので熱は減らない)。差は改造による出入りとして報告する。
//     残った物質の値(生成エンタルピーなど)が変わった分は、これまでの差し替え(ADR-0047)と同じくエネルギーを保つ。
//   - 単体の無い元素を含む物質を消す表は当てない(BuildSpeciesRemap が理由を返す)。世界の中身を見ずに表だけで決める
//     (GPU の世界を読み戻さずに済み、記録と再生で同じ判断になる)。
// 決定性: 表 2 つだけから決まるので、再生は記録した表の中身(ADR-0050)から同じ付け替えを作り直す(新しいコマンドは要らない)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "common/species_remap.hlsli"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 付け替えの表の読み方(common/species_remap.hlsli の Remap の約束。GPU の版はバッファを読む構造体)
    struct SpeciesRemapView {
        std::span<const uint32_t> newIds;
        std::span<const uint32_t> partBegin;
        std::span<const reaction::RxRemapPart> parts;
        std::span<const int64_t> removedH0;
        std::span<const reaction::RxRemapUnit> units;

        [[nodiscard]] uint32_t NewId(uint32_t oldId) const { return newIds[oldId]; }
        [[nodiscard]] uint32_t PartBegin(uint32_t oldId) const { return partBegin[oldId]; }
        [[nodiscard]] reaction::RxRemapPart Part(uint32_t index) const { return parts[index]; }
        [[nodiscard]] int64_t RemovedH0(uint32_t oldId) const { return removedH0[oldId]; }
        [[nodiscard]] uint32_t UnitCount() const { return static_cast<uint32_t>(units.size()); }
        [[nodiscard]] reaction::RxRemapUnit Unit(uint32_t index) const { return units[index]; }
    };

    struct SpeciesRemap {
        // --- 今の表の物質 ID ごと(添字 = 今の ID。0 は使わない)---
        std::vector<uint32_t> newIds;     // 新しい表の ID(0 = 消えた → parts で元素に分ける)
        std::vector<uint32_t> partBegin;  // parts の始まり(大きさは今の物質の数 + 1。消えていない物質は 0 個)
        std::vector<int64_t> removedH0;   // 今の表の h0(消えた物質の化学のエネルギーを数える)

        // --- 分けた先 ---
        std::vector<reaction::RxRemapPart> parts;
        std::vector<reaction::RxRemapUnit> units;  // 使う単体(新しい表の ID の昇順)

        bool identity = false;  // 物質の一覧(名前・ID・組み立て)が同じ。セルを触らなくてよい

        [[nodiscard]] SpeciesRemapView View() const {
            return {.newIds = newIds, .partBegin = partBegin, .parts = parts, .removedH0 = removedH0, .units = units};
        }
    };

    // 付け替えで起きたこと(セル 1 つか、足し合わせた世界の分)
    struct SpeciesRemapReport {
        uint64_t decomposedMicromoles = 0;     // 元素に分けた物質の量(µmol)
        uint64_t remainderAtomMicromoles = 0;  // 単体の原子の数で割り切れずに失った原子(µmol)
        uint64_t overflowAtomMicromoles = 0;   // セルの成分の上限に入らずに失った原子(µmol)
        int64_t energyDeltaMilliJoules = 0;    // 熱を保つためにセルに足したエネルギー(mJ。改造による出入り)

        void Add(const SpeciesRemapReport& other);
    };

    // 今の表 current から新しい表 next への付け替え。単体の無い元素を含む物質を消す表なら、理由(物質と元素の名前)を返す
    [[nodiscard]] std::expected<SpeciesRemap, std::string> BuildSpeciesRemap(const BakedReactionTable& current,
                                                                             const BakedReactionTable& next);

    // セル 1 つを付け替える(今の表の ID のセル → next の ID のセル)。remap は BuildSpeciesRemap(今の表, next) の結果。
    // 本体は common/species_remap.hlsli の RxRemapCell(GPU と同じ関数)
    [[nodiscard]] SpeciesRemapReport RemapReactionCell(const SpeciesRemap& remap, const BakedReactionTable& next,
                                                       reaction::RxCell& cell);

    // GPU に上げる形(32bit の語の列。common/species_remap.hlsli の「GPU のバッファの形」)
    [[nodiscard]] std::vector<uint32_t> PackSpeciesRemap(const SpeciesRemap& remap);

    // 元素の名前ごとの原子の数(µmol。名前のバイト順)。表が替わって元素の ID がずれても比べられる(テストと検査)
    struct NamedElementCount {
        std::string element;
        uint64_t atomMicromoles = 0;

        friend bool operator==(const NamedElementCount&, const NamedElementCount&) = default;
    };

    [[nodiscard]] std::vector<NamedElementCount> CountNamedElements(const BakedReactionTable& table,
                                                                    const reaction::RxCell& cell);

}  // namespace bicameral::sim
