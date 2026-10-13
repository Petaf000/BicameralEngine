-- world_bodies.luau — サンプルの世界の物(仮の世界の積み木。T-0098・T-0398。前は C++ の sim::MakeProbeStackScene に書いていた)。
-- 試験の値で、ゲームの中身ではない(D-006)。物理の座標(y が上・格子の角が原点)。長さは mm、質量は g(0 = 動かない物)。
-- 物は order の小さい順(同じなら鍵の順)に番号が付く。物理の結果は番号の順に依る(地面が 0 番、積み木が下から 1〜10 番)。
local bodies: { [string]: WorldBody } = {
    -- 地面: 100 m 四方・厚さ 1 m、上の面が y = 0。格子の床を覆う
    ground = {
        size_mm = { 100000, 1000, 100000 },
        position_mm = { 16000, -500, 16000 },
        mass_g = 0,
    },
}

-- 1 m の立方体(各 500 kg)10 段の柱。x = 10 m・z = 16.25 m(格子の左寄り。既定の断面 z = 32 のセルの真ん中を通る)
for level = 0, 9 do
    bodies["block_" .. tostring(level)] = {
        order = 1,
        size_mm = { 1000, 1000, 1000 },
        position_mm = { 10000, level * 1000 + 500, 16250 },
        mass_g = 500000,
    }
end

return bodies
