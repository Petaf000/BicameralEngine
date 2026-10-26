// support_fracture.h — 支えの法則の割れ(研究 R-B2・T-0872。docs/design/22-support-law.md §3.3・§3.4・§6)の試作。CPU だけ・浮動小数点なし(04 §4)。
// 支えの釣り合い(sim/support_law。C-0 の使い手)を 1 刻み解いた後、面ごとに割れの進み(真実。0〜1 = 割れた面積の割合)を進め、
// 進んだ分だけ面の係数を下げる(割れた面は係数 0。C-0 の部品の型は変えない = 法則の側で面を外す)。割れの判定は 2 通り(FractureRule):
//   - Strength(1 本目): 面の縁の応力が強さの形(引きの切り口 + 押し合い × 摩擦 + くっつく強さ〔c・φ は押しと引きの強さ 2 つから〕+ 押しの強さ)を
//     超えたら、その面は割れ切るまで進む
//   - Energy(2 本目): 強さを超えた面だけが割れ始め(どこで・どの向きに)、進みは面のたわみのエネルギーで決める(線形の軟化。
//     割れ切るまでに面が払うエネルギーが割れ面のエネルギー G × 面積 になるように軟化の傾きを細かさで決める = crack band)。
//     進みの分だけ解放されるエネルギー(変位を止めて面の硬さが落ちた分)は、払う割れ面のエネルギー以上(坂を下る時だけ進む)
//   - Window(3 本目・T-1042・T-1069): 割れの面(同じ境の面の並び)を、面の続きの両端(物の表面)から判定の長さ ℓ ずつの区間に分け、区間ごとに
//     引きの側・押しの側の応力の平均(面の中の直線の分布の正の部分・負の部分)・ずれ・エネルギーを平均して 2 本目と同じ判定をし、
//     区間の面を一緒に進める(有限の割れの判定〔Leguillon の coupled criterion〕と同じ形。セル ≤ ℓ なら判定がセルの大きさによらない)。
//     ℓ = 硬さ × 割れ面のエネルギー ÷ 引きの強さ²(Irwin の長さ。下限 SUPPORT_FRACTURE_MIN_LENGTH_MM。D-644)で、物質の別の値は持たない。
//     セル > ℓ の所は区間 = 面 1 つ(粗い所は ℓ の細かさに下りて割る = 22 §3.5。下りる道は下の refine)
// 割れの速さは物質の値(m/s。D-612 の既定 = この宇宙の速さとして遅く)。1 刻みに進む割合 = 速さ × 刻み ÷ 割れが渡る幅(1 を超える値は 1 で止める。
// 1 本目・2 本目は面の幅 = セル。3 本目は持ち主の区間の面の幅の和〔区間の面は一緒に進むので、区間を渡り切るまでが 幅 ÷ 速さ〕= 細かさによらない。T-1135。
// 速さから決まる小刻み〔D-612 (a)〕の道はまだ無い: T-0943)。
// 割れて地面・壁(固定した面)につながらなくなったセルは「分かれた」印を付けて釣り合いから外す(物の物理へ渡すのは R-O1。ここでは印まで)。
// 細かくする道(T-1067・22 §3.5・ADR-0470「割れの判定の数え方」): 場面に分けたセル(SupportScene::fine)があるか refine を入れると、割れの進みを
// 細かい格子(一辺 cellMm ÷ 2)の面ごとに持つ(粗い面は細かい面 2 つ分の幅で、2 つの値をそろえて持つ)。区間は面の数ではなく長さで切り、
// 応力は面積 w × D の重み、割れ面のエネルギーは G × w × D、軟化の傾きは面の梁の長さ h(粗い面 2s・細かい面 s・違う大きさの面 1.5s)、
// 1 刻みに進む割合は 速さ × 刻み ÷ 持ち主の区間の Σw(support_fracture_fine.cpp。T-1135)。refine は強さの比が refineIndex を超えた面の両側の粗いセルを 2 × 2 に分け、
// 割れが止んだ所(中の面が割れていない・外の面の 2 つの細かい面の進みが同じ・周り coarsenRing の中に比が coarsenIndex 以上のセルが無い)を
// 戻す(状態だけで決まる = N1)
// データの流れ: SupportScene + SupportStrength → SupportLaw(釣り合い)→ 面の力・エネルギー(FaceForces)→ 割れの進み → scene の intactX / intactUp
//   → 次の刻みの SupportLaw(前の答えから温めて始める)。帳簿: 解放されたエネルギー = 割れ面のエネルギー + 熱(pJ。整数で厳密)
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "sim/support_law.h"

namespace bicameral::sim {

    // 割れの判定の長さ ℓ の法則の下限(D-644。ℓ = 硬さ × 割れ面のエネルギー ÷ 引きの強さ² がこれより短い物は、これで判定する。
    // 判定を細かさによらなくするには割れかけの所をこの半分の細かさにする = 多重解像度の側の下限)
    inline constexpr uint64_t SUPPORT_FRACTURE_MIN_LENGTH_MM = 500;

    enum class FractureRule : uint8_t {
        Strength,  // 1 本目: 強さを超えた面が割れる
        Energy,    // 2 本目: 強さで割れ始め、進みは割れ面のエネルギーで(線形の軟化)
        Window,  // 3 本目(T-1042): 2 本目の判定を、割れの面に沿った材料の長さ ℓ の区間で平均する(区間の面は一緒に進む)
    };

    // 物質の強さ(22 §1.1。名前・単位は仮)
    struct SupportStrength {
        uint64_t compressiveKpa = 0;  // 押しの強さ σc
        uint64_t tensileKpa = 0;      // 引きの強さ σt
        uint64_t
            fractureEnergyMjPerM2 = 0;     // 割れ面のエネルギー G(mJ/m²)。0 = 導く: σt² × 破片の大きさ ÷ 硬さ(22 §3.4)
        uint64_t fragmentMm = 500;         // 破片の大きさ(G を導く時だけ)
        uint64_t crackSpeedMmPerS = 5000;  // 割れの速さ(仮 5 m/s。D-612 の既定 = この宇宙の速さとして遅く)
    };

    struct SupportFractureOptions {
        FractureRule rule = FractureRule::Energy;
        uint64_t stepUs = 16667;  // 1 刻み(µs。60 刻み/秒)

        // --- 割れかけの所を細かくする(T-1067)---
        bool refine = false;
        uint32_t refineIndex = 32768;  // 分ける強さの比(Q16。1/2)
        uint32_t refineRing = 1;  // 分ける周り(比が線を超えた面の両側のセルから、この数のセルまで。チェビシェフの距離)
        uint32_t coarsenIndex = 16384;  // 戻す強さの比(Q16。1/4)
        // 戻すのは、比が coarsenIndex 未満の刻みがこの数だけ続いた後(分けると比の測り方が変わるので、比の幅だけでは
        // 線の近くのセルが毎刻み分ける・戻すを行き来した = 2026-10-26 の測り)。続いた数はセルごとの状態(ハッシュに入る)
        uint32_t coarsenTicks = 120;
        // 戻してよいのは、このチェビシェフの距離の中に比が coarsenIndex 以上のセルが無いセルだけ(T-1134。分ける所の縁のセルが、
        // 自分の比だけを見て戻ると、隣の割れかけのセルの比の揺れで分ける・戻すを 2 秒ごとに行き来した)。0 = 自分のセルだけを見る(T-1067 の形)
        uint32_t coarsenRing = 2;
    };

    // 割れの帳簿(pJ)。releasedPj == surfacePj + heatPj が整数で厳密(熱は残りとして数える)
    struct SupportFractureLedger {
        int64_t releasedPj = 0;         // 面の硬さが落ちた分のたわみのエネルギー(変位を止めて。割れが受け取った分)
        int64_t surfacePj = 0;          // 割れ面のエネルギー(払った分)
        int64_t heatPj = 0;             // released − surface(負 = 坂を上った = エネルギーを借りた)
        int64_t worstStepHeatPj = 0;    // 面 1 つ・1 刻みの熱の最小(Energy は 0 以上のはず)
        uint32_t brokenFaces = 0;       // 割れ切った面
        uint32_t crackingFaces = 0;     // 割れかけの面(0 < 進み < 1)
        uint32_t separatedCells = 0;    // 分かれた印のセル
        uint32_t heatLimitedSteps = 0;  // 区間の熱が負になるので届く所を下げた歩の数(Window だけ。T-1103)

        // --- 細かい格子で持つ時(T-1067)---
        uint64_t brokenAreaMm2 = 0;    // 割れ切った面の面積の和(面の幅 × 奥行)
        uint64_t crackingAreaMm2 = 0;  // 割れかけの面の面積の和
        uint32_t fineCells = 0;        // 今の細かいセルの数
        uint32_t refineSteps = 0;      // セルを分けた・戻した刻みの数
        uint32_t rebuilds =
            0;  // 法則(SupportLaw)を作り直した数(荷を置いた・掘った・割れが進んだ・分けた。費用の測り。T-1134)
    };

    class SupportFracture {
    public:
        SupportFracture(const SupportScene& scene, const SupportOptions& options, const SupportStrength& strength,
                        const SupportFractureOptions& fracture);

        // セルに足す下向きの力(mN。並びは scene の [x + width × up])
        void SetLoads(const std::vector<int64_t>& loadMn);
        // 掘る: セル (x, up) を取り除く(地面につながらなくなった所は分かれる)
        void Dig(const std::vector<std::array<uint32_t, 2>>& cells);
        // 1 刻み: 釣り合いを解き、面ごとに割れの進みを進める(全部の面を同じ解から決めてから当てる = 順によらない)
        void Step();

        // --- 読む ---
        const SupportLaw& Law() const { return *m_law; }
        const SupportScene& Scene() const { return m_scene; }
        // 面(セル (x, up) と +軸の隣の間)の割れの進み(Q16。SUPPORT_INTACT_FULL = 割れ切った)
        uint32_t Progress(uint32_t x, uint32_t up, uint32_t axis) const;
        // 細かい格子の面(細かい座標 (fx, fup) と +軸の隣の間)の割れの進み(Q16。細かい格子で持つ時だけ。ほかは 0)
        uint32_t FineProgress(uint32_t fx, uint32_t fup, uint32_t axis) const;
        bool FineMode() const { return m_fineMode; }
        // 細かい格子の面の強さの比(Q16。縁の応力で。粗い面は 2 つの場所のどちらでも同じ値。面が無ければ 0)
        uint32_t FineFailureIndex(uint32_t fx, uint32_t fup, uint32_t axis) const;
        // 分かれた印(並びは scene。1 = 割れて地面・壁につながらなくなり、釣り合いから外したセル。物の物理へ渡すのは R-O1)
        const std::vector<uint8_t>& Separated() const { return m_separated; }
        const SupportFractureLedger& Ledger() const { return m_ledger; }
        uint64_t FractureEnergyMjPerM2() const { return m_fractureEnergy; }
        // 軟化の比 r = 割れ切る伸び ÷ 割れ始める伸び = 2 × (E G ÷ σt²) ÷ セル(Q16)。1 以下ならセルが粗すぎて軟化できない(もろく割れる)
        uint64_t SofteningRatio() const { return m_softening; }
        // 面の強さの比(Q16。1 = SUPPORT_INTACT_FULL で強さちょうど。割れていない硬さに戻した応力で)
        uint32_t FailureIndex(uint32_t x, uint32_t up, uint32_t axis) const;
        uint64_t Hash() const;

    private:
        // 3 本目(Window)の面 1 つの値と区間の和(T-1069)
        struct WindowFace {
            int64_t pull = 0;  // 引きの側の応力の平均 × 面積(割れていない硬さに戻して)
            int64_t push = 0;
            int64_t axial = 0;
            int64_t shear = 0;
            int64_t energyPj = 0;
            SupportFaceForces forces;
            uint32_t progress = 0;
            bool present = false;  // 割れ切っていない面がある
        };

        struct WindowSum {
            // --- 強さを見る範囲の和 ---
            int64_t pull = 0;
            int64_t push = 0;
            int64_t axial = 0;
            int64_t shear = 0;
            uint32_t rangeCount = 0;

            // --- 持ち主の区間の面 ---
            int64_t energyPj = 0;
            int64_t heatPj = 0;
            uint32_t count = 0;
            uint32_t progress = SUPPORT_INTACT_FULL;  // 面の進みの小さい方
            uint32_t target = 0;                      // 今の解で進んでよい所
        };

        static constexpr uint32_t NO_WINDOW = 0xffffffffu;
        static constexpr size_t NO_FACE = ~size_t{0};

        // --- 細かい格子で持つ時(T-1067。support_fracture_fine.cpp)---
        // 面の種類ごとの値(粗い面・細かい面・違う大きさの面)
        enum FaceKindIndex : uint32_t { FACE_COARSE = 0, FACE_FINE = 1, FACE_MIXED = 2, FACE_KINDS = 3 };

        struct FaceKind {
            uint32_t slots = 0;      // 面の幅(細かい格子の数)
            uint64_t areaMm2 = 0;    // 面の幅 × 奥行
            uint64_t surfacePj = 0;  // G × 面積
            uint64_t unitPj = 0;     // 引きの強さちょうどで面の梁(長さ h)が一様に伸びた時のエネルギー σt² A h ÷ (2E)
            uint64_t softening = 0;  // r = G A ÷ U_t(Q16)
        };

        struct FineFace {
            uint32_t kind = FACE_COARSE;
            uint32_t axis = 0;
            uint32_t key = 0;                 // 細かい格子の低い側のセル(fx + 2 width × fup)
            uint32_t pairKey = 0;             // 粗い面の 2 つ目の細かい面(粗い面でなければ key)
            uint32_t line = 0;                // 面の並ぶ線(x の面は fx・上の面は fup)
            uint32_t start = 0;               // 線に沿った位置(細かい格子)
            std::array<uint32_t, 2> cells{};  // 両側の場面のセル([x + width × up])

            // --- 今の解の値(割れていない硬さに戻して)---
            int64_t pull = 0;  // 引きの側の応力の平均 × 面積
            int64_t push = 0;
            int64_t edgePull = 0;  // 縁の引きの力(1 本目・2 本目と細かくする判定)
            int64_t edgePush = 0;
            int64_t axial = 0;
            int64_t shear = 0;
            int64_t energyPj = 0;
            SupportFaceForces forces;
            uint32_t progress = 0;
            bool present = false;  // 割れ切っていない面がある
        };

        struct FineWindow {
            // --- 強さを見る範囲の和 ---
            int64_t pull = 0;
            int64_t push = 0;
            int64_t axial = 0;
            int64_t shear = 0;
            uint64_t areaMm2 = 0;

            // --- 持ち主の区間の面 ---
            int64_t energyPj = 0;
            uint64_t unitPj = 0;
            uint64_t surfacePj = 0;
            int64_t heatPj = 0;
            uint32_t count = 0;
            uint32_t progress = SUPPORT_INTACT_FULL;
            uint64_t widthMm = 0;  // 持ち主の区間の面の幅の和(割れが渡る幅。T-1135)
            uint32_t target = 0;
        };

        void InitFine();
        uint32_t SlotState(uint32_t fx, uint32_t fup) const;  // 0 = 無い・1 = 分けていないセル・2 = 細かいセル
        std::vector<FineFace> FineFaces(bool read) const;
        bool FaceAt(uint32_t fx, uint32_t fup, uint32_t axis, FineFace& face) const;
        void ReadFineFace(FineFace& face) const;
        void FineStep();
        bool FineWindowStep(const std::vector<FineFace>& faces);
        void AssignFineRun(const std::vector<size_t>& run, const std::vector<FineFace>& faces, uint32_t span,
                           std::vector<uint32_t>& homeOf, std::vector<FineWindow>& windows) const;
        bool AdvanceFineWindows(const std::vector<FineFace>& faces, const std::vector<uint32_t>& homeOf,
                                std::vector<FineWindow>& windows);
        int64_t FineFaceHeat(const FineFace& face, uint32_t reach) const;
        bool Refine(const std::vector<FineFace>& faces);
        bool CanMerge(uint32_t x, uint32_t up) const;
        void ApplyFineProgress();
        void SeparateFine();
        uint32_t IndexFromArea(int64_t pull, int64_t push, int64_t axial, int64_t shear, uint64_t areaMm2) const;
        uint32_t TargetFromSums(uint32_t index, int64_t energyPj, uint64_t unitPj, uint64_t surfacePj,
                                uint32_t progress) const;
        uint32_t EnergyIndexOf(int64_t energyPj, uint64_t unitPj) const;
        int64_t SurfaceOf(uint32_t progress, uint64_t surfacePj, uint64_t softening) const;
        std::vector<uint32_t>& FineProgressOf(uint32_t axis) { return axis == 0 ? m_progressFineX : m_progressFineUp; }

        static int64_t PositivePart(int64_t a, int64_t b);
        int64_t FaceHeat(const WindowFace& face, uint32_t reach) const;
        int64_t WindowHeat(const std::vector<WindowFace>& faces, const std::vector<size_t>& nextOf, size_t first,
                           uint32_t reach) const;

        WindowFace ReadWindowFace(uint32_t x, uint32_t up, uint32_t axis) const;
        void AssignRun(const std::vector<size_t>& run, uint32_t span, const std::vector<WindowFace>& faces,
                       std::vector<uint32_t>& homeOf, std::vector<WindowSum>& windows);
        bool AdvanceWindows(const std::vector<WindowFace>& faces, const std::vector<uint32_t>& homeOf,
                            std::vector<WindowSum>& windows);
        uint32_t IndexOf(const SupportFaceForces& forces) const;
        uint32_t IndexFrom(int64_t pull, int64_t push, int64_t axial, int64_t shear) const;
        uint32_t EnergyIndex(int64_t undamagedPj) const;
        uint32_t TargetProgress(const SupportFaceForces& forces, uint32_t progress) const;
        uint32_t TargetFrom(uint32_t index, int64_t undamagedPj, uint32_t progress) const;
        bool WindowStep();
        int64_t SurfacePj(uint32_t progress) const;
        uint32_t SpeedStepFor(uint64_t widthMm) const;
        bool Advance(uint32_t x, uint32_t up, uint32_t axis);
        void Separate();
        void Rebuild();
        std::vector<uint32_t>& ProgressOf(uint32_t axis) { return axis == 0 ? m_progressX : m_progressUp; }

        SupportScene m_scene;
        SupportOptions m_options;
        SupportStrength m_strength;
        SupportFractureOptions m_fracture;
        std::unique_ptr<SupportLaw> m_law;

        uint64_t m_fractureEnergy = 0;  // G(mJ/m²)
        uint64_t m_faceSurfacePj = 0;   // G × セルの面積(pJ)
        uint64_t m_faceUnitPj = 0;      // 面が引きの強さちょうどで一様に伸びた時のエネルギー σt² dx³ ÷ (2E)(pJ)
        uint64_t m_softening = 0;       // r(Q16)
        uint32_t m_speedStep = 0;       // 面 1 つ(1 本目・2 本目)の 1 刻みに進む割合の上限(Q16)
        uint64_t m_rootStrength = 0;    // √(σc σt)(kPa。くっつく強さ c = これ ÷ 2)
        uint64_t m_windowMm = 0;        // ℓ(Window の区間の長さ)

        std::vector<uint32_t> m_progressX;
        std::vector<uint32_t> m_progressUp;
        std::vector<uint32_t> m_nextX;  // 同じ解から決めた次の進み(当てる前)
        std::vector<uint32_t> m_nextUp;
        std::vector<uint8_t> m_separated;
        SupportFractureLedger m_ledger;

        // --- 細かい格子で持つ時(T-1067)---
        bool m_fineMode = false;
        std::array<FaceKind, FACE_KINDS> m_kinds{};
        std::vector<uint32_t> m_progressFineX;  // [fx + 2 width × fup]
        std::vector<uint32_t> m_progressFineUp;
        std::vector<uint32_t> m_nextFineX;
        std::vector<uint32_t> m_nextFineUp;
        std::vector<uint16_t> m_calmTicks;  // 分けたセルの、比が coarsenIndex 未満の刻みが続いた数([x + width × up])
    };

}  // namespace bicameral::sim
