// probe_liquid.h — 仮の世界(ProbeSim の 64³ のセル)に液体の流れを載せる CPU リファレンス(T-0609・ADR-0266。07 §3・05 §3)。
//
// 形(ADR-0266。仮・ユーザー未確認):
//   - **セルが唯一の真実**: 液体の成分は、セル(reaction.hlsli の RxCell)の成分のうち表で液体の物質(phase = "liquid"。
//     BakedReactionTable::speciesLiquid)。液体の別の格子は持たない。足した欄は面の運動量 ×3 だけ(区画のセルの + 側の面)。
//   - **3D のパイプ模型 + 列の静水圧**(07 §3・T-0026 の W1): 面ごとに運動量を持ち(スタッガード)、両側の圧力の差と重さで毎刻み変える。
//     圧力は質量の単位(mg)で書く: P = 2 × 支えている上の液体の質量 + 自分の質量 + 溢れの圧力(体積がセルを超えたぶん × 倍率)。
//     支えている上の液体 = (上のセルが支えている分 + 上のセルの質量)× 自分の満ち具合(満ちていないセルは上の水を支えきれず、上の水は落ちてくる)。
//     さらに P に「下のセルが支える割合」(壁・区画の床は 1、液体は満ち具合、空は 0)を掛ける(落ちている水は横へ押し広げない)。
//     縦の面は P_下 − P_上 − (m_下 + m_上)。満ちたセルの上に静かな水があれば、P の差と重さがちょうど打ち消し合う(整数で厳密に 0)。
//   - **運動量の保存**(D-442): 各セルの圧力は自分の両側の面を同じ大きさで逆向きに押すので、内側の力の和は整数で厳密に 0。
//     外から入る運動量(壁と床の押し返し・重さ・摩擦・行き場のない運動量)は帳簿へ。→ Σ 面の運動量 − 帳簿 は毎刻みビット単位で一定。
//   - **運動量の移流**(T-0610・ADR-0276): 面の運動量を、面のまわりの体積(隣り合う 2 セルの中心から中心)の境を通る質量の流れ
//     (隣の 2 つの面の流れの和 = 2 倍の流れ)で風上に運ぶ(スタッガードの移流。運ぶ量 = 2 倍の流れ × 風上の面の運動量 ÷ 風上の面の 2 倍の質量)。
//     面から出る 2 倍の流れの合計が面の 2 倍の質量を超えれば比で縮める(符号が反転しない)。引いて足すので運動量の和は整数で厳密に保たれる。
//     上流が空でも面のまわりに液体があれば運動量は残す(流れないだけ)。まわりが空になった面だけ「行き場なし」として帳簿へ。
//   - **位置と運動のエネルギー**(T-0610): セルのエネルギー(熱 + 化学)の外に、位置のエネルギー(セルごと)と運動のエネルギー(面ごと)を mJ で持つ。
//     位置は液体と一緒に、運動は運動量と一緒に同じ比で運び、刻みの終わりの状態から計算し直した値との差をセルの熱にする
//     (落ちる水・摩擦・速さの上限で失った分が熱になる。数値の誤差で増えた分は熱から引く)。区画の外の段(反応・置いた水)で液体が現れた・
//     消えた分の位置のエネルギーは帳簿へ。→ Σ セルのエネルギー + 位置 + 運動 − 帳簿 は毎刻みビット単位で一定。
//   - **面の運動量は液体に付く**(T-0656・T-0648・ADR-0286 の液体の版): 区画の外の段(蒸発・凝縮・凍る・溶ける・反応・置く操作)で
//     面の両側の液体の質量が前の刻みの終わりより減っていたら速さを保つ(運動量 × 今 ÷ 前。減った液体は同じ速さで気体・固体へ移ったものとし、
//     その運動量は帳簿へ)。増えていたら運動量を保つ(増えた液体は静止して現れる)。どちらも運動のエネルギーは前の値を超えないので、
//     差は熱になり、質量の増減が熱を負にすることはない。気体の区画(sim/probe_gas)と同じ決まり。
//   - **速さの上限**: 面の速さを maxSpeedMillimetersPerSecond で止める(1 刻みに面のまわりの液体の半分まで動く = 15 m/s が既定)。超えた運動量は帳簿へ。
//   - **質量・成分・エネルギー**: 面の運動量から 1 刻みに動く質量を決め(上流のセルから。出る合計がセルの液体を超えれば比で縮める)、
//     上流の液体の成分の比で物質量を、液体のぶんのエネルギー(熱 × 動く熱容量 ÷ セルの熱容量 + 動く化学)を一緒に動かす。
//     整数で引いて足すので、物質量とエネルギーは構造的にビット単位で保存。前の刻みの値だけから決める(Jacobi 型。順番に依存しない)。
//   - **固体と体積を分け合う**(T-0612・ADR-0268。sim/cell_volume): 固体が占める体積(物質の密度から。書いていなければ 0)を除いた
//     「空き」に液体が入る。満ち具合・溢れ・下の支えは空きで数え、圧力は「質量 ÷ 空き」(孔の中の水も開けた水と同じ高さで同じ圧力)。
//     面は両側の空きの小さい方だけ開き、力と重さもその割合(残りは固体が受けて帳簿の壁へ)。空きが 0 のセルと区画の外の面は壁。
//     孔が液体で満ちるまで上の液体を支えない(上の水は孔に染み込む)。
//   - **段の順**: 適用 → 物理の熱 → 表の差し替え → 気体 → **液体** → 伝導と反応(ProbeReference::Advance の (1c))。
// 今の制限(分けたチケット): 粘りは区画の設定(物質に書くのは T-0628)・相変化は反応表(T-0611)・
//   閉じた所の空気の圧力を液体の圧力に足さない(T-0618)・保存点・状態のハッシュへの組み込みは T-0620。
//   GPU は ProbeSim の液体の段(T-0613。shaders/sim/probe_liquid_tick.hlsl が式を 1 行ずつ写す。ここを変えたらそちらも)。
//   (液体が入ったセルの空気は、気体の空きが減って圧力が上がり、気体の段で押し出される。sim/probe_gas)
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "common/reaction.hlsli"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 液体の格子のセルの 1 辺(mm。仮の世界の 0.5 m)
    constexpr uint64_t PROBE_LIQUID_CELL_EDGE_MILLIMETERS = 500;

    // セルの体積(pL = µmol × mm³/mol。0.125 m³)
    constexpr uint64_t PROBE_LIQUID_CELL_VOLUME_PICOLITERS = PROBE_LIQUID_CELL_EDGE_MILLIMETERS *
                                                             PROBE_LIQUID_CELL_EDGE_MILLIMETERS *
                                                             PROBE_LIQUID_CELL_EDGE_MILLIMETERS * 1000000;

    struct ProbeLiquidOptions {
        // --- 区画(仮の世界の格子の座標。y が上〔world_cells の約束〕)---
        std::array<uint32_t, 3> minCell = {0, 0, 0};
        std::array<uint32_t, 3> sizeCells = {0, 0, 0};  // 0 を含めば液体は流れない

        // --- 模型の値(仮。作り手から見た違いはチケット T-0609 の判断待ち)---
        uint32_t frictionShift =
            7;  // 面の運動量を毎刻み 1/2^frictionShift 減らす(底と粘りの摩擦。帳簿へ。T-0610 で 4 → 7。T-0628)
        uint32_t overfillStiffness = 16;  // 体積がセルを超えたぶんの圧力の倍率(閉じた洞窟で圧力を伝える)

        // --- 運動量の移流と速さ(T-0610。仮)---
        bool advection = true;                          // 運動量を液体と一緒に運ぶ(false は T-0609 の形。比べるため)
        uint32_t maxSpeedMillimetersPerSecond = 15000;  // 面の速さの上限(30000 = 1 刻みに 1 セル が最大)

        [[nodiscard]] bool Enabled() const { return sizeCells[0] != 0 && sizeCells[1] != 0 && sizeCells[2] != 0; }
    };

    // 外から液体に入った運動量の合計(格子の軸 x・y・z。単位は面の運動量と同じ mg。
    // 本当の運動量 kg·m/s = 値 × 1e-6 × 重力加速度 × 刻みの長さ ÷ 2)。内訳は刻みごとの報告に(床の押し返しと重さは
    // 毎刻み打ち消し合いながら大きくなるので、内訳を積むと 64bit を溢れる)
    struct ProbeLiquidLedger {
        std::array<int64_t, 3> external = {};
        int64_t energy = 0;  // mJ。区画の外の段で液体が現れた(+)・消えた(−)分の位置のエネルギー
    };

    // 1 刻みの報告
    struct ProbeLiquidStepReport {
        // --- この刻みに外から入った運動量の内訳(格子の軸。和が帳簿の増え)---
        std::array<int64_t, 3> wall = {};      // 壁・床・区画の外の面の押し返し
        std::array<int64_t, 3> gravity = {};   // 重さ
        std::array<int64_t, 3> friction = {};  // 摩擦(底と粘り)
        std::array<int64_t, 3> dropped = {};   // まわりが空になった面の運動量(移流なしでは上流が空の面も)
        std::array<int64_t, 3> capped = {};    // 速さの上限で止めた運動量
        std::array<int64_t, 3> vanished = {};  // 区画の外の段で液体が減った面の、減った液体が持って出た運動量(T-0656)

        // --- エネルギー(mJ)---
        int64_t heat = 0;              // 位置と運動のエネルギーから熱になった分(負は熱から戻った分)
        int64_t potentialEntered = 0;  // 区画の外の段で液体が現れた・消えた分の位置のエネルギー(帳簿の増え)
        uint32_t cappedFaces = 0;      // 速さの上限で止めた面

        uint32_t wetCells = 0;         // 液体のあるセル
        uint32_t changedCells = 0;     // 値が変わったセル
        uint32_t limitedCells = 0;     // 出る量を中身で縮めたセル
        uint32_t overfilledCells = 0;  // 液体の体積がセルを超えたセル
        uint64_t movedMilligrams = 0;  // 面を通って動いた質量の合計
    };

    class ProbeLiquidField {
    public:
        // 区画を作る。断る: 区画が格子からはみ出す・重力が 0(環境の値 environment.gravity)
        [[nodiscard]] static std::expected<ProbeLiquidField, std::string> Create(const BakedReactionTable& table,
                                                                                 const ProbeLiquidOptions& options);

        // 液体の 1 刻み。cells は格子の全部のセル(その場で書き換える)。値が変わったセルの番号を changed に足す
        ProbeLiquidStepReport Step(const BakedReactionTable& table, std::span<reaction::RxCell> cells,
                                   std::vector<uint32_t>& changed);

        [[nodiscard]] const ProbeLiquidOptions& Options() const { return m_options; }
        [[nodiscard]] const ProbeLiquidLedger& Ledger() const { return m_ledger; }

        // 面の運動量の和(格子の軸)。Σ − 帳簿 が一定(保存の検査)
        [[nodiscard]] int64_t MomentumSum(uint32_t axis) const;

        // 区画の中の番号(x + 幅 × (z + 奥行き × y))→ 格子の全部のセルの中の番号
        [[nodiscard]] uint32_t GridIndex(uint32_t region) const;
        [[nodiscard]] uint32_t RegionCellCount() const { return (uint32_t)m_faces[0].size(); }

        // 位置のエネルギー(セルごと)と運動のエネルギー(面ごと)の和(mJ。セルのエネルギーの外)。
        // Σ セルのエネルギー + これ − 帳簿のエネルギー が一定(保存の検査)
        [[nodiscard]] int64_t PotentialEnergySum() const;
        [[nodiscard]] int64_t KineticEnergySum() const;

        // 面の運動量・エネルギー・帳簿のハッシュ(決定性の検査。セルは呼ぶ側が別に)
        [[nodiscard]] uint64_t Hash() const;

        // セルの液体の質量(ng = µmol × mg/mol)と体積(pL = µmol × mm³/mol)
        [[nodiscard]] static uint64_t LiquidMass(const BakedReactionTable& table, const reaction::RxCell& cell);
        [[nodiscard]] static uint64_t LiquidVolume(const BakedReactionTable& table, const reaction::RxCell& cell);

    private:
        ProbeLiquidOptions m_options;
        uint64_t m_gravityMicrometersPerSecond2 = 0;
        std::array<std::vector<int64_t>, 3> m_faces;    // 区画のセルの + 側の面の運動量(格子の軸 x・y・z)
        std::array<std::vector<int64_t>, 3> m_kinetic;  // 面の運動のエネルギー(mJ。セルのエネルギーの外)
        std::vector<int64_t> m_potential;  // セルの液体の位置のエネルギー(mJ。区画の床から。セルのエネルギーの外)
        std::vector<uint64_t> m_mass;      // セルの液体の質量(mg。前の刻みの終わり。段の外での増減を見る。T-0656)
        ProbeLiquidLedger m_ledger;

        [[nodiscard]] uint32_t RegionIndex(uint32_t x, uint32_t y, uint32_t z) const;
        [[nodiscard]] std::array<uint32_t, 3> RegionCoordinates(uint32_t region) const;
        [[nodiscard]] bool AtRest() const;
        [[nodiscard]] int64_t PotentialOf(uint32_t region, uint64_t massMilligrams) const;
        [[nodiscard]] int64_t KineticOf(int64_t momentum, uint64_t doubledMass) const;
        [[nodiscard]] uint64_t MaxMomentum(uint64_t doubledMass) const;

        // Step の段(probe_liquid.cpp)
        struct StepWork;
        [[nodiscard]] uint32_t FaceOpenQ16At(const StepWork& work, uint32_t region, uint32_t axis) const;
        void FollowMassChange(const StepWork& work, ProbeLiquidStepReport& report);
        void ComputePressure(StepWork& work, ProbeLiquidStepReport& report) const;
        void ApplyForces(StepWork& work, ProbeLiquidStepReport& report);
        void MoveMass(const BakedReactionTable& table, std::span<reaction::RxCell> cells, StepWork& work,
                      ProbeLiquidStepReport& report);
        void AdvectMomentum(StepWork& work, ProbeLiquidStepReport& report);
        void SettleEnergy(const BakedReactionTable& table, std::span<reaction::RxCell> cells, StepWork& work,
                          ProbeLiquidStepReport& report);
    };

}  // namespace bicameral::sim
