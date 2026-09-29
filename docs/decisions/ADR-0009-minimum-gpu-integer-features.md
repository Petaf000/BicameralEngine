# ADR-0009 最低機の条件に 64bit 整数演算と 64bit atomic を入れる

- Status: Accepted(2026-09-30、ユーザーが案 A を選んだ。D-211)
- 日付: 2026-09-30
- 決めた人: ユーザー(Claude の提案から案 A)

## 背景
- シミュは整数で計算し(ADR-0008)、数学ライブラリ(shaders/common/fixed.hlsli)は 64bit の整数を前提にしている(04 §2・§3)。
  保存量の足し合わせは 64bit の atomic を使う(04 R4)。
- D3D12 では、シェーダーの 64bit 整数演算(Int64ShaderOps)と、64bit atomic のうち typed リソース・groupshared・
  ヒープの記述子経由のものは**任意の機能**。raw / structured バッファへの 64bit atomic は SM 6.6 以上で必須。
- 最低機は「Work Graphs 対応・VRAM 8GB(RTX 3060 / RX 7600 級)」(D-210)。SM 6.8 と Work Graphs はすでに必須。
- T-0013(2026-09-30)で、開発機(RTX 3070 Ti)と WARP はどれも対応していることを確かめた(docs/perf.md)。
  RX 7600 などの AMD の対応は**未確認**(D-207 で機械を用意した時に `--caps` で確かめる)。

## 決定(案 A)
- **必須**: Int64ShaderOps と、raw / structured バッファへの 64bit atomic。起動時(デバイスを作る所)に確かめ、無ければ理由を出して終える。
  確かめる所: engine/src/gpu/device.cpp の CreateDevice(Int64ShaderOps。raw バッファの 64bit atomic は SM 6.6 以上で必須なので SM 6.8 の確認に含まれる)。
- **使わない**: typed リソース・groupshared・ヒープの記述子経由の 64bit atomic。使いたくなったら、その時に対応状況を調べて決め直す。

## 検討した代案と、採らなかった理由
- 案 B: 64bit atomic を全部必須にする(groupshared の 64bit atomic で、グループ内で足してから 1 回だけ全体へ足せる)。
  → 速くできる所はあるが、対応していない機種を理由なく外しうる。groupshared は 32bit を 2 つ使う足し算でも書ける。
- 案 C: 64bit を 32bit の組で模擬する(Int64ShaderOps が無い機種でも動く)。
  → 命令数が増え、fixed.hlsli が複雑になる。Work Graphs 対応の機種で Int64ShaderOps が無いものは見つかっていない(ただし未確認)。

## 影響(何がしやすく/しにくくなるか)
- fixed.hlsli と保存量の足し合わせを今のまま使える。起動時の確認で、対応していない機種では分かりやすく止まる。
- groupshared の 64bit atomic を使う最適化は、使う時に改めて決める必要がある。
