# T-0724 区画をやめる: gas_regions・liquid_regions を読まない・場面を直す(D-510)

- Status: 作業中(2026-10-18 wt2 で止めた。読み込みの境目・場面・型の定義・試験は済み。残り = 下の「続き」)
- 種類: 工学
- PC: 必須
- マイルストーン: ③' 運ぶ計算の共通の土台 段 4 ②(ROADMAP)
- 設計: docs/design/20-transport-substrate.md・07 §2.7 / 決定: D-510・D-513・D-514・ADR-0342(Proposed)

## 目的
区画の箱を読まない。気体は基準の大気があれば・液体は重力があれば世界のどこでも流れる。仮の世界は「世界いっぱいの区画 1 つ」で残す(D-513 で退かせる)。
場面・パッケージ・型の定義から区画の書き方を外し、残っていたら注意を出す。

## 完了条件
- [x] world_layout_package が gas_regions・liquid_regions を読まず、書いてあれば注意(表の警告)
- [x] BakeWorldLayout が流れる所を表の基準の大気と重力から作る(世界いっぱい)
- [x] 場面 grass_fire_river・型の定義から区画を外す
- [x] world_layout・standard_packages・reaction_package・environment_package・gpu_play_metric・window_play_*・window_regions_play_* が通る
- [ ] window_regions_copy_record(20000 フレーム)が 600 秒に当たる: 仮の世界の世界いっぱいの刻みが 85 ms(予算の 5.6 倍)→ 判断待ち(ADR-0342)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-18(wt2・作業役): 読み込みの境目だけを変えた(world_layout_package は前の分類を読まず注意・BakeWorldLayout が表の基準の大気と重力から
  世界いっぱいの流れる所を作る・仮の世界の作りはそのまま)。場面 grass_fire_river の flow_regions.luau と型の定義から区画を外した。
  ⑤ の場面は仮の世界の GPU の刻みが約 85 ms(予算の 5.6 倍)になり、写しの窓の試験が 600 秒に当たった → 判断待ち(ADR-0342)。

## 続き(次の作業役へ)
- ADR-0342 の判断待ち((a) このまま・(b) 仮の世界だけ流す切り替えを残す・(c) ⑤ の場面を先に多重解像度へ)を司令塔・ユーザーに聞く。
- (a) なら tests/CMakeLists.txt の window_regions_copy_record の --frames 20000 を減らすか上限を上げ、window_regions_copy_* を通す。
- --auto-play の置く場所(気体の区画の真ん中 = 今は世界の真ん中)を場面に合う決め方に(frame_loop.cpp の MakeAutoPlayStrokes)。
- docs/design/20・07 の区画の記述の整理・ROADMAP の T-0724 の行(本体)。
