-- atmosphere.luau — 基準の大気の高さの点(environment.atmosphere_profile)を書く補助(T-0499・D-494・ADR-0182 の改訂)。
-- 使い方(ゲーム本体。earth_environment に依存する):
--   local atmosphere = require("@earth_environment/atmosphere")
--   local earth = require("@earth_environment")                       -- 地球の表の写し(書き換えても元は変わらない)
--   local profile = atmosphere.set_point(earth.environment.atmosphere_profile, { height_mm = 0, temperature_mk = 300000, composition = ... })
--   return { environment = { atmosphere_profile = profile } }        -- ゲーム本体は依存の鍵を書き直してよい(T-0498)
-- 計算は足し算・引き算・掛け算・割り算と math.floor だけ(どの機械でも同じ値。exp は下の級数で作る)。値はエンジンが読む前の
-- 下書きなので、出した点はそのまま表の中身(と表の版)に入る。
local M = {}

local GAS_CONSTANT = 8.314462618 -- J/(mol·K)(2019 年の SI の定義値 = アボガドロ定数 × ボルツマン定数)

-- 表の鍵を名前の順に(表を回す順は決まっていないので、足し算の順をそろえる)
local function sorted_names(entries: { [string]: any }): { string }
    local names: { string } = {}
    for name in entries do
        table.insert(names, name)
    end
    table.sort(names)

    return names
end

-- 点の列の写しに point を入れる: 同じ高さの点があれば置き換え、無ければ高さの順の場所に足す(元の列は変えない)
function M.set_point(points: { AtmospherePoint }, point: AtmospherePoint): { AtmospherePoint }
    local result: { AtmospherePoint } = {}
    local placed = false
    for _, existing in points do
        if not placed and existing.height_mm >= point.height_mm then
            table.insert(result, point)
            placed = true
            if existing.height_mm == point.height_mm then
                continue
            end
        end
        table.insert(result, existing)
    end

    if not placed then
        table.insert(result, point)
    end

    return result
end

-- 物質のモル質量(mg/mol)= Σ 元素の数 × 原子量。species・elements はパッケージの表の形(require("@real_matter").species など)
function M.molar_masses(species: { [string]: any }, elements: { [string]: any }): { [string]: number }
    local masses: { [string]: number } = {}
    for name, entry in species do
        local mass = 0
        for _, element in sorted_names(entry.composition) do
            mass += entry.composition[element] * elements[element].atomic_mass_mg_per_mol
        end
        masses[name] = mass
    end

    return masses
end

-- e^x(足し算と掛け算だけ: 2 で割って |x| ≤ 0.5 にし、級数 20 項のあと 2 乗で戻す)
local function exponential(x: number): number
    local halvings = 0
    while x < -0.5 or x > 0.5 do
        x /= 2
        halvings += 1
    end

    local term, sum = 1, 1
    for i = 1, 20 do
        term = term * x / i
        sum += term
    end

    for _ = 1, halvings do
        sum *= sum
    end

    return sum
end

-- 点の列の温度(K)を高さ(mm)で直線に読む(端の外は端の値)
local function temperature_at(temperatures: { AtmospherePoint }, height: number): number
    local first, last = temperatures[1], temperatures[#temperatures]
    if height <= first.height_mm then
        return first.temperature_mk / 1000
    end
    if height >= last.height_mm then
        return last.temperature_mk / 1000
    end

    for i = 2, #temperatures do
        local high = temperatures[i]
        if height < high.height_mm then
            local low = temperatures[i - 1]
            local t = (height - low.height_mm) / (high.height_mm - low.height_mm)
            return (low.temperature_mk + (high.temperature_mk - low.temperature_mk) * t) / 1000
        end
    end

    return last.temperature_mk / 1000
end

-- 割合(足して 1 の重み)を ppm の整数にする: 切り捨てて、端数を余りの大きい順(同じなら名前の順)に 1 ppm ずつ配る。0 は載せない
local function to_parts_per_million(weights: { [string]: number }): { [string]: number }
    local names = sorted_names(weights)
    local total = 0
    for _, name in names do
        total += weights[name]
    end

    local parts: { [string]: number } = {}
    local remainders: { [string]: number } = {}
    local sum = 0
    for _, name in names do
        local exact = weights[name] / total * 1000000
        parts[name] = math.floor(exact)
        remainders[name] = exact - parts[name]
        sum += parts[name]
    end

    table.sort(names, function(a, b)
        if remainders[a] ~= remainders[b] then
            return remainders[a] > remainders[b]
        end
        return a < b
    end)
    local i = 1
    while sum < 1000000 do
        parts[names[i]] += 1
        sum += 1
        i += 1
    end

    local result: { [string]: number } = {}
    for name, value in parts do
        if value > 0 then
            result[name] = value
        end
    end

    return result
end

-- 「重い物ほど下に溜まる」大気の点を作る(拡散の釣り合い: 物質ごとに自分のモル質量の重さで高さとともに減る。
--   x_i(z) ∝ x_i(地面) × exp(−M_i g ∫ dz ÷ (R T))。風で混ざらない大気の形。地球の約 86 km より下はよく混ざっているので、地球の値には使わない)。
-- args:
--   gravity_um_per_s2: 重力(µm/s²)
--   ground: 一番低い高さの成分(物質 → ppm)
--   temperatures: 温度の高さの分布(AtmospherePoint の列。composition は読まない。earth の atmosphere_profile をそのまま渡せる)
--   heights_mm: 点を作る高さ(低い順。最初が ground の高さ)。この範囲の temperatures の点の高さも足す(温度の折れ目を保つ)
--   molar_masses: 物質 → mg/mol(molar_masses で作れる)
--   step_mm: 積分の刻み(省けば 10000 = 10 m)
-- 返す: AtmospherePoint の列(温度は temperatures を直線で読んだ値の四捨五入)
function M.settled(args: {
    gravity_um_per_s2: number,
    ground: { [string]: number },
    temperatures: { AtmospherePoint },
    heights_mm: { number },
    molar_masses: { [string]: number },
    step_mm: number?,
}): { AtmospherePoint }
    -- --- 点を作る高さ = heights_mm と、その範囲の温度の折れ目 ---
    local bottom, top = args.heights_mm[1], args.heights_mm[#args.heights_mm]
    local seen: { [number]: boolean } = {}
    local heights: { number } = {}
    for _, height in args.heights_mm do
        if not seen[height] then
            seen[height] = true
            table.insert(heights, height)
        end
    end
    for _, point in args.temperatures do
        local height = point.height_mm
        if height > bottom and height < top and not seen[height] then
            seen[height] = true
            table.insert(heights, height)
        end
    end
    table.sort(heights)

    -- --- 下から ∫ dz ÷ T(m/K)を中点で積み、各高さで成分を出す ---
    local gravity = args.gravity_um_per_s2 / 1000000
    local step = args.step_mm or 10000
    local integral = 0
    local z = bottom
    local points: { AtmospherePoint } = {}
    for _, height in heights do
        while z < height do
            local dz = math.min(step, height - z)
            integral += (dz / 1000) / temperature_at(args.temperatures, z + dz / 2)
            z += dz
        end

        local weights: { [string]: number } = {}
        for _, name in sorted_names(args.ground) do
            local parts = args.ground[name]
            local mass = args.molar_masses[name] / 1000000 -- kg/mol
            weights[name] = parts * exponential(-mass * gravity * integral / GAS_CONSTANT)
        end

        table.insert(points, {
            height_mm = height,
            temperature_mk = math.floor(temperature_at(args.temperatures, height) * 1000 + 0.5),
            composition = to_parts_per_million(weights),
        })
    end

    return points
end

return M
