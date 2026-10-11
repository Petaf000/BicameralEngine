# T-0177 仮の世界で反応の上限の印を数える(T-0163 から分けた)

- Status: Done(2026-10-11)
- 種類: 工学(小)
- 設計: 02 §3。関係: T-0163(多重解像度の世界の刻みは数える器 MR_COUNTER_LIMIT_* で数え済み)・T-0022

## やること
仮の世界(common/probe_world.hlsli の ProbeStepCell。GPU の probe_conduct.hlsl と CPU の ProbeReference)でも、反応の上限に当たった印
(RxWaitStep::limits)を (セル, 刻み) の数で数え、CPU と GPU で同じ数になることを gpu_probe_sim(_warp)で確かめる。
GPU の数える場所は刻みのハッシュの表か WgStats のゲージ(どちらか軽い方)。仮の世界の表(燃える木箱)では 0 のまま(T-0163 の測定で 1 セル最大 6 種・進む規則は最大 4)。
仮の世界を多重解像度の世界に置き換える予定なら、このチケットはやらない(BACKLOG と照らす)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(作業役・本体): ProbeCellStep に limits(RxWaitStep::limits)を足し、GPU の ConductBlock が刻みのハッシュの表の空き語
  [12] 待たせた・[13] 選んだ(PROBE_HASH_OFFSET_LIMIT_*。適用の単位が 0 にしてある語)にウェーブで数えて 0 でない時だけ atomic で足す(CountLimits)。
  CPU リファレンス(ProbeReference::Limits)は GPU が計算するブロック(m_scheduled)のセルだけ数える(眠っているブロックは GPU が評価しない)。
  ProbeTickHash::limits(ProbeLimitCounts)で読み戻し、gpu_probe_sim・gpu_probe_place の比べ方に足した。gpu_probe_sim に TestLimits
  (上限の試験の表のセルを中央の 8³ に置き、残りは規則の無い q09)を足した: 40 刻みで待たせた 10240・選んだ 10240(セル, 刻み)が HW・WARP で毎刻み CPU と同じ。
