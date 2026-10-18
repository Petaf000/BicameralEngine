-- init.luau — 世界の端の上だけ open(外は earth_environment の基準の大気。横と下は書かないので wall)
return {
    environment = {
        world_edges = { top = "open" },
    },
}
