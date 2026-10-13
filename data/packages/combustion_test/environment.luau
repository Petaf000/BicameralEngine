-- environment.luau — サンプルの世界の環境の値(分類 environment。T-0400・ADR-0176)。地球の値。
-- 1 つの鍵 = 1 つの値(整数・単位は鍵の名前)。書かない値は 0 = 無い(ブランクの世界と同じ)。読み手は engine/src/script/environment_package.cpp。
-- 標準パッケージ(地球の環境)に分けるのは T-0373(環境ミキサー)。
return {
    gravity_um_per_s2 = 9806650, -- 重力 9.80665 m/s²(標準の重力の定義値)
    sound_speed_mm_per_s = 340000, -- 気体の音速の基準 340 m/s(D-440: 本物の音速。地面の近くの空気)
}
