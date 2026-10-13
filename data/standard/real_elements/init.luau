-- init.luau — 現実の元素(原子量、mg/mol)。IUPAC の標準原子量の慣用値(C 12.011・H 1.008・N 14.007・O 15.999・Ar 39.95)。
-- ⑤ の場面(延焼・氷・木の橋)と地球の大気に要る分だけ(D-471 ②)。足りない元素は場面のチケットで足す。
return {
    elements = {
        C = { atomic_mass_mg_per_mol = 12011 },
        H = { atomic_mass_mg_per_mol = 1008 },
        N = { atomic_mass_mg_per_mol = 14007 },
        O = { atomic_mass_mg_per_mol = 15999 },
        Ar = { atomic_mass_mg_per_mol = 39950 },
    },
}
