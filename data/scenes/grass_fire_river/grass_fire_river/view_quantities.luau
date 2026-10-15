-- view_quantities.luau — 表示の「色分けする量」(C キー)の枠 1〜3。量は 1 セルあたりの µmol。
-- 減り(depletion)は全部のセルで「幅 − 量」を塗るので、草の無いセル(空気・地面・川)まで塗られる。草は残りの量で見せる(作り心地の記録 6)。
return {
    slot_1 = { label = "草(残り)", species = "cellulose", span_micromoles = 1200000 },
    slot_2 = { label = "炭", species = "carbon", span_micromoles = 7200000 },
    slot_3 = { label = "水", species = "water", span_micromoles = 6917000000 },
}
