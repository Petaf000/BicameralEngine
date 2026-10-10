-- elements.luau — 元素(原子量、mg/mol)。IUPAC の標準原子量の慣用値(C 12.011・H 1.008・O 15.999・N 14.007)。
return {
    C = { atomic_mass_mg_per_mol = 12011 },
    H = { atomic_mass_mg_per_mol = 1008 },
    N = { atomic_mass_mg_per_mol = 14007 },
    O = { atomic_mass_mg_per_mol = 15999 },
    -- 触るための仮の魔素の元素(T-0225・D-449・D-451)。名前も原子量も試験用の仮の値で、ゲームの中身ではない(D-006)。
    -- 本当の魔素の値は非公開側で決める。原子量は g で量を入れた時に扱いやすい桁にしただけ
    mana_test = { atomic_mass_mg_per_mol = 10000 },
}
