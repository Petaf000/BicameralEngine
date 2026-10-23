// light_exchange_probe.cpp — 研究 T-1062: 世界の中の光を「面どうしの対のつながり(段に分けたラジオシティ)」で運ぶ試作。
// 距離ごとの段(RC。sim/cascade_light・cascade_light_test)と同じ場面で並べるための、捨てる測りの道具(CLAUDE.md §1.10)。
//
// 考え方(芯): 刻みごとに変わるのは「放つ量(温度)と照り返しの持ち越し」だけで、光の通り道(どの面がどの面をどれだけ見るか)は
// 形が変わった時にしか変わらない。そこで通り道を面どうしの対のつながり(係数 G_ij = G_ji。形だけで決まる)として作っておき、
// 刻みでは整数の掛け算と足し算だけで配る。遠い対は世界の木のブロック(2^k セル)どうしのつながりにまとめる(段に分けたラジオシティ)。
//   - 刻みに乱数が無いので瞬間の筋が無い(誤差は形で決まった偏りだけ)。
//   - G が対称なので、同じ温度の閉じた箱では行きと帰りが同じ整数になり、坂を下るは構造で守れる(ADR-0252 の対の形と同じ)。
//   - 空へ抜ける割合 Φ は面ごとに形から作り、つながり + Φ で配り切れない残りは自分へ返す(閉じた箱で光が壁を抜けない)。
// データの流れ: 場面(セルの物質・温度)→ 面(固い物のセルの、空気に向いた面)→ 木(ブロックごとの面の組・軸ごと)
//   → 2 つの木を一緒に下りてつながりを作る(よく離れた組はまとめ、近い対は面の上の点の積分 + 見えるかの光線)
//   → 刻み: 面の出す量 → 組の和 → つながりごとに 行き・帰り を整数で → 組から面へ分け直す → 吸う・返す(持ち越し)・空へ。
// 形を作る所(つながりの係数・空の割合)と組の中の 1 次の重みはこの試作では double。法則に入れるなら整数で作る(T-1079)。
// 刻みの量(放つ・配る・吸う・返す・空へ)は整数で、保存はビット単位。ツールなので浮動小数点を使ってよい(CLAUDE.md 原則 2)。
// 使い方: light_exchange_probe [theta=0.5] [n|h|r|g|c] [勾配の上限=0.25] [c の場面の番号]
//   n = 全部(64³ を除く)・h = 64³ も・r = 筋と箱と板・g = 地面の受け方(1 本目と 2 本目)・c = 重い場面を 1 つ。
//   f = 炎・形の変化・煙の道筋(T-1080 の 1 本目。c の場面の番号の代わりに地形の大きさ)・m = 動く物の箱(T-1080 の 2 本目)。
//   Linux でも g++ -std=c++20 -O2 で通る(研究の時の回し方)。
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <numbers>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("失敗 %d 行: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // (a × b) >> 32 と (a × b) / c(128bit の途中の値。MSVC と g++ で同じ結果)
    uint64_t MulShift32(uint64_t a, uint64_t b) {
#if defined(_MSC_VER)
        uint64_t high = 0;
        const uint64_t low = _umul128(a, b, &high);

        return (high << 32) | (low >> 32);
#else
        return static_cast<uint64_t>((static_cast<unsigned __int128>(a) * b) >> 32);
#endif
    }

    uint64_t MulDiv(uint64_t a, uint64_t b, uint64_t c) {
#if defined(_MSC_VER)
        uint64_t high = 0;
        const uint64_t low = _umul128(a, b, &high);
        uint64_t remainder = 0;

        return _udiv128(high, low, c, &remainder);
#else
        return static_cast<uint64_t>((static_cast<unsigned __int128>(a) * b) / c);
#endif
    }

    // --- 場面 ---

    enum class Material : uint8_t {
        Air,
        Stone,  // 灰色で通さない・照り返し 0.5(D-610 の既定。RC の試験と同じ)
        Black,  // 通さない・照り返し 0
        Flame,  // 炎(毎刻み動く熱い気体。T-1080): 光線は通す・外側の面から放つ(体積の光源を面の要素にする仮の形)
    };

    struct Voxel {
        Material material = Material::Air;
        double temperatureK = 0;
    };

    constexpr double FIRE_K = 1200;
    constexpr double ROOM_K = 293;
    constexpr double QUIET_K = 500;  // RC の試験と同じく、室温の物は放たない(焚き火の光だけを見る)
    constexpr double CELL_M = 0.5;
    constexpr double TICK_S = 1.0 / 60.0;
    constexpr double SIGMA = 5.670374419e-8;
    constexpr double UNIT_PER_J = 1000.0 * 65536.0;  // 量は 2^−16 mJ(RC と同じ)

    struct Scene {
        int sx = 0;
        int sy = 0;
        int sz = 0;
        double cellM = CELL_M;
        std::vector<Voxel> voxels;

        [[nodiscard]] bool Inside(int x, int y, int z) const {
            return x >= 0 && y >= 0 && z >= 0 && x < sx && y < sy && z < sz;
        }

        [[nodiscard]] size_t IndexOf(int x, int y, int z) const { return ((static_cast<size_t>(z) * sy) + y) * sx + x; }

        [[nodiscard]] bool Opaque(int x, int y, int z) const {
            if (!Inside(x, y, z))
                return false;

            const Material material = voxels[IndexOf(x, y, z)].material;

            return material != Material::Air && material != Material::Flame;
        }
    };

    Scene MakeScene(int sx, int sy, int sz, const std::function<Voxel(int, int, int)>& shape) {
        Scene scene;
        scene.sx = sx;
        scene.sy = sy;
        scene.sz = sz;
        scene.voxels.resize(static_cast<size_t>(sx) * sy * sz);
        for (int z = 0; z < sz; ++z) {
            for (int y = 0; y < sy; ++y) {
                for (int x = 0; x < sx; ++x)
                    scene.voxels[scene.IndexOf(x, y, z)] = shape(x, y, z);
            }
        }

        return scene;
    }

    // --- 面 ---

    using Vec = std::array<double, 3>;

    Vec Add(Vec a, Vec b) {
        return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
    }
    Vec Sub(Vec a, Vec b) {
        return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    }
    Vec Scale(Vec a, double s) {
        return {a[0] * s, a[1] * s, a[2] * s};
    }
    double Dot(Vec a, Vec b) {
        return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
    }

    // 面の向き dir = 軸 × 2 + (負の面なら 1)。RC の面の番号と同じ
    Vec NormalOf(int dir) {
        Vec n{0, 0, 0};
        n[dir / 2] = (dir % 2) != 0 ? -1.0 : 1.0;
        return n;
    }

    struct Face {
        int x = 0;
        int y = 0;
        int z = 0;
        int dir = 0;
        size_t cell = 0;
        Vec center{};
        uint32_t reflectanceQ16 = 0;
        uint64_t emission = 0;  // 1 刻みに熱で放つ量(2^−16 mJ)
    };

    // 面の上の点(u, v は −0.5〜0.5)。空気の側へごく僅かずらす(光線の始まりを空気のセルにする)
    Vec PointOnFace(const Face& face, double u, double v) {
        const int axis = face.dir / 2;
        Vec p = face.center;
        p[(axis + 1) % 3] += u;
        p[(axis + 2) % 3] += v;

        return Add(p, Scale(NormalOf(face.dir), 1e-6));
    }

    std::vector<Face> CollectFaces(const Scene& scene) {
        std::vector<Face> faces;
        for (int z = 0; z < scene.sz; ++z) {
            for (int y = 0; y < scene.sy; ++y) {
                for (int x = 0; x < scene.sx; ++x) {
                    const Voxel& voxel = scene.voxels[scene.IndexOf(x, y, z)];
                    if (voxel.material == Material::Air)
                        continue;

                    for (int dir = 0; dir < 6; ++dir) {
                        const Vec n = NormalOf(dir);
                        const int nx = x + static_cast<int>(n[0]);
                        const int ny = y + static_cast<int>(n[1]);
                        const int nz = z + static_cast<int>(n[2]);
                        if (scene.Opaque(nx, ny, nz))
                            continue;

                        // 炎どうしの内側の面は要素にしない(炎の塊の外側だけが放つ)
                        const bool neighborFlame = scene.Inside(nx, ny, nz) &&
                                                   scene.voxels[scene.IndexOf(nx, ny, nz)].material == Material::Flame;
                        if (voxel.material == Material::Flame && neighborFlame)
                            continue;

                        Face face{.x = x, .y = y, .z = z, .dir = dir, .cell = scene.IndexOf(x, y, z)};
                        face.center = Add({x + 0.5, y + 0.5, z + 0.5}, Scale(n, 0.5));
                        const double reflectance = voxel.material == Material::Stone ? 0.5 : 0.0;
                        face.reflectanceQ16 = static_cast<uint32_t>(reflectance * 65536);
                        if (voxel.temperatureK >= QUIET_K) {
                            const double areaM2 = scene.cellM * scene.cellM;
                            const double joules = SIGMA * std::pow(voxel.temperatureK, 4) * areaM2 * (1 - reflectance) *
                                                  TICK_S;
                            face.emission = static_cast<uint64_t>(joules * UNIT_PER_J);
                        }

                        faces.push_back(face);
                    }
                }
            }
        }

        return faces;
    }

    // --- 光線(形を作る時だけ。刻みでは歩かない)---

    struct RayCounter {
        uint64_t rays = 0;
        uint64_t steps = 0;
    };

    // p0 から p1 まで(どちらも空気のセルの中)、固い物のセルを通らなければ true
    bool Visible(const Scene& scene, Vec p0, Vec p1, RayCounter& counter) {
        ++counter.rays;
        const Vec d = Sub(p1, p0);
        std::array<int, 3> cell{static_cast<int>(std::floor(p0[0])), static_cast<int>(std::floor(p0[1])),
                                static_cast<int>(std::floor(p0[2]))};
        const std::array<int, 3> last{static_cast<int>(std::floor(p1[0])), static_cast<int>(std::floor(p1[1])),
                                      static_cast<int>(std::floor(p1[2]))};
        std::array<int, 3> step{};
        Vec tMax{};
        Vec tDelta{};
        for (int a = 0; a < 3; ++a) {
            step[a] = d[a] > 0 ? 1 : (d[a] < 0 ? -1 : 0);
            if (step[a] == 0) {
                tMax[a] = 1e30;
                tDelta[a] = 1e30;
                continue;
            }

            const double boundary = step[a] > 0 ? cell[a] + 1.0 : static_cast<double>(cell[a]);
            tMax[a] = (boundary - p0[a]) / d[a];
            tDelta[a] = std::fabs(1.0 / d[a]);
        }

        while (cell != last) {
            int a = 0;
            if (tMax[1] < tMax[a])
                a = 1;

            if (tMax[2] < tMax[a])
                a = 2;

            if (tMax[a] > 1.0)
                break;

            cell[a] += step[a];
            tMax[a] += tDelta[a];
            ++counter.steps;
            if (scene.Opaque(cell[0], cell[1], cell[2]))
                return false;
        }

        return true;
    }

    // p0 から向き d へ、世界の外へ出れば true(空へ抜ける)
    bool Escapes(const Scene& scene, Vec p0, Vec d, RayCounter& counter) {
        const double far = 4.0 * (scene.sx + scene.sy + scene.sz);
        Vec p1 = Add(p0, Scale(d, far));
        // 世界の箱で切る(箱の外の点まで歩けば外へ出たと分かる)
        ++counter.rays;
        std::array<int, 3> cell{static_cast<int>(std::floor(p0[0])), static_cast<int>(std::floor(p0[1])),
                                static_cast<int>(std::floor(p0[2]))};
        const Vec dd = Sub(p1, p0);
        std::array<int, 3> step{};
        Vec tMax{};
        Vec tDelta{};
        for (int a = 0; a < 3; ++a) {
            step[a] = dd[a] > 0 ? 1 : (dd[a] < 0 ? -1 : 0);
            if (step[a] == 0) {
                tMax[a] = 1e30;
                tDelta[a] = 1e30;
                continue;
            }

            const double boundary = step[a] > 0 ? cell[a] + 1.0 : static_cast<double>(cell[a]);
            tMax[a] = (boundary - p0[a]) / dd[a];
            tDelta[a] = std::fabs(1.0 / dd[a]);
        }

        while (scene.Inside(cell[0], cell[1], cell[2])) {
            if (scene.Opaque(cell[0], cell[1], cell[2]))
                return false;

            int a = 0;
            if (tMax[1] < tMax[a])
                a = 1;

            if (tMax[2] < tMax[a])
                a = 2;

            cell[a] += step[a];
            tMax[a] += tDelta[a];
            ++counter.steps;
        }

        return true;
    }

    // 遮る物の無い面と面の G(セルの面積を 1 とした単位)。mm × mm の点どうしの積分
    double GeometricExchange(const Face& fa, const Face& fb, int mm) {
        const Vec na = NormalOf(fa.dir);
        const Vec nb = NormalOf(fb.dir);
        double g = 0;
        const double weight = 1.0 / (static_cast<double>(mm) * mm * mm * mm);
        for (int i = 0; i < mm * mm; ++i) {
            const Vec pa = PointOnFace(fa, ((i % mm) + 0.5) / mm - 0.5, ((i / mm) + 0.5) / mm - 0.5);
            for (int j = 0; j < mm * mm; ++j) {
                const Vec pb = PointOnFace(fb, ((j % mm) + 0.5) / mm - 0.5, ((j / mm) + 0.5) / mm - 0.5);
                const Vec d = Sub(pb, pa);
                const double r2 = Dot(d, d);
                const double cosA = std::max(0.0, Dot(na, d));
                const double cosB = std::max(0.0, -Dot(nb, d));
                g += weight * cosA * cosB / (std::numbers::pi * r2 * r2);
            }
        }

        return g;
    }

    // --- 木(ブロックごと・面の向きごとの組)---

    struct Group {
        int node = 0;
        int dir = 0;
        std::vector<int> faces;  // 面の番号(小さい順)
        Vec centroid{};
        Vec boxMin{};
        Vec boxMax{};
        std::vector<int> children;  // 子のブロックの同じ向きの組
        int parent = -1;
    };

    struct Node {
        int level = 0;
        std::array<int, 3> block{};
        std::vector<int> children;
        std::array<int, 6> groups{-1, -1, -1, -1, -1, -1};
    };

    struct Link {
        int groupA = 0;
        int groupB = 0;
        uint64_t fractionAbQ32 = 0;  // A が出す量のうち B へ行く割合
        uint64_t fractionBaQ32 = 0;
        // 受け手 B の位置で ln K の勾配(K = 点どうしの G。組の中の 1 次の変わり方。2 本目)。A の位置の勾配は −gradientB
        Vec gradientB{};
    };

    struct Settings {
        double theta = 0.5;        // よく離れた: (半径 A + 半径 B) < theta × 距離
        bool linear = true;        // 組の中を 1 次(勾配)で分ける(2 本目)。false = 面の数の比(1 本目)
        double slopeLimit = 0.25;  // 0 より大きければ、|∇ln K| × 組の半径 がこれを超えるつながりは細かくする(2 本目)
        int skyRays = 64;          // 空へ抜ける割合を作る向きの数(面ごと。決まった向き)
        // 形が変わったセル。指定すると、そのセルを通りうる対(2 つのブロックの包み)だけを作り直す(費用の見込み)
        bool rebuildOnly = false;
        std::array<int, 3> changedCell{};
    };

    struct Network {
        std::vector<Face> faces;
        std::vector<Node> nodes;
        std::vector<Group> groups;
        std::vector<int> groupsByLevel;  // 葉から根の順
        std::vector<Link> links;
        std::vector<uint64_t> skyQ32;  // 面ごとの空へ抜ける割合
        RayCounter rays;
        uint64_t leafPairs = 0;
        uint64_t clusterLinks = 0;
        uint64_t leafLinks = 0;
        double worstLinkSum = 0;  // 面ごとの つながりの割合の和 の最大(1 を超えたら縮めた)
        double worstSelf = 0;     // 面ごとの「配り切れず自分へ返す」割合の最大
        double meanSelf = 0;
        double buildMs = 0;
        bool linear = true;
        // 決めた対(つながりを作った・遮られて作らなかった・面と面を測った)。形が変わった時に作り直す対を探す(T-1080)
        std::vector<std::array<int, 2>> records;
        uint64_t pairRays = 0;  // Rebuild で、決め直した対に使った光線(残りは新しい要素)
    };

    class Builder {
    public:
        Builder(const Scene& scene, const Settings& settings, Network& network)
            : m_scene(scene), m_settings(settings), m_net(network) {}

        void Build() {
            const auto start = std::chrono::steady_clock::now();
            m_net.linear = m_settings.linear;
            m_net.faces = CollectFaces(m_scene);
            BuildTree();
            Refine(m_root, m_root);
            if (!m_settings.rebuildOnly)
                NormalizeAndSky();

            m_net.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }

        // 形が変わった後の場面で、選んだ対(前の木のブロックの場所で渡す)と新しい要素(セル)だけを作り直す。光線の数を数える
        void Rebuild(const std::vector<std::array<std::array<int, 4>, 2>>& pairs,
                     const std::vector<std::array<int, 3>>& cells) {
            m_net.linear = m_settings.linear;
            m_net.faces = CollectFaces(m_scene);
            BuildTree();
            for (const auto& pair : pairs) {
                const auto a = m_index.find(KeyOf(pair[0][0], pair[0][1], pair[0][2], pair[0][3]));
                const auto b = m_index.find(KeyOf(pair[1][0], pair[1][1], pair[1][2], pair[1][3]));
                if (a != m_index.end() && b != m_index.end())
                    Refine(a->second, b->second);
            }

            m_net.pairRays = m_net.rays.rays;

            for (const auto& cell : cells) {
                const auto leaf = m_index.find(KeyOf(0, cell[0], cell[1], cell[2]));
                if (leaf != m_index.end())
                    Refine(leaf->second, m_root);
            }
        }

    private:
        static uint64_t KeyOf(int level, int bx, int by, int bz) {
            return (static_cast<uint64_t>(level) << 60) | (static_cast<uint64_t>(bx) << 40) |
                   (static_cast<uint64_t>(by) << 20) | static_cast<uint64_t>(bz);
        }

        int NodeAt(int level, std::array<int, 3> block) {
            const uint64_t key = KeyOf(level, block[0], block[1], block[2]);
            const auto found = m_index.find(key);
            if (found != m_index.end())
                return found->second;

            const int id = static_cast<int>(m_net.nodes.size());
            m_net.nodes.push_back(Node{.level = level, .block = block});
            m_index.emplace(key, id);

            return id;
        }

        int GroupAt(int node, int dir) {
            int& id = m_net.nodes[node].groups[dir];
            if (id >= 0)
                return id;

            id = static_cast<int>(m_net.groups.size());
            Group group{.node = node, .dir = dir};
            group.boxMin = {1e30, 1e30, 1e30};
            group.boxMax = {-1e30, -1e30, -1e30};
            m_net.groups.push_back(group);

            return id;
        }

        void BuildTree() {
            int top = 0;
            while ((1 << top) < std::max({m_scene.sx, m_scene.sy, m_scene.sz}))
                ++top;

            for (size_t f = 0; f < m_net.faces.size(); ++f) {
                const Face& face = m_net.faces[f];
                int child = -1;
                int childGroup = -1;
                for (int level = 0; level <= top; ++level) {
                    const int node = NodeAt(level, {face.x >> level, face.y >> level, face.z >> level});
                    const int group = GroupAt(node, face.dir);
                    Group& g = m_net.groups[group];
                    if (g.faces.empty() || g.faces.back() != static_cast<int>(f))
                        g.faces.push_back(static_cast<int>(f));

                    for (int a = 0; a < 3; ++a) {
                        g.boxMin[a] = std::min(g.boxMin[a], face.center[a]);
                        g.boxMax[a] = std::max(g.boxMax[a], face.center[a]);
                        g.centroid[a] += face.center[a];
                    }

                    if (child >= 0) {
                        auto& kids = m_net.nodes[node].children;
                        if (std::ranges::find(kids, child) == kids.end())
                            kids.push_back(child);

                        auto& groupKids = m_net.groups[group].children;
                        if (std::ranges::find(groupKids, childGroup) == groupKids.end())
                            groupKids.push_back(childGroup);

                        m_net.groups[childGroup].parent = group;
                    }

                    child = node;
                    childGroup = group;
                }
            }

            for (Group& g : m_net.groups)
                g.centroid = Scale(g.centroid, 1.0 / static_cast<double>(g.faces.size()));

            m_root = NodeAt(top, {0, 0, 0});
            // 葉から根の順(刻みの和と分け直しで使う)
            for (int level = 0; level <= top; ++level) {
                for (size_t g = 0; g < m_net.groups.size(); ++g) {
                    if (m_net.nodes[m_net.groups[g].node].level == level)
                        m_net.groupsByLevel.push_back(static_cast<int>(g));
                }
            }
        }

        [[nodiscard]] Vec CenterOf(int node) const {
            const Node& n = m_net.nodes[node];
            const double size = static_cast<double>(1 << n.level);

            return {(n.block[0] + 0.5) * size, (n.block[1] + 0.5) * size, (n.block[2] + 0.5) * size};
        }

        // 組 a の面から組 b の面のどれかが前に見え、逆も同じになりうるか(箱で)
        [[nodiscard]] bool GroupsCanFace(const Group& a, const Group& b) const {
            const int axisA = a.dir / 2;
            const int axisB = b.dir / 2;
            const bool aSeesB = (a.dir % 2) == 0 ? b.boxMax[axisA] > a.boxMin[axisA] + 1e-9
                                                 : b.boxMin[axisA] < a.boxMax[axisA] - 1e-9;
            const bool bSeesA = (b.dir % 2) == 0 ? a.boxMax[axisB] > b.boxMin[axisB] + 1e-9
                                                 : a.boxMin[axisB] < b.boxMax[axisB] - 1e-9;

            return aSeesB && bSeesA;
        }

        [[nodiscard]] bool NodesCanFace(int a, int b) const {
            for (const int ga : m_net.nodes[a].groups) {
                if (ga < 0)
                    continue;

                for (const int gb : m_net.nodes[b].groups) {
                    if (gb >= 0 && GroupsCanFace(m_net.groups[ga], m_net.groups[gb]))
                        return true;
                }
            }

            return false;
        }

        // 変わったセルが 2 つのブロックの包み(中心を結ぶ線から 大きい方の半径 + セルの半径 以内)に入りうるか
        [[nodiscard]] bool HullMayContain(int a, int b) const {
            const Vec cell{m_settings.changedCell[0] + 0.5, m_settings.changedCell[1] + 0.5,
                           m_settings.changedCell[2] + 0.5};
            const Vec ca = CenterOf(a);
            const Vec cb = CenterOf(b);
            const Vec ab = Sub(cb, ca);
            const double length2 = Dot(ab, ab);
            const double t = length2 == 0 ? 0 : std::clamp(Dot(Sub(cell, ca), ab) / length2, 0.0, 1.0);
            const Vec near = Add(ca, Scale(ab, t));
            const double distance = std::sqrt(Dot(Sub(cell, near), Sub(cell, near)));
            const double radius = (std::sqrt(3.0) / 2) * std::max(1 << m_net.nodes[a].level, 1 << m_net.nodes[b].level);

            return distance <= radius + std::sqrt(3.0) / 2;
        }

        void Refine(int a, int b) {
            if (m_settings.rebuildOnly && !HullMayContain(a, b))
                return;

            const Node& na = m_net.nodes[a];
            if (a == b) {
                if (na.level == 0)
                    return;

                const std::vector<int> kids = na.children;
                for (size_t i = 0; i < kids.size(); ++i) {
                    for (size_t j = i; j < kids.size(); ++j)
                        Refine(kids[i], kids[j]);
                }

                return;
            }

            if (!NodesCanFace(a, b))
                return;

            const Node& nb = m_net.nodes[b];
            if (na.level == 0 && nb.level == 0) {
                LeafLink(a, b);
                m_net.records.push_back({a, b});
                return;
            }

            const double distance = std::sqrt(Dot(Sub(CenterOf(a), CenterOf(b)), Sub(CenterOf(a), CenterOf(b))));
            const double radii = (std::sqrt(3.0) / 2) * ((1 << na.level) + (1 << nb.level));
            if (radii < m_settings.theta * distance && ClusterLink(a, b)) {
                m_net.records.push_back({a, b});
                return;
            }

            const std::vector<int> kidsA = na.level >= nb.level ? na.children : std::vector<int>{a};
            const std::vector<int> kidsB = nb.level >= na.level ? nb.children : std::vector<int>{b};
            for (const int ka : kidsA) {
                for (const int kb : kidsB)
                    Refine(ka, kb);
            }
        }

        // 代表の面(組の面を等間隔に最大 4 つ)
        [[nodiscard]] std::vector<int> Representatives(const Group& group) const {
            const size_t count = group.faces.size();
            const size_t want = std::min<size_t>(4, count);
            std::vector<int> picked;
            for (size_t i = 0; i < want; ++i)
                picked.push_back(group.faces[((2 * i + 1) * count) / (2 * want)]);

            return picked;
        }

        // よく離れた 2 つのブロック: 向きの組ごとに点の近似 G = N_a N_b cos cos / (π r²)。見えるかは代表の面の光線で。
        // 一部だけ見える組があれば false(呼ぶ側が細かくする)
        bool ClusterLink(int a, int b) {
            std::vector<Link> made;
            for (const int ga : m_net.nodes[a].groups) {
                if (ga < 0)
                    continue;

                for (const int gb : m_net.nodes[b].groups) {
                    if (gb < 0)
                        continue;

                    const Group& groupA = m_net.groups[ga];
                    const Group& groupB = m_net.groups[gb];
                    const Vec d = Sub(groupB.centroid, groupA.centroid);
                    const double r2 = Dot(d, d);
                    const double r = std::sqrt(r2);
                    const double cosA = Dot(NormalOf(groupA.dir), d) / r;
                    const double cosB = -Dot(NormalOf(groupB.dir), d) / r;
                    if (cosA <= 0 || cosB <= 0)
                        continue;

                    int seen = 0;
                    int tried = 0;
                    for (const int fa : Representatives(groupA)) {
                        for (const int fb : Representatives(groupB)) {
                            ++tried;
                            seen += Visible(m_scene, PointOnFace(m_net.faces[fa], 0, 0),
                                            PointOnFace(m_net.faces[fb], 0, 0), m_net.rays)
                                        ? 1
                                        : 0;
                        }
                    }

                    if (seen != 0 && seen != tried)
                        return false;

                    if (seen == 0)
                        continue;

                    const double na = static_cast<double>(groupA.faces.size());
                    const double nb = static_cast<double>(groupB.faces.size());
                    const double g = na * nb * cosA * cosB / (std::numbers::pi * r2);
                    // ∇_b ln K = n_A / (n_A·d) + n_B / (n_B·d) − 4 d / |d|²(d = b − a)
                    const Vec nA = NormalOf(groupA.dir);
                    const Vec nB = NormalOf(groupB.dir);
                    const Vec gradient = Sub(Add(Scale(nA, 1 / Dot(nA, d)), Scale(nB, 1 / Dot(nB, d))),
                                             Scale(d, 4 / r2));
                    // 組の面の広がり(箱の半分)の上で ln K が変わる大きさ。超えるなら細かくする(1 次で足りない所)
                    if (m_settings.slopeLimit > 0) {
                        double spreadA = 0;
                        double spreadB = 0;
                        for (int axis = 0; axis < 3; ++axis) {
                            spreadA += std::fabs(gradient[axis]) * (groupA.boxMax[axis] - groupA.boxMin[axis]) / 2;
                            spreadB += std::fabs(gradient[axis]) * (groupB.boxMax[axis] - groupB.boxMin[axis]) / 2;
                        }

                        if (std::max(spreadA, spreadB) > m_settings.slopeLimit)
                            return false;
                    }

                    made.push_back(Link{.groupA = ga,
                                        .groupB = gb,
                                        .fractionAbQ32 = static_cast<uint64_t>(std::llround(g / na * 4294967296.0)),
                                        .fractionBaQ32 = static_cast<uint64_t>(std::llround(g / nb * 4294967296.0)),
                                        .gradientB = gradient});
                }
            }

            m_net.clusterLinks += made.size();
            m_net.links.insert(m_net.links.end(), made.begin(), made.end());

            return true;
        }

        // 面と面の点の積分(見えるかは partial の時だけ点ごとに光線)
        double Quadrature(const Face& fa, const Face& fb, int mm, bool partial) {
            const Vec na = NormalOf(fa.dir);
            const Vec nb = NormalOf(fb.dir);
            double g = 0;
            const double weight = 1.0 / (static_cast<double>(mm) * mm * mm * mm);
            for (int i = 0; i < mm * mm; ++i) {
                const Vec pa = PointOnFace(fa, ((i % mm) + 0.5) / mm - 0.5, ((i / mm) + 0.5) / mm - 0.5);
                for (int j = 0; j < mm * mm; ++j) {
                    const Vec pb = PointOnFace(fb, ((j % mm) + 0.5) / mm - 0.5, ((j / mm) + 0.5) / mm - 0.5);
                    const Vec d = Sub(pb, pa);
                    const double r2 = Dot(d, d);
                    const double cosA = std::max(0.0, Dot(na, d));
                    const double cosB = std::max(0.0, -Dot(nb, d));
                    if (cosA <= 0 || cosB <= 0)
                        continue;

                    if (partial && !Visible(m_scene, pa, pb, m_net.rays))
                        continue;

                    g += weight * cosA * cosB / (std::numbers::pi * r2 * r2);
                }
            }

            return g;
        }

        // 遮る物の無い面と面の G を、ずれ(セル)と 2 つの向きで引く表(ベイクする形の代わり。細かい積分 m = 24 で一度だけ)
        double UnoccludedExchange(const Face& fa, const Face& fb) {
            const int dx = fb.x - fa.x;
            const int dy = fb.y - fa.y;
            const int dz = fb.z - fa.z;
            const uint64_t key = (static_cast<uint64_t>(dx + 512) << 40) | (static_cast<uint64_t>(dy + 512) << 28) |
                                 (static_cast<uint64_t>(dz + 512) << 16) | (static_cast<uint64_t>(fa.dir) << 8) |
                                 static_cast<uint64_t>(fb.dir);
            const auto found = m_table.find(key);
            if (found != m_table.end())
                return found->second;

            const double distance = std::sqrt(Dot(Sub(fb.center, fa.center), Sub(fb.center, fa.center)));
            const int mm = distance < 2 ? 24 : (distance < 4 ? 12 : (distance < 8 ? 6 : 3));
            const double g = Quadrature(fa, fb, mm, false);
            m_table.emplace(key, g);

            return g;
        }

        // 面と面: 遮る物の無い G は表から。見えるかはまず 2 × 2 の点どうし 16 本、一部なら 4 × 4 の点ごとに見える割合を掛ける
        double FacePairExchange(const Face& fa, const Face& fb) {
            const double distance = std::sqrt(Dot(Sub(fb.center, fa.center), Sub(fb.center, fa.center)));
            const int m = distance < 2 ? 6 : (distance < 4 ? 4 : (distance < 8 ? 2 : 1));
            int seen = 0;
            int tried = 0;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    const double ua = (i % 2) * 0.5 - 0.25;
                    const double va = (i / 2) * 0.5 - 0.25;
                    const double ub = (j % 2) * 0.5 - 0.25;
                    const double vb = (j / 2) * 0.5 - 0.25;
                    ++tried;
                    seen += Visible(m_scene, PointOnFace(fa, ua, va), PointOnFace(fb, ub, vb), m_net.rays) ? 1 : 0;
                }
            }

            if (seen == 0)
                return 0;

            const double open = UnoccludedExchange(fa, fb);
            if (seen == tried)
                return open;

            const int mm = std::max(m, 4);
            const double all = Quadrature(fa, fb, mm, false);

            return all <= 0 ? 0 : open * Quadrature(fa, fb, mm, true) / all;
        }

        void LeafLink(int a, int b) {
            ++m_net.leafPairs;
            for (const int ga : m_net.nodes[a].groups) {
                if (ga < 0)
                    continue;

                for (const int gb : m_net.nodes[b].groups) {
                    if (gb < 0)
                        continue;

                    const Face& fa = m_net.faces[m_net.groups[ga].faces[0]];
                    const Face& fb = m_net.faces[m_net.groups[gb].faces[0]];
                    const Vec d = Sub(fb.center, fa.center);
                    if (Dot(NormalOf(fa.dir), d) <= 0 || Dot(NormalOf(fb.dir), d) >= 0)
                        continue;

                    const double g = FacePairExchange(fa, fb);
                    if (g <= 0)
                        continue;

                    const auto q = static_cast<uint64_t>(std::llround(g * 4294967296.0));
                    m_net.links.push_back(Link{.groupA = ga, .groupB = gb, .fractionAbQ32 = q, .fractionBaQ32 = q});
                    ++m_net.leafLinks;
                }
            }
        }

        // 面ごとの「つながりで出す割合」φ_f = Σ F × (1 + h · (x_f − 組の重心))(1 次の項も含む。形だけで決まる)
        [[nodiscard]] std::vector<double> FaceFractions() const {
            std::vector<double> fraction(m_net.faces.size(), 0);
            const auto add = [&](int group, uint64_t fractionQ32, Vec slope) {
                const Group& g = m_net.groups[group];
                const double base = static_cast<double>(fractionQ32) / 4294967296.0;
                for (const int f : g.faces) {
                    const double linear = m_net.linear ? Dot(slope, Sub(m_net.faces[f].center, g.centroid)) : 0.0;
                    fraction[f] += base * (1 + linear);
                }
            };
            for (const Link& link : m_net.links) {
                add(link.groupA, link.fractionAbQ32, Scale(link.gradientB, -1));
                add(link.groupB, link.fractionBaQ32, link.gradientB);
            }

            return fraction;
        }

        // φ_f ≤ 1 − 2^−20 にし(超えたら両端の組の小さい方の倍率で縮める。対称のまま)、空へ抜ける割合を作る
        void NormalizeAndSky() {
            const size_t faceCount = m_net.faces.size();
            constexpr double LIMIT = 1.0 - (1.0 / 1048576.0);
            std::vector<double> faceSum = FaceFractions();
            std::vector<double> groupScale(m_net.groups.size(), 1.0);
            for (size_t g = 0; g < m_net.groups.size(); ++g) {
                for (const int f : m_net.groups[g].faces) {
                    m_net.worstLinkSum = std::max(m_net.worstLinkSum, faceSum[f]);
                    if (faceSum[f] > LIMIT)
                        groupScale[g] = std::min(groupScale[g], LIMIT / faceSum[f]);
                }
            }

            for (Link& link : m_net.links) {
                const double s = std::min(groupScale[link.groupA], groupScale[link.groupB]);
                if (s >= 1)
                    continue;

                link.fractionAbQ32 = static_cast<uint64_t>(static_cast<double>(link.fractionAbQ32) * s);
                link.fractionBaQ32 = static_cast<uint64_t>(static_cast<double>(link.fractionBaQ32) * s);
            }

            faceSum = FaceFractions();

            // 空: 余弦で重みを付けた決まった向き(層に分けた 8 × 8)
            const int side = static_cast<int>(std::lround(std::sqrt(m_settings.skyRays)));
            m_net.skyQ32.assign(faceCount, 0);
            double selfSum = 0;
            for (size_t f = 0; f < faceCount; ++f) {
                const Face& face = m_net.faces[f];
                const int axis = face.dir / 2;
                const Vec n = NormalOf(face.dir);
                Vec u{0, 0, 0};
                Vec v{0, 0, 0};
                u[(axis + 1) % 3] = 1;
                v[(axis + 2) % 3] = 1;
                int escaped = 0;
                for (int i = 0; i < side; ++i) {
                    for (int j = 0; j < side; ++j) {
                        const double s1 = (i + 0.5) / side;
                        const double s2 = (j + 0.5) / side;
                        const double radius = std::sqrt(s1);
                        const double angle = 2 * std::numbers::pi * s2;
                        const Vec d = Add(Add(Scale(u, radius * std::cos(angle)), Scale(v, radius * std::sin(angle))),
                                          Scale(n, std::sqrt(1 - s1)));
                        escaped += Escapes(m_scene, PointOnFace(face, 0, 0), d, m_net.rays) ? 1 : 0;
                    }
                }

                const double sky = std::min(static_cast<double>(escaped) / (side * side),
                                            std::max(0.0, 1 - faceSum[f]));
                m_net.skyQ32[f] = static_cast<uint64_t>(sky * 4294967296.0);
                const double self = std::max(0.0, 1 - faceSum[f] - sky);
                selfSum += self;
                m_net.worstSelf = std::max(m_net.worstSelf, self);
            }

            m_net.meanSelf = faceCount == 0 ? 0 : selfSum / static_cast<double>(faceCount);
        }

        const Scene& m_scene;
        const Settings& m_settings;
        Network& m_net;
        std::unordered_map<uint64_t, int> m_index;
        std::unordered_map<uint64_t, double> m_table;
        int m_root = 0;
    };

    // --- 刻み(整数だけ)---

    struct TickResult {
        std::vector<uint64_t> emittedPerCell;   // 熱で放った(セルから引く)
        std::vector<uint64_t> absorbedPerCell;  // 吸った(セルへ足す)
        std::vector<uint64_t> carry;            // 面ごとの次の刻みへの持ち越し(照り返し。D-619)
        uint64_t emitted = 0;
        uint64_t carriedIn = 0;
        uint64_t absorbed = 0;
        uint64_t reflected = 0;
        uint64_t escaped = 0;
        uint64_t selfReturned = 0;
        uint64_t objectAbsorbed = 0;  // 動く物の箱が遮って吸った(T-1080)
        bool negativeSelf = false;
    };

    // 量 total を重み weights の比で整数に分ける(余りは決まった順で 1 ずつ。和はちょうど total)
    void SplitByWeights(uint64_t total, const std::vector<uint64_t>& weights, uint64_t weightSum,
                        std::vector<uint64_t>& out) {
        out.assign(weights.size(), 0);
        if (total == 0 || weightSum == 0)
            return;

        uint64_t given = 0;
        for (size_t i = 0; i < weights.size(); ++i) {
            out[i] = MulDiv(total, weights[i], weightSum);
            given += out[i];
        }

        for (size_t i = 0; given < total; i = (i + 1) % weights.size()) {
            if (weights[i] == 0)
                continue;

            ++out[i];
            ++given;
        }
    }

    // 線分 p0–p1 が箱 [boxMin, boxMax] に入るか(板の方法)
    bool SegmentHitsBox(Vec p0, Vec p1, Vec boxMinCorner, Vec boxMaxCorner) {
        double low = 0;
        double high = 1;
        for (int a = 0; a < 3; ++a) {
            const double d = p1[a] - p0[a];
            const double boxLow = boxMinCorner[a];
            const double boxHigh = boxMaxCorner[a];
            if (std::fabs(d) < 1e-12) {
                if (p0[a] < boxLow || p0[a] > boxHigh)
                    return false;

                continue;
            }

            double t0 = (boxLow - p0[a]) / d;
            double t1 = (boxHigh - p0[a]) / d;
            if (t0 > t1)
                std::swap(t0, t1);

            low = std::max(low, t0);
            high = std::min(high, t1);
            if (low > high)
                return false;
        }

        return true;
    }

    bool SegmentHitsCell(Vec p0, Vec p1, const std::array<int, 3>& cell) {
        return SegmentHitsBox(p0, p1, {cell[0] - 1e-6, cell[1] - 1e-6, cell[2] - 1e-6},
                              {cell[0] + 1 + 1e-6, cell[1] + 1 + 1e-6, cell[2] + 1 + 1e-6});
    }

    std::vector<int> RepresentativesOf(const Group& group) {
        const size_t count = group.faces.size();
        const size_t want = std::min<size_t>(4, count);
        std::vector<int> picked;
        for (size_t i = 0; i < want; ++i)
            picked.push_back(group.faces[((2 * i + 1) * count) / (2 * want)]);

        return picked;
    }

    // 動く物の箱(T-1080 の 2 本目): つながりは形の変わらない所で作っておき、刻みごとにつながりの決めた光線(16 本)を箱と解いて
    // 通る割合を掛ける。遮られた分は物が吸う(帳簿 objectAbsorbed。物の面のつながり〔物が放つ〕は作っていない)
    struct MovingBox {
        bool active = false;
        Vec low{};
        Vec high{};
    };

    struct BoxCost {
        uint64_t candidates = 0;    // 包みの検査を通ったつながり
        uint64_t segmentTests = 0;  // 線分と箱の検査
        uint64_t shadowed = 0;      // 通る割合が 1 でないつながり
    };

    // つながりの決めた光線のうち箱を通らない割合(Q16)
    uint32_t LinkPassQ16(const Network& net, const Link& link, const MovingBox& box, BoxCost& cost) {
        const Group& groupA = net.groups[link.groupA];
        const Group& groupB = net.groups[link.groupB];
        // 包み: 中心を結ぶ線から、組の箱の大きい方の半径 + 箱の半径以内
        const Vec ca = groupA.centroid;
        const Vec cb = groupB.centroid;
        const Vec boxCenter = Scale(Add(box.low, box.high), 0.5);
        const Vec ab = Sub(cb, ca);
        const double t = std::clamp(Dot(Sub(boxCenter, ca), ab) / std::max(1e-12, Dot(ab, ab)), 0.0, 1.0);
        const Vec near = Add(ca, Scale(ab, t));
        const auto spread = [](const Group& g) {
            const Vec d = Sub(g.boxMax, g.boxMin);
            return 0.5 * std::sqrt(Dot(d, d)) + 1.0;
        };
        const Vec boxHalf = Scale(Sub(box.high, box.low), 0.5);
        const double reach = std::max(spread(groupA), spread(groupB)) + std::sqrt(Dot(boxHalf, boxHalf));
        if (Dot(Sub(boxCenter, near), Sub(boxCenter, near)) > reach * reach)
            return 65536;

        ++cost.candidates;
        int tried = 0;
        int blocked = 0;
        if (groupA.children.empty() && groupB.children.empty()) {
            const Face& fa = net.faces[groupA.faces[0]];
            const Face& fb = net.faces[groupB.faces[0]];
            for (int i = 0; i < 16; ++i) {
                const Vec pa = PointOnFace(fa, ((i % 2) * 0.5) - 0.25, (((i / 2) % 2) * 0.5) - 0.25);
                const Vec pb = PointOnFace(fb, (((i / 4) % 2) * 0.5) - 0.25, ((i / 8) * 0.5) - 0.25);
                ++tried;
                blocked += SegmentHitsBox(pa, pb, box.low, box.high) ? 1 : 0;
            }
        } else {
            for (const int fa : RepresentativesOf(groupA)) {
                for (const int fb : RepresentativesOf(groupB)) {
                    ++tried;
                    blocked += SegmentHitsBox(PointOnFace(net.faces[fa], 0, 0), PointOnFace(net.faces[fb], 0, 0),
                                              box.low, box.high)
                                   ? 1
                                   : 0;
                }
            }
        }

        cost.segmentTests += static_cast<uint64_t>(tried);
        if (blocked != 0)
            ++cost.shadowed;

        return static_cast<uint32_t>((65536 * (tried - blocked)) / tried);
    }

    TickResult Step(const Scene& scene, const Network& net, const std::vector<uint64_t>& carryIn, bool reverse,
                    const MovingBox& box = {}, BoxCost* boxCost = nullptr) {
        const size_t faceCount = net.faces.size();
        const size_t groupCount = net.groups.size();
        TickResult result;
        result.emittedPerCell.assign(scene.voxels.size(), 0);
        result.absorbedPerCell.assign(scene.voxels.size(), 0);
        result.carry.assign(faceCount, 0);

        // 面の出す量 = 熱 + 前の刻みの照り返し
        std::vector<uint64_t> power(faceCount, 0);
        for (size_t f = 0; f < faceCount; ++f) {
            const uint64_t carried = carryIn.empty() ? 0 : carryIn[f];
            power[f] = net.faces[f].emission + carried;
            result.emitted += net.faces[f].emission;
            result.carriedIn += carried;
            result.emittedPerCell[net.faces[f].cell] += net.faces[f].emission;
        }

        // 組の和と出す量の 1 次のモーメント(組の重心のまわり。葉から根へ)
        std::vector<uint64_t> sum(groupCount, 0);
        std::vector<Vec> moment(groupCount, Vec{0, 0, 0});
        for (const int g : net.groupsByLevel) {
            const Group& group = net.groups[g];
            if (group.children.empty()) {
                sum[g] = power[group.faces[0]];
                continue;
            }

            for (const int child : group.children) {
                sum[g] += sum[child];
                const Vec offset = Sub(net.groups[child].centroid, group.centroid);
                moment[g] = Add(moment[g], Add(moment[child], Scale(offset, static_cast<double>(sum[child]))));
            }
        }

        // つながりごとに行きと帰り(同じ係数 G)。1 次: 出す側は出す量の重心のずれで量を直し、受ける側は勾配を溜める
        std::vector<uint64_t> debit(groupCount, 0);
        std::vector<uint64_t> credit(groupCount, 0);
        std::vector<double> debitAt(groupCount, 0);               // 出す割合(組の重心で)
        std::vector<Vec> debitSlope(groupCount, Vec{0, 0, 0});    // 出す割合の勾配
        std::vector<Vec> creditMoment(groupCount, Vec{0, 0, 0});  // 受けた量 × 勾配
        uint32_t passQ16 = 65536;
        const auto send = [&](int from, int to, uint64_t fractionQ32, Vec slopeFrom, Vec slopeTo) {
            const double fraction = static_cast<double>(fractionQ32) / 4294967296.0;
            double base = static_cast<double>(sum[from]);
            if (net.linear)
                base = std::clamp(base + Dot(slopeFrom, moment[from]), 0.0, 2.0 * static_cast<double>(sum[from]));

            const auto amount = static_cast<uint64_t>(base * fraction);
            const uint64_t passed = (amount * passQ16) >> 16;
            debit[from] += amount;
            credit[to] += passed;
            result.objectAbsorbed += amount - passed;
            debitAt[from] += fraction;
            debitSlope[from] = Add(debitSlope[from], Scale(slopeFrom, fraction));
            creditMoment[to] = Add(creditMoment[to], Scale(slopeTo, static_cast<double>(passed)));
        };
        for (size_t k = 0; k < net.links.size(); ++k) {
            const Link& link = net.links[reverse ? net.links.size() - 1 - k : k];
            const Vec gradientA = Scale(link.gradientB, -1);
            BoxCost unused;
            passQ16 = box.active ? LinkPassQ16(net, link, box, boxCost != nullptr ? *boxCost : unused) : 65536;
            send(link.groupA, link.groupB, link.fractionAbQ32, gradientA, link.gradientB);
            send(link.groupB, link.groupA, link.fractionBaQ32, link.gradientB, gradientA);
        }

        // 根から葉へ分け直す。出した分: 子の重み = 子の出す量 × (割合 + 勾配 · ずれ) + 勾配 · 子のモーメント。
        // 受けた分: 子の重み = 子の面の数 × (1 + 勾配 · ずれ)。どちらも和はちょうど親の量(整数で分ける)
        std::vector<uint64_t> weights;
        std::vector<uint64_t> parts;
        for (auto it = net.groupsByLevel.rbegin(); it != net.groupsByLevel.rend(); ++it) {
            const int g = *it;
            const Group& group = net.groups[g];
            if (group.children.empty())
                continue;

            const double count = static_cast<double>(group.faces.size());
            weights.clear();
            for (const int child : group.children) {
                const Group& kid = net.groups[child];
                const Vec offset = Sub(kid.centroid, group.centroid);
                double w = static_cast<double>(sum[child]);
                if (net.linear && debitAt[g] > 0) {
                    const double at = debitAt[g] + Dot(debitSlope[g], offset);
                    w = std::max(
                        0.0, (static_cast<double>(sum[child]) * at + Dot(debitSlope[g], moment[child])) / debitAt[g]);
                }

                weights.push_back(static_cast<uint64_t>(w * 1024.0));
            }

            uint64_t weightSum = 0;
            for (const uint64_t w : weights)
                weightSum += w;

            SplitByWeights(debit[g], weights, weightSum, parts);
            for (size_t i = 0; i < parts.size(); ++i) {
                const int child = group.children[i];
                debit[child] += parts[i];
                const Vec offset = Sub(net.groups[child].centroid, group.centroid);
                debitAt[child] += debitAt[g] + Dot(debitSlope[g], offset);
                debitSlope[child] = Add(debitSlope[child], debitSlope[g]);
            }

            weights.clear();
            const double perFace = static_cast<double>(credit[g]) / count;
            for (const int child : group.children) {
                const Group& kid = net.groups[child];
                const double faces = static_cast<double>(kid.faces.size());
                double w = faces * perFace;
                if (net.linear)
                    w = std::max(0.0, w + Dot(creditMoment[g], Sub(kid.centroid, group.centroid)) * faces / count);

                weights.push_back(static_cast<uint64_t>(w));
            }

            weightSum = 0;
            for (const uint64_t w : weights)
                weightSum += w;

            if (weightSum == 0) {
                weights.clear();
                for (const int child : group.children)
                    weights.push_back(net.groups[child].faces.size());

                weightSum = group.faces.size();
            }

            SplitByWeights(credit[g], weights, weightSum, parts);
            for (size_t i = 0; i < parts.size(); ++i) {
                const int child = group.children[i];
                credit[child] += parts[i];
                const double faces = static_cast<double>(net.groups[child].faces.size());
                creditMoment[child] = Add(creditMoment[child], Scale(creditMoment[g], faces / count));
            }
        }

        // 面: 空へ・自分へ返す残り・受けた分を吸う / 返す
        std::vector<uint64_t> faceDebit(faceCount, 0);
        std::vector<uint64_t> faceCredit(faceCount, 0);
        for (size_t g = 0; g < groupCount; ++g) {
            if (!net.groups[g].children.empty())
                continue;

            faceDebit[net.groups[g].faces[0]] = debit[g];
            faceCredit[net.groups[g].faces[0]] = credit[g];
        }

        for (size_t f = 0; f < faceCount; ++f) {
            if (faceDebit[f] > power[f]) {
                result.negativeSelf = true;
                continue;
            }

            // 空へ抜ける分は、つながりで出した残りを超えない(割合の丸めで超えうる。保存はこの形でビット単位)
            const uint64_t skyShare = MulShift32(power[f], net.skyQ32[f]);
            const uint64_t sky = std::min(skyShare, power[f] - faceDebit[f]);

            const uint64_t self = power[f] - faceDebit[f] - sky;
            const uint64_t incoming = faceCredit[f] + self;
            const uint64_t reflected = (incoming * net.faces[f].reflectanceQ16) >> 16;
            const uint64_t absorbed = incoming - reflected;
            result.carry[f] = reflected;
            result.absorbedPerCell[net.faces[f].cell] += absorbed;
            result.absorbed += absorbed;
            result.reflected += reflected;
            result.escaped += sky;
            result.selfReturned += self;
        }

        return result;
    }

    uint64_t HashOf(const TickResult& result) {
        uint64_t hash = 1469598103934665603ull;
        const auto mix = [&hash](uint64_t value) {
            hash ^= value;
            hash *= 1099511628211ull;
        };
        for (const uint64_t value : result.absorbedPerCell)
            mix(value);

        for (const uint64_t value : result.carry)
            mix(value);

        return hash;
    }

    bool Conserves(const TickResult& result) {
        return !result.negativeSelf && result.emitted + result.carriedIn ==
                                           result.absorbed + result.reflected + result.escaped + result.objectAbsorbed;
    }

    // --- 場面ごとの測り(RC の cascade_light_test と同じ場面)---

    void PrintNetwork(const char* name, const Network& net) {
        std::printf(
            "  [%s] 面 %zu・組 %zu・つながり %zu(まとめた %llu・面と面 %llu)・形を作る光線 %llu 本(歩いたセル %llu)・"
            "%.0f ms・つながりの和の最大 %.3f・自分へ返す 最大 %.3f 平均 %.4f\n",
            name, net.faces.size(), net.groups.size(), net.links.size(),
            static_cast<unsigned long long>(net.clusterLinks), static_cast<unsigned long long>(net.leafLinks),
            static_cast<unsigned long long>(net.rays.rays), static_cast<unsigned long long>(net.rays.steps),
            net.buildMs, net.worstLinkSum, net.worstSelf, net.meanSelf);
    }

    Network BuildNetwork(const Scene& scene, const Settings& settings) {
        Network net;
        Builder(scene, settings, net).Build();

        return net;
    }

    std::pair<double, double> Deviation(const std::vector<double>& values) {
        double mean = 0;
        for (const double value : values)
            mean += value / static_cast<double>(values.size());

        double worst = 0;
        double squares = 0;
        for (const double value : values) {
            const double deviation = (value / mean) - 1;
            worst = std::max(worst, std::fabs(deviation));
            squares += deviation * deviation / static_cast<double>(values.size());
        }

        return {worst, std::sqrt(squares)};
    }

    // 焚き火から 8 m の円の 24 か所の黒い受け手(RC の MeasureRing と同じ。scaleCells = 1 で 0.5 m、2 で 0.25 m のセル)
    void RecordRing(const Settings& settings, int scaleCells, int radiusCells = 16) {
        const int size = std::max(48, 2 * radiusCells + 16) * scaleCells;
        const int center = size / 2;
        const int radius = radiusCells * scaleCells;
        constexpr int COUNT = 24;
        std::vector<std::array<int, 3>> receivers;
        for (int k = 0; k < COUNT; ++k) {
            const double angle = 2 * std::numbers::pi * k / COUNT;
            receivers.push_back(
                {center + static_cast<int>(std::lround(radius * std::cos(angle) / scaleCells)) * scaleCells, center,
                 center + static_cast<int>(std::lround(radius * std::sin(angle) / scaleCells)) * scaleCells});
        }

        const auto inBlock = [scaleCells](int x, int y, int z, const std::array<int, 3>& at) {
            return x >= at[0] && x < at[0] + scaleCells && y >= at[1] && y < at[1] + scaleCells && z >= at[2] &&
                   z < at[2] + scaleCells;
        };
        Scene scene = MakeScene(size, size, size, [&](int x, int y, int z) {
            if (inBlock(x, y, z, {center, center, center}))
                return Voxel{Material::Stone, FIRE_K};

            const bool receiver = std::ranges::any_of(receivers, [&](const auto& r) { return inBlock(x, y, z, r); });

            return receiver ? Voxel{Material::Black, 0} : Voxel{};
        });
        scene.cellM = CELL_M / scaleCells;
        const Network net = BuildNetwork(scene, settings);
        std::vector<double> scale(COUNT, 0);
        for (int k = 0; k < COUNT; ++k) {
            const double dx = receivers[k][0] - center;
            const double dz = receivers[k][2] - center;
            const double distanceSquared = (dx * dx) + (dz * dz);
            const double projected = (std::fabs(dx) + std::fabs(dz)) / std::sqrt(distanceSquared);
            scale[k] = distanceSquared / (projected * projected);
        }

        // 正しい値: 焚き火の面から受け手の面への遮る物の無い G(細かい積分)× 放つ量。受け手ごと
        std::vector<double> exact(COUNT, 0);
        for (const Face& fire : net.faces) {
            if (fire.emission == 0)
                continue;

            for (const Face& face : net.faces) {
                for (int k = 0; k < COUNT; ++k) {
                    const bool mine = face.x >= receivers[k][0] && face.x < receivers[k][0] + scaleCells &&
                                      face.y >= receivers[k][1] && face.y < receivers[k][1] + scaleCells &&
                                      face.z >= receivers[k][2] && face.z < receivers[k][2] + scaleCells;
                    const Vec d = Sub(face.center, fire.center);
                    if (!mine || Dot(NormalOf(fire.dir), d) <= 0 || Dot(NormalOf(face.dir), d) >= 0)
                        continue;

                    exact[k] += GeometricExchange(fire, face, 6) * static_cast<double>(fire.emission);
                }
            }
        }

        std::vector<uint64_t> carry;
        double instantWorst = 0;
        double instantRms = 0;
        double exactWorst = 0;
        double receivedShare = 0;
        constexpr int TICKS = 60;
        for (int tick = 0; tick < TICKS; ++tick) {
            const TickResult result = Step(scene, net, carry, false);
            EXPECT(Conserves(result));
            std::vector<double> one(COUNT, 0);
            double total = 0;
            for (int k = 0; k < COUNT; ++k) {
                uint64_t received = 0;
                for (int dz = 0; dz < scaleCells; ++dz) {
                    for (int dy = 0; dy < scaleCells; ++dy) {
                        for (int dx = 0; dx < scaleCells; ++dx)
                            received += result.absorbedPerCell[scene.IndexOf(receivers[k][0] + dx, receivers[k][1] + dy,
                                                                             receivers[k][2] + dz)];
                    }
                }

                one[k] = static_cast<double>(received) * scale[k];
                exactWorst = std::max(exactWorst, std::fabs(static_cast<double>(received) / exact[k] - 1));
                total += static_cast<double>(received);
            }

            const auto [worst, rms] = Deviation(one);
            instantWorst = std::max(instantWorst, worst);
            instantRms = std::max(instantRms, rms);
            receivedShare += total / static_cast<double>(result.emitted) / TICKS;
            carry = result.carry;
        }

        std::printf(
            "筋(%s m のセル): %d m の円 瞬間 |ずれ| の最大 %.2f %%・二乗平均 %.2f %%(RC の試験と同じ割り方。60 "
            "刻みのうち最悪の刻み)/ "
            "正しい値(面の積分)からのずれの最大 %.2f %%・受け手 24 か所が受けた割合 %.5f\n",
            scaleCells == 1 ? "0.5" : "0.25", radiusCells / 2, instantWorst * 100, instantRms * 100, exactWorst * 100,
            receivedShare);
        PrintNetwork("8 m の円", net);
        EXPECT(instantWorst < 0.10);
        EXPECT(exactWorst < 0.05);
    }

    // 石の地面の上 2 m に焚き火 1 セル。1 刻み目(持ち越し無し)に地面の上の面が受ける量を、面ごとの正しい値と比べる。
    // まとめたつながりは組の中で面の数の比で分けるので、組の中の 1/r² と cos の変わり方が「ブロックの形の偏り」になる(瞬間の筋の芯)
    void RecordGroundPattern(const Settings& settings) {
        constexpr int SIZE = 48;
        constexpr int CENTER = SIZE / 2;
        const Scene scene = MakeScene(SIZE, SIZE, SIZE, [](int x, int y, int z) {
            if (y < 8)
                return Voxel{Material::Black, 0};

            return x == CENTER && y == 12 && z == CENTER ? Voxel{Material::Stone, FIRE_K} : Voxel{};
        });
        const Network net = BuildNetwork(scene, settings);
        const TickResult result = Step(scene, net, {}, false);
        std::array<double, 3> worst{};
        std::array<double, 3> squares{};
        std::array<int, 3> counts{};
        for (const Face& face : net.faces) {
            if (face.dir != 2 || face.y != 7)
                continue;

            double exact = 0;
            for (const Face& fire : net.faces) {
                const Vec d = Sub(face.center, fire.center);
                if (fire.emission == 0 || Dot(NormalOf(fire.dir), d) <= 0)
                    continue;

                exact += GeometricExchange(fire, face, 4) * static_cast<double>(fire.emission);
            }

            const double dx = face.x + 0.5 - CENTER - 0.5;
            const double dz = face.z + 0.5 - CENTER - 0.5;
            const double radius = std::sqrt((dx * dx) + (dz * dz));
            const int band = radius < 8 ? 0 : (radius < 20 ? 1 : 2);
            const double deviation = static_cast<double>(result.absorbedPerCell[face.cell]) / exact - 1;
            worst[band] = std::max(worst[band], std::fabs(deviation));
            squares[band] += deviation * deviation;
            ++counts[band];
        }

        std::printf(
            "地面の受け方(焚き火の 2 m 下の黒い地面・1 刻み目・正しい値からのずれ): 4 m 以内 最大 %.1f %%・二乗平均 "
            "%.1f %% / 4〜10 m 最大 "
            "%.1f %%・二乗平均 %.1f %% / 10 m より遠く 最大 %.1f %%・二乗平均 %.1f %%\n",
            worst[0] * 100, std::sqrt(squares[0] / counts[0]) * 100, worst[1] * 100,
            std::sqrt(squares[1] / counts[1]) * 100, worst[2] * 100, std::sqrt(squares[2] / counts[2]) * 100);
        PrintNetwork("地面の受け方", net);
    }

    void RecordSlope(const Settings& settings) {
        constexpr int SIZE = 18;
        const Scene scene = MakeScene(SIZE, SIZE, SIZE, [](int x, int y, int z) {
            const bool shell = x < 2 || y < 2 || z < 2 || x >= SIZE - 2 || y >= SIZE - 2 || z >= SIZE - 2;
            const bool block = x >= 7 && x < 10 && y >= 2 && y < 6 && z >= 7 && z < 10;

            return shell || block ? Voxel{Material::Stone, 1000} : Voxel{};
        });
        const Network net = BuildNetwork(scene, settings);
        for (const int ticks : {20, 60, 200}) {
            for (const int skip : {0, 20}) {
                std::vector<double> net0(scene.voxels.size(), 0);
                std::vector<double> emitted(scene.voxels.size(), 0);
                std::vector<uint64_t> carry;
                for (int tick = 0; tick < ticks + skip; ++tick) {
                    const TickResult result = Step(scene, net, carry, false);
                    EXPECT(Conserves(result));
                    carry = result.carry;
                    if (tick < skip)
                        continue;

                    for (size_t cell = 0; cell < scene.voxels.size(); ++cell) {
                        emitted[cell] += static_cast<double>(result.emittedPerCell[cell]);
                        net0[cell] += static_cast<double>(result.absorbedPerCell[cell]) -
                                      static_cast<double>(result.emittedPerCell[cell]);
                    }
                }

                double worstShare = 0;
                double worstMk = 0;
                for (size_t cell = 0; cell < scene.voxels.size(); ++cell) {
                    const auto flat = static_cast<int64_t>(cell);
                    const std::array<int64_t, 3> at = {flat % SIZE, (flat / SIZE) % SIZE, flat / (SIZE * SIZE)};
                    const bool outer = std::ranges::any_of(at, [](int64_t v) { return v == 0 || v == SIZE - 1; });
                    if (emitted[cell] == 0 || outer)
                        continue;

                    worstShare = std::max(worstShare, std::fabs(net0[cell]) / emitted[cell]);
                    worstMk = std::max(worstMk, std::fabs(net0[cell]) / ticks / 65536.0 / 170000.0);
                }

                std::printf(
                    "坂を下る(閉じた箱・1000 K の石): 初めの %d 刻みを除いて %d 刻み: 正味 ÷ 放った 最大 %.3f "
                    "%%・温度に直して最大 %.4f mK/刻み\n",
                    skip, ticks, worstShare * 100, worstMk);
                if (skip > 0)
                    EXPECT(worstMk < 1.0);
            }
        }

        PrintNetwork("閉じた箱", net);
    }

    void TestWallShadow(const Settings& settings) {
        std::array<uint64_t, 2> received{};
        for (int wall = 0; wall < 2; ++wall) {
            const Scene scene = MakeScene(32, 24, 24, [wall](int x, int y, int z) {
                if (x == 8 && y == 12 && z == 12)
                    return Voxel{Material::Stone, FIRE_K};

                if (wall != 0 && x == 14 && y >= 4 && y < 20 && z >= 4 && z < 20)
                    return Voxel{Material::Stone, ROOM_K};

                return x == 20 && y == 12 && z == 12 ? Voxel{Material::Black, 0} : Voxel{};
            });
            const Network net = BuildNetwork(scene, settings);
            std::vector<uint64_t> carry;
            for (int tick = 0; tick < 10; ++tick) {
                const TickResult result = Step(scene, net, carry, false);
                EXPECT(Conserves(result));
                received[wall] += result.absorbedPerCell[scene.IndexOf(20, 12, 12)];
                carry = result.carry;
            }
        }

        std::printf("壁の陰: 受け手 壁なし %llu・壁あり %llu(10 刻み)\n", static_cast<unsigned long long>(received[0]),
                    static_cast<unsigned long long>(received[1]));
        EXPECT(received[0] != 0);
        EXPECT(received[1] * 20 < received[0]);
    }

    double ParallelSquaresView(double side, double distance) {
        const double x = side / distance;
        const double x2 = x * x;
        const double term1 = std::log(std::sqrt((1 + x2) * (1 + x2) / (1 + 2 * x2)));
        const double term2 = 2 * x * std::sqrt(1 + x2) * std::atan(x / std::sqrt(1 + x2));
        const double term3 = 2 * x * std::atan(x);

        return 2 / (std::numbers::pi * x2) * (term1 + term2 - term3);
    }

    void TestParallelPlates(const Settings& settings, int scaleCells) {
        const int side = 16 * scaleCells;
        for (const int baseGap : {1, 4, 8}) {
            const int gap = baseGap * scaleCells;
            const int low = 8 * scaleCells;
            Scene scene = MakeScene(32 * scaleCells, gap + 4, 32 * scaleCells, [=](int x, int y, int z) {
                const bool inside = x >= low && x < low + side && z >= low && z < low + side;
                if (y == 0)
                    return Voxel{Material::Black, 0};

                if (y == 1)
                    return Voxel{Material::Black, inside ? 1000.0 : 0.0};

                return y == 2 + gap && inside ? Voxel{Material::Black, 0} : Voxel{};
            });
            scene.cellM = CELL_M / scaleCells;
            const Network net = BuildNetwork(scene, settings);
            const TickResult result = Step(scene, net, {}, false);
            EXPECT(Conserves(result));
            double received = 0;
            for (int z = low; z < low + side; ++z) {
                for (int x = low; x < low + side; ++x)
                    received += static_cast<double>(result.absorbedPerCell[scene.IndexOf(x, 2 + gap, z)]);
            }

            const double expected = ParallelSquaresView(side, gap);
            const double measured = received / static_cast<double>(result.emitted);
            std::printf(
                "平行な黒い板(%s m のセル)%d × %d・隙間 %d セル: F = %.4f(解析解 %.4f・%+.2f %%)・つながり %zu\n",
                scaleCells == 1 ? "0.5" : "0.25", side, side, gap, measured, expected, (measured / expected - 1) * 100,
                net.links.size());
            EXPECT(std::fabs(measured / expected - 1) < 0.10);
        }
    }

    void TestDeterministic(const Settings& settings) {
        constexpr int SIZE = 24;
        const Scene scene = MakeScene(SIZE, SIZE, SIZE, [](int x, int y, int z) {
            if (y < 4)
                return Voxel{Material::Stone, ROOM_K};

            if (x == 12 && z == 12 && y == 4)
                return Voxel{Material::Stone, FIRE_K};

            return x == 18 && y < 10 ? Voxel{Material::Stone, ROOM_K} : Voxel{};
        });
        const Network net = BuildNetwork(scene, settings);
        std::vector<uint64_t> carryA;
        std::vector<uint64_t> carryB;
        bool same = true;
        for (int tick = 0; tick < 5; ++tick) {
            const TickResult a = Step(scene, net, carryA, false);
            const TickResult b = Step(scene, net, carryB, true);
            same = same && HashOf(a) == HashOf(b) && Conserves(a) && Conserves(b);
            carryA = a.carry;
            carryB = b.carry;
        }

        std::printf("決定的: つながりを逆の順に回しても同じ %s・保存 ビット単位\n", same ? "はい" : "いいえ");
        EXPECT(same);
    }

    // 費用の場面: RC と同じ「石の地面 + 熱い床」と、照り返す面が向き合う重い場面(段のある地形・閉じた部屋)
    Scene FireGround(int size, int hot) {
        const int center = size / 2;
        return MakeScene(size, size, size, [=](int x, int y, int z) {
            if (y < 8)
                return Voxel{Material::Stone, ROOM_K};

            const int low = center - (hot / 2);
            const bool fire = y == 8 && x >= low && x < low + hot && z >= low && z < low + hot;

            return fire ? Voxel{Material::Stone, FIRE_K} : Voxel{};
        });
    }

    // 段のある地形(高さ 4〜15 の決まった凸凹)+ 柱 + 真ん中の焚き火 hot × hot
    Scene Terrain(int size, int hot) {
        const int center = size / 2;
        const auto height = [](int x, int z) {
            const uint32_t h = (static_cast<uint32_t>(x / 4) * 73856093u) ^ (static_cast<uint32_t>(z / 4) * 19349663u);
            return 4 + static_cast<int>((h >> 7) % 12);
        };
        return MakeScene(size, size, size, [=](int x, int y, int z) {
            const int ground = height(x, z);
            const int low = center - (hot / 2);
            const bool fireColumn = x >= low && x < low + hot && z >= low && z < low + hot;
            if (y < ground)
                return Voxel{Material::Stone, ROOM_K};

            if (fireColumn && y == ground)
                return Voxel{Material::Stone, FIRE_K};

            const bool pillar = (x % 16) == 3 && (z % 16) == 11 && y < ground + 10;

            return pillar ? Voxel{Material::Stone, ROOM_K} : Voxel{};
        });
    }

    // 閉じた石の部屋(内のり size − 2)の床の真ん中に焚き火
    Scene ClosedRoom(int size, int hot) {
        const int center = size / 2;
        return MakeScene(size, size, size, [=](int x, int y, int z) {
            const bool shell = x < 1 || y < 1 || z < 1 || x >= size - 1 || y >= size - 1 || z >= size - 1;
            if (shell)
                return Voxel{Material::Stone, ROOM_K};

            const int low = center - (hot / 2);
            const bool fire = y == 1 && x >= low && x < low + hot && z >= low && z < low + hot;

            return fire ? Voxel{Material::Stone, FIRE_K} : Voxel{};
        });
    }

    // 決めた対の見えるかを決めた光線(試作の Builder と同じ点)のどれかがセルを通るか = 形が変わると決め直す対
    bool RecordCrosses(const Network& net, const std::array<int, 2>& record, const std::array<int, 3>& cell) {
        const Node& na = net.nodes[record[0]];
        const Node& nb = net.nodes[record[1]];
        for (const int ga : na.groups) {
            if (ga < 0)
                continue;

            for (const int gb : nb.groups) {
                if (gb < 0)
                    continue;

                const Group& groupA = net.groups[ga];
                const Group& groupB = net.groups[gb];
                if (na.level == 0 && nb.level == 0) {
                    const Face& fa = net.faces[groupA.faces[0]];
                    const Face& fb = net.faces[groupB.faces[0]];
                    for (int i = 0; i < 16; ++i) {
                        const Vec pa = PointOnFace(fa, ((i % 2) * 0.5) - 0.25, (((i / 2) % 2) * 0.5) - 0.25);
                        const Vec pb = PointOnFace(fb, (((i / 4) % 2) * 0.5) - 0.25, ((i / 8) * 0.5) - 0.25);
                        if (SegmentHitsCell(pa, pb, cell))
                            return true;
                    }

                    continue;
                }

                for (const int fa : RepresentativesOf(groupA)) {
                    for (const int fb : RepresentativesOf(groupB)) {
                        if (SegmentHitsCell(PointOnFace(net.faces[fa], 0, 0), PointOnFace(net.faces[fb], 0, 0), cell))
                            return true;
                    }
                }
            }
        }

        return false;
    }

    // 形が 1 セル変わった時に、決めた光線がそのセルを通る対だけを決め直す + 新しく出た面のつながりを作る(T-1080 の 1 本目)
    void RecordChangeExact(const char* name, Scene scene, std::array<int, 3> cell, const Settings& settings,
                           const Network& before, Material to) {
        const bool added = to != Material::Air;
        scene.voxels[scene.IndexOf(cell[0], cell[1], cell[2])] = Voxel{to, to == Material::Flame ? FIRE_K : ROOM_K};
        std::vector<std::array<std::array<int, 4>, 2>> pairs;
        // 炎は光線を通すので、遮りは変わらない(新しい要素だけ)
        if (to != Material::Flame) {
            for (const auto& record : before.records) {
                if (!RecordCrosses(before, record, cell))
                    continue;

                const Node& a = before.nodes[record[0]];
                const Node& b = before.nodes[record[1]];
                pairs.push_back({std::array<int, 4>{a.level, a.block[0], a.block[1], a.block[2]},
                                 std::array<int, 4>{b.level, b.block[0], b.block[1], b.block[2]}});
            }
        }

        std::vector<std::array<int, 3>> cells;
        if (added)
            cells.push_back(cell);
        else {
            for (int dir = 0; dir < 6; ++dir) {
                const Vec n = NormalOf(dir);
                const std::array<int, 3> next = {cell[0] + static_cast<int>(n[0]), cell[1] + static_cast<int>(n[1]),
                                                 cell[2] + static_cast<int>(n[2])};
                if (scene.Opaque(next[0], next[1], next[2]))
                    cells.push_back(next);
            }
        }

        Network net;
        Builder(scene, settings, net).Rebuild(pairs, cells);
        std::printf(
            "  形の変化(決めた光線で探す)%s (%d, %d, %d) を %s: 決め直す対 %zu(決めた対 %zu)・新しい要素のセル "
            "%zu・光線 %llu 本(うち決め直した対 %llu。歩いたセル %llu)\n",
            name, cell[0], cell[1], cell[2], to == Material::Flame ? "炎に" : (added ? "石に" : "空気に"), pairs.size(),
            before.records.size(), cells.size(), static_cast<unsigned long long>(net.rays.rays),
            static_cast<unsigned long long>(net.pairRays), static_cast<unsigned long long>(net.rays.steps));
    }

    // 煙の柱(箱 low〜high)を、つながりごとの粗い道筋(4³ セルのブロック)で通しやすさを掛ける時の 1 刻みの手間の見込み(T-1080)
    void RecordSmokeCost(const char* name, const Network& net, std::array<int, 3> low, std::array<int, 3> high) {
        constexpr int BLOCK = 4;
        uint64_t crossing = 0;
        uint64_t blockSteps = 0;
        for (const Link& link : net.links) {
            const Vec p0 = net.groups[link.groupA].centroid;
            const Vec p1 = net.groups[link.groupB].centroid;
            // 道筋をブロックの細かさで歩く(線分の長さ ÷ ブロック × 2 点を数える目安)
            const double length = std::sqrt(Dot(Sub(p1, p0), Sub(p1, p0)));
            const int samples = std::max(2, static_cast<int>(std::ceil(2 * length / BLOCK)));
            bool smoky = false;
            for (int i = 0; i <= samples && !smoky; ++i) {
                const Vec p = Add(p0, Scale(Sub(p1, p0), static_cast<double>(i) / samples));
                smoky = p[0] >= low[0] && p[0] < high[0] && p[1] >= low[1] && p[1] < high[1] && p[2] >= low[2] &&
                        p[2] < high[2];
            }

            if (!smoky)
                continue;

            ++crossing;
            blockSteps += static_cast<uint64_t>(std::ceil(length / BLOCK)) + 1;
        }

        std::printf("  煙の柱 %s: 道筋が煙を通るつながり %llu(全体 %zu)・ブロックを歩く手間 %llu/刻み\n", name,
                    static_cast<unsigned long long>(crossing), net.links.size(),
                    static_cast<unsigned long long>(blockSteps));
    }

    // 形が 1 セル変わった時に作り直す対の費用(光線)。変わったセルを通りうる対だけを作り直す見込み
    void RecordChange(const char* name, Scene scene, std::array<int, 3> cell, const Settings& settings,
                      const Network& before) {
        Voxel& voxel = scene.voxels[scene.IndexOf(cell[0], cell[1], cell[2])];
        voxel = voxel.material == Material::Air ? Voxel{Material::Stone, ROOM_K} : Voxel{};
        Settings partial = settings;
        partial.rebuildOnly = true;
        partial.changedCell = cell;
        Network net;
        Builder(scene, partial, net).Build();
        // 空の割合を取り直す面 = 変わったセルが見える面(面の真ん中からセルの真ん中への光線で数える)
        RayCounter counter;
        size_t seeing = 0;
        const Vec target{cell[0] + 0.5, cell[1] + 0.5, cell[2] + 0.5};
        for (const Face& face : before.faces) {
            const Vec d = Sub(target, face.center);
            if (Dot(NormalOf(face.dir), d) <= 0)
                continue;

            const Vec stop = Sub(target, Scale(d, 0.6 / std::sqrt(Dot(d, d))));
            seeing += Visible(scene, PointOnFace(face, 0, 0), stop, counter) ? 1 : 0;
        }

        std::printf(
            "  形の変化 %s (%d, %d, %d) を %s: 作り直す対の光線 %llu 本(歩いたセル %llu)・作り直したつながり %zu(全体 "
            "%zu)・"
            "空の割合を取り直す面 %zu(× %d 本)\n",
            name, cell[0], cell[1], cell[2], voxel.material == Material::Air ? "空気に" : "石に",
            static_cast<unsigned long long>(net.rays.rays), static_cast<unsigned long long>(net.rays.steps),
            net.links.size(), before.links.size(), seeing, settings.skyRays);
    }

    void RecordCost(const char* name, const Scene& scene, const Settings& settings,
                    std::array<int, 3> changed = {-1, -1, -1}) {
        const Network net = BuildNetwork(scene, settings);
        std::vector<uint64_t> carry = Step(scene, net, {}, false).carry;
        constexpr int TICKS = 5;
        const auto start = std::chrono::steady_clock::now();
        bool conserves = true;
        for (int tick = 0; tick < TICKS; ++tick) {
            const TickResult result = Step(scene, net, carry, false);
            conserves = conserves && Conserves(result);
            carry = result.carry;
        }

        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
                          TICKS;
        size_t carried = 0;
        for (const uint64_t value : carry)
            carried += value != 0 ? 1 : 0;

        std::printf("費用: %s: 刻み %.2f ms(CPU 1 スレッド)・持ち越す面 %zu・保存 %s\n", name, ms, carried,
                    conserves ? "ビット単位" : "崩れた");
        PrintNetwork(name, net);
        EXPECT(conserves);
        if (changed[0] >= 0) {
            RecordChange(name, scene, changed, settings, net);
            const bool air = scene.voxels[scene.IndexOf(changed[0], changed[1], changed[2])].material == Material::Air;
            RecordChangeExact(name, scene, changed, settings, net, air ? Material::Stone : Material::Air);
            // 地面の真ん中の 1 セルを掘る(石 → 空気)
            for (int y = scene.sy - 1; y >= 0; --y) {
                if (!scene.Opaque(scene.sx / 2 + 5, y, scene.sz / 2))
                    continue;

                RecordChangeExact(name, scene, {scene.sx / 2 + 5, y, scene.sz / 2}, settings, net, Material::Air);
                break;
            }
        }
    }

}  // namespace

int main(int argc, char** argv) {
    Settings settings;
    if (argc > 1)
        settings.theta = std::atof(argv[1]);

    if (argc > 3)
        settings.slopeLimit = std::atof(argv[3]);

    const char mode = argc > 2 ? argv[2][0] : 'n';
    const bool heavy = mode == 'h';
    // 炎と煙(T-1080): 段のある地形の焚き火の上に炎 2 × 2 × 3・その上に煙の柱 4 × 4 × 16。
    // 炎のセルが 1 つ増える時の新しい要素の光線・1 セルの形の変化・動く箱(2³ が 1 セル動く = 8 セルが変わる)の見込み
    if (mode == 'f') {
        const int size = argc > 4 ? std::atoi(argv[4]) : 48;
        Scene scene = Terrain(size, 4);
        const int center = size / 2;
        int ground = 0;
        while (scene.Opaque(center, ground, center))
            ++ground;

        for (int y = ground; y < ground + 3; ++y) {
            for (int z = center - 1; z < center + 1; ++z) {
                for (int x = center - 1; x < center + 1; ++x)
                    scene.voxels[scene.IndexOf(x, y, z)] = Voxel{Material::Flame, FIRE_K};
            }
        }

        const Network net = BuildNetwork(scene, settings);
        PrintNetwork("地形 + 炎", net);
        RecordChangeExact("炎が 1 セル増える", scene, {center + 1, ground + 1, center}, settings, net, Material::Flame);
        RecordChangeExact("炎の横に石を置く", scene, {center + 3, ground + 1, center}, settings, net, Material::Stone);
        RecordChangeExact("空中に石を置く", scene, {center + 6, ground + 4, center + 2}, settings, net,
                          Material::Stone);
        RecordSmokeCost("4 × 4 × 16", net, {center - 2, ground + 3, center - 2}, {center + 2, ground + 19, center + 2});
        return failureCount == 0 ? 0 : 1;
    }

    // 動く物の箱(T-1080 の 2 本目): 箱を石として作り直したつながり(正しい)と、箱の無いつながり + 刻みごとの箱の検査を比べる
    if (mode == 'm') {
        const auto shape = [](bool withBox) {
            return [withBox](int x, int y, int z) {
                if (x == 8 && y == 12 && z == 12)
                    return Voxel{Material::Stone, FIRE_K};

                if (withBox && x >= 13 && x < 16 && y >= 10 && y < 14 && z >= 10 && z < 14)
                    return Voxel{Material::Black, 0};

                return x == 22 && y >= 4 && y < 20 && z >= 4 && z < 20 ? Voxel{Material::Black, 0} : Voxel{};
            };
        };
        const Scene withBox = MakeScene(32, 24, 24, shape(true));
        const Scene without = MakeScene(32, 24, 24, shape(false));
        const Network exactNet = BuildNetwork(withBox, settings);
        const Network openNet = BuildNetwork(without, settings);
        const MovingBox box{.active = true, .low = {13, 10, 10}, .high = {16, 14, 14}};
        BoxCost cost;
        const TickResult exact = Step(withBox, exactNet, {}, false);
        const TickResult moving = Step(without, openNet, {}, false, box, &cost);
        const TickResult open = Step(without, openNet, {}, false);
        double exactSum = 0;
        double movingSum = 0;
        double peak = 0;
        double worst = 0;
        int exactDark = 0;
        int movingDark = 0;
        for (int z = 4; z < 20; ++z) {
            for (int y = 4; y < 20; ++y) {
                const size_t at = without.IndexOf(22, y, z);
                const auto e = static_cast<double>(exact.absorbedPerCell[at]);
                const auto m = static_cast<double>(moving.absorbedPerCell[at]);
                const auto o = static_cast<double>(open.absorbedPerCell[at]);
                exactSum += e;
                movingSum += m;
                peak = std::max(peak, o);
                worst = std::max(worst, std::fabs(m - e));
                exactDark += e < 0.05 * o ? 1 : 0;
                movingDark += m < 0.05 * o ? 1 : 0;
            }
        }

        std::printf(
            "動く箱 3 × 4 × 4(焚き火と受け手の板の間): 板が受けた和 刻みの検査 / 作り直し = %.3f・セルごとの差の最大 "
            "%.1f %%(箱の無い時の最大に対して)・"
            "陰(5 %% 未満)のセル 作り直し %d / 刻みの検査 %d・保存 %s・つながり %zu のうち包みを通った "
            "%llu・線分と箱の検査 %llu・陰のつながり %llu\n",
            movingSum / exactSum, worst / peak * 100, exactDark, movingDark,
            Conserves(moving) ? "ビット単位" : "崩れた", openNet.links.size(),
            static_cast<unsigned long long>(cost.candidates), static_cast<unsigned long long>(cost.segmentTests),
            static_cast<unsigned long long>(cost.shadowed));
        EXPECT(Conserves(moving));

        // 費用: 段のある地形の焚き火の近くに 2³ の箱
        const int size = argc > 4 ? std::atoi(argv[4]) : 32;
        const Scene terrain = Terrain(size, 4);
        const Network net = BuildNetwork(terrain, settings);
        const int center = size / 2;
        int ground = 0;
        while (terrain.Opaque(center + 4, ground, center))
            ++ground;

        BoxCost terrainCost;
        const MovingBox cart{.active = true,
                             .low = {center + 4.0, ground + 0.0, center + 0.0},
                             .high = {center + 6.0, ground + 2.0, center + 2.0}};
        const auto start = std::chrono::steady_clock::now();
        const TickResult tick = Step(terrain, net, {}, false, cart, &terrainCost);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf(
            "  %d³ の地形で 2³ の箱: つながり %zu のうち包みを通った %llu・線分と箱の検査 %llu/刻み・陰のつながり "
            "%llu・刻み %.1f ms(CPU)・保存 %s\n",
            size, net.links.size(), static_cast<unsigned long long>(terrainCost.candidates),
            static_cast<unsigned long long>(terrainCost.segmentTests),
            static_cast<unsigned long long>(terrainCost.shadowed), ms, Conserves(tick) ? "ビット単位" : "崩れた");
        return failureCount == 0 ? 0 : 1;
    }

    // 重い場面を 1 つずつ(argv[4] = 番号)
    if (mode == 'c') {
        const int which = argc > 4 ? std::atoi(argv[4]) : 0;
        if (which == 0)
            RecordCost("48³ 段のある地形 + 柱・熱いセル 16", Terrain(48, 4), settings, {30, 18, 20});
        else if (which == 1)
            RecordCost("32³ 閉じた石の部屋・熱いセル 16", ClosedRoom(32, 4), settings, {16, 10, 16});
        else if (which == 2)
            RecordCost("64³ 石の地面・熱いセル 256", FireGround(64, 16), settings, {40, 9, 32});
        else if (which == 3)
            RecordCost("64³ 段のある地形 + 柱・熱いセル 16", Terrain(64, 4), settings, {40, 20, 30});
        else
            RecordCost("64³ 閉じた石の部屋・熱いセル 16", ClosedRoom(64, 4), settings, {32, 20, 32});

        return failureCount == 0 ? 0 : 1;
    }

    if (mode == 'g') {
        settings.linear = false;
        RecordGroundPattern(settings);
        settings.linear = true;
        RecordGroundPattern(settings);
        return 0;
    }

    if (mode == 'r') {
        RecordGroundPattern(settings);
        RecordRing(settings, 1);
        RecordRing(settings, 1, 64);
        RecordSlope(settings);
        TestParallelPlates(settings, 1);
        return failureCount == 0 ? 0 : 1;
    }

    std::printf("面どうしの対のつながり(T-1062 の試作): theta %.2f・勾配の上限 %.2f・空の向き %d\n", settings.theta,
                settings.slopeLimit, settings.skyRays);
    TestDeterministic(settings);
    TestWallShadow(settings);
    TestParallelPlates(settings, 1);
    TestParallelPlates(settings, 2);
    RecordRing(settings, 1);
    RecordRing(settings, 2);
    RecordRing(settings, 1, 64);
    RecordGroundPattern(settings);
    RecordSlope(settings);
    for (const int hot : {1, 16, 48})
        RecordCost(hot == 1 ? "48³ 石の地面・熱いセル 1"
                            : (hot == 16 ? "48³ 石の地面・熱いセル 256" : "48³ 石の地面・熱いセル 2304"),
                   FireGround(48, hot), settings);

    RecordCost("48³ 段のある地形 + 柱・熱いセル 16", Terrain(48, 4), settings, {30, 18, 20});
    RecordCost("32³ 閉じた石の部屋・熱いセル 16", ClosedRoom(32, 4), settings, {16, 10, 16});
    if (heavy) {
        RecordCost("64³ 石の地面・熱いセル 256", FireGround(64, 16), settings, {40, 9, 32});
        RecordCost("64³ 段のある地形 + 柱・熱いセル 16", Terrain(64, 4), settings, {40, 20, 30});
        RecordCost("64³ 閉じた石の部屋・熱いセル 16", ClosedRoom(64, 4), settings, {32, 20, 32});
    }

    if (failureCount != 0) {
        std::printf("失敗 %d 件\n", failureCount);
        return 1;
    }

    std::printf("通過\n");
    return 0;
}
