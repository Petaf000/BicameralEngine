-- init.luau — 2 つ目の点が 1 つ目より低い(ベイクで止まる)
return {
    environment = {
        atmosphere_surface_pressure_pa = 100000,
        atmosphere_profile = {
            { height_mm = 1000, temperature_mk = 290000, composition = { nitrogen = 1000000 } },
            { height_mm = 0, temperature_mk = 300000, composition = { nitrogen = 1000000 } },
        },
    },
}
