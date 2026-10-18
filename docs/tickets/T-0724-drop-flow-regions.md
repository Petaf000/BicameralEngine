# T-0724 区画をやめる: gas_regions・liquid_regions を読まない・場面を直す(D-510)

- Status: Review(2026-10-19 wt2。D-521 = ADR-0342 Accepted。ホットリロードの 4 本が落ちる = 判断待ち。下の「続き」)
- 種類: 工学
- PC: 必須
- マイルストーン: ③' 運ぶ計算の共通の土台 段 4 ②(ROADMAP)
- 設計: docs/design/20-transport-substrate.md・07 §2.7 / 決定: D-510・D-513・D-514・D-521・ADR-0342(Accepted)

## 目的
区画の箱を読まない。気体は基準の大気があれば・液体は重力があれば世界のどこでも流れる。仮の世界は「世界いっぱいの区画 1 つ」で残す(D-513 で退かせる)。
場面・パッケージ・型の定義から区画の書き方を外し、残っていたら注意を出す。

## 完了条件
- [x] world_layout_package が gas_regions・liquid_regions を読まず、書いてあれば注意(表の警告)
- [x] BakeWorldLayout が流れる所を表の基準の大気と重力から作る(世界いっぱい)
- [x] 場面 grass_fire_river・型の定義から区画を外す
- [x] world_layout・standard_packages・reaction_package・environment_package・gpu_play_metric・window_play_*・window_regions_play_* が通る
- [x] 過渡期の遅さ(D-521: 案 c を急ぎ、それまで案 a): 写しの窓の試験 window_copy_*・window_regions_copy_*・window_metrics_*(指標も写し)は刻みを減らしても通らない
  (本物の 1 刻み約 85 ms がフレームの予算全体を超え、余りの予算でだけ進む写しが 1 刻みも進まない)ので DISABLED にした(理由と戻す条件は ADR-0342・tests/CMakeLists.txt)
- [x] 画像の基準 probe_volume・probe_slice を置き換えた(熱い気体が上がる・世界いっぱいが活性。意図した変化)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-18(wt2・作業役): 読み込みの境目だけを変えた(world_layout_package は前の分類を読まず注意・BakeWorldLayout が表の基準の大気と重力から
  世界いっぱいの流れる所を作る・仮の世界の作りはそのまま)。場面 grass_fire_river の flow_regions.luau と型の定義から区画を外した。
  ⑤ の場面は仮の世界の GPU の刻みが約 85 ms(予算の 5.6 倍)になり、写しの窓の試験が 600 秒に当たった → 判断待ち(ADR-0342)。
- 2026-10-19(wt2・作業役): D-521(案 c を急ぎ、それまで案 a)で ADR-0342 を Accepted に。写しの窓の試験はサンプルの世界(window_copy_record)も
  600 秒に当たった(T-0724 の前は流れが無く軽かった)。写しは余りの予算でだけ進む(D-468)ので刻みを減らしても通らない → 3 組を DISABLED。
  画像の基準 2 枚を置き換え(熱い気体が上がる・世界いっぱいが活性)。window_* の残りは通った。ただしホットリロードの 4 本が落ちる(T-0670 が未。判断待ち)。

## 続き(司令塔・後のチケットへ)
- **判断待ち(マージの前)**: 表の差し替え(物質の一覧・重力・基準の大気が変わる表)のホットリロードが、地球の環境に依る世界で断られる(区画の作り直し T-0670 が未)。
  window_hot_reload_record・window_hot_reload_game_record・window_gravity_reload_record・window_play_table_record が落ちる(止めていない)。案とおすすめは ADR-0342。
- 写しの窓の試験を戻す: ⑤ の場面は多重解像度へ移した後(T-0725〜T-0727)・サンプルの世界は仮の世界を退かせる時(D-513)。
- --auto-play の置く場所(気体の区画の真ん中 = 今は世界の真ん中)を場面に合う決め方に(frame_loop.cpp の MakeAutoPlayStrokes)。
- docs/design/20・07 の区画の記述の整理・ROADMAP の T-0724 の行(本体)。
