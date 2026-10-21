// multires_state.h — 多重解像度の世界の持ち越す状態を「区画の一覧」として回る(運ぶ計算の共通の土台の段 1 ①。T-0711。ADR-0329。設計 §2.1)。
// 状態のハッシュ(HashWholeNest)・保存点(SaveNestState / RestoreNestState)・写し(CopyNestState。写しの早送りの元)は
// この一覧(VisitNestState)を回るだけ。欄を登録すると(sim/multires_fields.h の RegisterNestField)一覧の後ろに入り、3 つとも直さずに付いて行く。
// 一覧の前の部分(見出し〜溢れ)の並びと要約の仕方は T-0711 より前の HashWholeNest と同じ(欄の無い世界の要約はビット単位で同じ)。
// GPU(段 6)は同じ一覧からバッファの一覧を作る(今の仮の世界の保存点 probe_save_point.cpp::StateRegions が手で並べている所)。
#pragma once

#include <any>
#include <string>
#include <vector>

#include "common/state_field.hlsli"
#include "sim/multires_nest.h"

namespace bicameral::sim {

    // 状態の区画 1 つ
    struct NestStateRegion {
        std::string name;
        // 状態のハッシュに入るか(入らない区画も保存点・写しには入る: 種〔GPU は順の決まらない一覧で持つ〕・索引〔作り直せるが、
        // 墓石の位置が次の挿し入れ = 数える欄を変えるので写す〕)
        bool hashed = true;
    };

    // nest(const でも可)の区画を順に visit(区画, 値の並び) で回る。値の並びは std::vector か std::array、セルは
    // MultiresCellStore(頁は成分の数に合わせた幅。要約は GPU と同じ並びの列で。T-0931)、登録した欄は MultiresFieldStore(疎なら頁の表ごと。T-0921)。
    // 状態に入らないもの: 1 刻みの要求・取り合いの印(刻みの境では空)・陰解法の費用と写し(計測用)
    template <typename Nest, typename Visit>
    void VisitNestState(Nest& nest, Visit&& visit) {
        visit(NestStateRegion{.name = "blocks"}, nest.blocks);
        visit(NestStateRegion{.name = "cells"}, nest.cells);
        visit(NestStateRegion{.name = "fractions"}, nest.fractions);
        visit(NestStateRegion{.name = "free_blocks"}, nest.freeBlocks);
        visit(NestStateRegion{.name = "free_fractions"}, nest.freeFractions);
        visit(NestStateRegion{.name = "ledger"}, nest.ledger);
        visit(NestStateRegion{.name = "counters"}, nest.counters);

        // --- 溢れ(T-0187。使う世界だけ)---
        if (nest.wideCells) {
            visit(NestStateRegion{.name = "cell_overflow"}, nest.cellOverflow);
            visit(NestStateRegion{.name = "fraction_overflow"}, nest.fractionOverflow);
            visit(NestStateRegion{.name = "uniform_overflow"}, nest.uniformOverflow);
        }

        // --- ハッシュに入らない区画 ---
        visit(NestStateRegion{.name = "seeds", .hashed = false}, nest.seeds);
        visit(NestStateRegion{.name = "index", .hashed = false}, nest.index);

        // --- 登録した欄(登録の順。粗くした余りは上の世界の帳簿の欄の列に入っている。T-0762)。
        //     導かれる値は状態に入らない(真実から解き直す。温めた始まりは D-549 の仮 (A) で入る。T-0854)。
        //     値は置き場ごと(疎なら頁の表も)写し、要約は密の並び〔一様の値・セルの頁ごとの値。無い頁は 0〕で取る(T-0921)---
        for (auto& field : nest.fields) {
            if (!state_field::SfSaved(field.desc))
                continue;

            visit(NestStateRegion{.name = "field:" + field.name, .hashed = state_field::SfHashed(field.desc)},
                  field.store);
        }
    }

    // 保存点(CPU。区画ごとの値の写し)。GPU の保存点は段 6
    struct MultiresSaveState {
        std::vector<std::string> names;
        std::vector<std::any> regions;
        std::vector<state_field::SfFieldDesc> fieldDescs;  // 登録した欄の形(戻す先と比べる)
    };

    // 刻みの境の状態を写す
    [[nodiscard]] MultiresSaveState SaveNestState(const MultiresNest& nest);

    // 保存点に戻す。形(区画の名前と大きさ・溢れの有無・登録した欄)が違えば何もせず false。
    // 保存点に入らない導かれる値は 0 にする(起きている所は次に計算する時に真実から解き直す。T-0854)
    [[nodiscard]] bool RestoreNestState(MultiresNest& nest, const MultiresSaveState& save);

    // from の状態を、同じ形で作った to へ写す(写しの早送りの元)。形が違えば何もせず false
    [[nodiscard]] bool CopyNestState(const MultiresNest& from, MultiresNest& to);

}  // namespace bicameral::sim
