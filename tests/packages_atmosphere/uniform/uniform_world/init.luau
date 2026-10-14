-- init.luau — 一様な大気(T-0373 の書き方。高さ 0 の点 1 つとして読む)
return {
    environment = {
        atmosphere_temperature_mk = 300000,
        atmosphere_surface_pressure_pa = 100000,
    },
    atmosphere = {
        nitrogen = 790000,
        oxygen = 210000,
    },
}
