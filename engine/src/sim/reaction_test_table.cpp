// reaction_test_table.cpp — 試験用の反応表の中身(reaction_test_table.h)。
// 生成エンタルピー(J/mol、298.15 K)と比熱(mJ/(mol·K))は NIST の値を丸めた。セルロースは C6H10O5 の 1 単位あたり
// (燃焼熱から逆算した約 −963 kJ/mol)、比熱は約 1.3 J/(g·K)。この値だと熱分解が発熱になる(実際は生成物が複雑でほぼ中立)。
// 速度: 熱分解は Broido–Shafizadeh の値の桁、ほかは 600〜1000 K で燃え始めるように置いた仮の値。
// 熱伝導率(T-0089): 現実の値(木 0.12・炭 0.1・気体 0.017〜0.027 W/(m·K))に倍率を掛けた試験の値。
// 現実の値だと木の中を 0.5 m 伝わるのに数日かかり、空気の伝導はほぼ 0(現実の火は放射と対流で広がる。M3)。
// 「リアルっぽいが、反応が遅すぎない」ように大きくする(ユーザー 2026-10-01)。M3 で放射と対流が入ったら見直す。
// 仮の魔素 mana_test(T-0225・D-449・D-451): 気体 1 種と、魔素を触媒にする反応 2 本。エディタで値を動かして試すための
// 試験用の仮の値で、ゲームの中身ではない(D-006)。本当の魔素は非公開側で決める(data/packages/combustion_test と同じ中身)。
// 倍率は gpu_probe_fire_test で決めた(1 回のクリックで、木箱の壁の 9 割が約 50 秒で炭になる):
//   固体 × 20000・気体 × 5000。固体を大きくしすぎる(× 80000)と、熱が燃える前に薄まって火が消える。
//   倍率を全部同じにすると、燃えたセルの熱が空気へ逃げて広がらない(気体 × 5 万: 火が 1 セルで消える)
#include "sim/reaction_test_table.h"

namespace bicameral::sim {

    namespace {

        ElementCount Atom(std::string_view element, uint32_t count) {
            return {.element = std::string(element), .count = count};
        }

        // 反応物(速度式で次数 1)
        RuleTerm Reactant(std::string_view species, uint32_t coefficient) {
            return {.species = std::string(species), .coefficient = coefficient, .firstOrder = true};
        }

        // 速度式で次数 0 の反応物(触媒の魔素。あれば効き、濃さに依らない)
        RuleTerm Catalyst(std::string_view species) {
            return {.species = std::string(species), .coefficient = 1, .firstOrder = false};
        }

        RuleTerm Product(std::string_view species, uint32_t coefficient) {
            return {.species = std::string(species), .coefficient = coefficient, .firstOrder = false};
        }

        // 熱伝導率の試験の倍率と、現実の値(mW/(m·K))から試験の値にする
        constexpr uint32_t TEST_SOLID_CONDUCTIVITY_SCALE = 20000;
        constexpr uint32_t TEST_GAS_CONDUCTIVITY_SCALE = 5000;

        constexpr uint32_t SolidConductivity(uint32_t realMilliwattsPerMeterKelvin) {
            return realMilliwattsPerMeterKelvin * TEST_SOLID_CONDUCTIVITY_SCALE;
        }

        constexpr uint32_t GasConductivity(uint32_t realMilliwattsPerMeterKelvin) {
            return realMilliwattsPerMeterKelvin * TEST_GAS_CONDUCTIVITY_SCALE;
        }

        std::vector<ElementDefinition> Elements() {
            return {
                {.name = "C", .atomicMass = 12011},
                {.name = "H", .atomicMass = 1008},
                {.name = "O", .atomicMass = 15999},
                {.name = "N", .atomicMass = 14007},
                // 仮の魔素の元素(T-0225。試験用の仮の値)
                {.name = "mana_test", .atomicMass = 10000},
            };
        }

        std::vector<SpeciesDefinition> Species() {
            return {
                {.name = "cellulose",
                 .composition = {Atom("C", 6), Atom("H", 10), Atom("O", 5)},
                 .formationEnthalpy = -963000,
                 .heatCapacity = 211000,
                 .thermalConductivity = SolidConductivity(120)},
                {.name = "carbon",
                 .composition = {Atom("C", 1)},
                 .formationEnthalpy = 0,
                 .heatCapacity = 8520,
                 .thermalConductivity = SolidConductivity(100)},
                {.name = "oxygen",
                 .composition = {Atom("O", 2)},
                 .formationEnthalpy = 0,
                 .heatCapacity = 29378,
                 .thermalConductivity = GasConductivity(27)},
                {.name = "nitrogen",
                 .composition = {Atom("N", 2)},
                 .formationEnthalpy = 0,
                 .heatCapacity = 29124,
                 .thermalConductivity = GasConductivity(26)},
                {.name = "carbon_dioxide",
                 .composition = {Atom("C", 1), Atom("O", 2)},
                 .formationEnthalpy = -393509,
                 .heatCapacity = 37135,
                 .thermalConductivity = GasConductivity(17)},
                {.name = "carbon_monoxide",
                 .composition = {Atom("C", 1), Atom("O", 1)},
                 .formationEnthalpy = -110527,
                 .heatCapacity = 29142,
                 .thermalConductivity = GasConductivity(25)},
                {.name = "water_vapor",
                 .composition = {Atom("H", 2), Atom("O", 1)},
                 .formationEnthalpy = -241826,
                 .heatCapacity = 33580,
                 .thermalConductivity = GasConductivity(25)},
                // 仮の魔素(気体。T-0225。試験用の仮の値: 元素の単体・単原子の理想気体の比熱 5/2 R・気体並みの熱伝導率)
                {.name = "mana_test",
                 .composition = {Atom("mana_test", 1)},
                 .formationEnthalpy = 0,
                 .heatCapacity = 20786,
                 .thermalConductivity = GasConductivity(20)},
            };
        }

        std::vector<RuleDefinition> Rules() {
            return {
                // 酸素なしで炭と水蒸気になる(一次反応)
                {.name = "cellulose_pyrolysis",
                 .reactants = {Reactant("cellulose", 1)},
                 .products = {Product("carbon", 6), Product("water_vapor", 5)},
                 .rate = {.preExponentialMantissa = 28, .preExponentialExponent10 = 18, .activationEnergy = 242400}},
                // 木が燃える(O2 を炭の燃焼と取り合う)
                {.name = "cellulose_combustion",
                 .reactants = {Reactant("cellulose", 1), Reactant("oxygen", 6)},
                 .products = {Product("carbon_dioxide", 6), Product("water_vapor", 5)},
                 .rate = {.preExponentialMantissa = 2, .preExponentialExponent10 = 10, .activationEnergy = 150000}},
                {.name = "carbon_combustion",
                 .reactants = {Reactant("carbon", 1), Reactant("oxygen", 1)},
                 .products = {Product("carbon_dioxide", 1)},
                 .rate = {.preExponentialMantissa = 3, .preExponentialExponent10 = 8, .activationEnergy = 160000}},
                // 熱いと CO が増える
                {.name = "carbon_partial_combustion",
                 .reactants = {Reactant("carbon", 2), Reactant("oxygen", 1)},
                 .products = {Product("carbon_monoxide", 2)},
                 .rate = {.preExponentialMantissa = 15, .preExponentialExponent10 = 8, .activationEnergy = 200000}},
                // 吸熱(熱を資源として取り合う場面)
                {.name = "boudouard",
                 .reactants = {Reactant("carbon", 1), Reactant("carbon_dioxide", 1)},
                 .products = {Product("carbon_monoxide", 2)},
                 .rate = {.preExponentialMantissa = 1, .preExponentialExponent10 = 10, .activationEnergy = 250000}},
                // --- 仮の魔素の反応(T-0225。試験用の仮の値。魔素は両辺に 1 ずつの触媒)---
                // 魔素があると木が低い温度で燃える(cellulose_combustion の活性化エネルギーを下げたもの)
                {.name = "mana_test_catalyzed_cellulose_combustion",
                 .reactants = {Reactant("cellulose", 1), Reactant("oxygen", 6), Catalyst("mana_test")},
                 .products = {Product("carbon_dioxide", 6), Product("water_vapor", 5), Product("mana_test", 1)},
                 .rate = {.preExponentialMantissa = 2, .preExponentialExponent10 = 10, .activationEnergy = 100000}},
                // 魔素があると吸熱の boudouard が低い温度で進む
                {.name = "mana_test_catalyzed_boudouard",
                 .reactants = {Reactant("carbon", 1), Reactant("carbon_dioxide", 1), Catalyst("mana_test")},
                 .products = {Product("carbon_monoxide", 2), Product("mana_test", 1)},
                 .rate = {.preExponentialMantissa = 1, .preExponentialExponent10 = 10, .activationEnergy = 170000}},
            };
        }

    }  // namespace

    ReactionTableDefinition MakeCombustionTestTable() {
        return {.elements = Elements(), .species = Species(), .rules = Rules()};
    }

}  // namespace bicameral::sim
