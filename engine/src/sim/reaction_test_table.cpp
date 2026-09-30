// reaction_test_table.cpp — 試験用の反応表の中身(reaction_test_table.h)。
// 生成エンタルピー(J/mol、298.15 K)と比熱(mJ/(mol·K))は NIST の値を丸めた。セルロースは C6H10O5 の 1 単位あたり
// (燃焼熱から逆算した約 −963 kJ/mol)、比熱は約 1.3 J/(g·K)。この値だと熱分解が発熱になる(実際は生成物が複雑でほぼ中立)。
// 速度: 熱分解は Broido–Shafizadeh の値の桁、ほかは 600〜1000 K で燃え始めるように置いた仮の値。
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

        RuleTerm Product(std::string_view species, uint32_t coefficient) {
            return {.species = std::string(species), .coefficient = coefficient, .firstOrder = false};
        }

        std::vector<ElementDefinition> Elements() {
            return {
                {.name = "C", .atomicMass = 12011},
                {.name = "H", .atomicMass = 1008},
                {.name = "O", .atomicMass = 15999},
                {.name = "N", .atomicMass = 14007},
            };
        }

        std::vector<SpeciesDefinition> Species() {
            return {
                {.name = "cellulose",
                 .composition = {Atom("C", 6), Atom("H", 10), Atom("O", 5)},
                 .formationEnthalpy = -963000,
                 .heatCapacity = 211000},
                {.name = "carbon", .composition = {Atom("C", 1)}, .formationEnthalpy = 0, .heatCapacity = 8520},
                {.name = "oxygen", .composition = {Atom("O", 2)}, .formationEnthalpy = 0, .heatCapacity = 29378},
                {.name = "nitrogen", .composition = {Atom("N", 2)}, .formationEnthalpy = 0, .heatCapacity = 29124},
                {.name = "carbon_dioxide",
                 .composition = {Atom("C", 1), Atom("O", 2)},
                 .formationEnthalpy = -393509,
                 .heatCapacity = 37135},
                {.name = "carbon_monoxide",
                 .composition = {Atom("C", 1), Atom("O", 1)},
                 .formationEnthalpy = -110527,
                 .heatCapacity = 29142},
                {.name = "water_vapor",
                 .composition = {Atom("H", 2), Atom("O", 1)},
                 .formationEnthalpy = -241826,
                 .heatCapacity = 33580},
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
            };
        }

    }  // namespace

    ReactionTableDefinition MakeCombustionTestTable() {
        return {.elements = Elements(), .species = Species(), .rules = Rules()};
    }

}  // namespace bicameral::sim
