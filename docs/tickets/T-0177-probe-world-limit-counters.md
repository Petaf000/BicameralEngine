# T-0177 仮の世界で反応の上限の印を数える(T-0163 から分けた)

- Status: Todo
- 種類: 工学(小)
- 設計: 02 §3。関係: T-0163(多重解像度の世界の刻みは数える器 MR_COUNTER_LIMIT_* で数え済み)・T-0022

## やること
仮の世界(common/probe_world.hlsli の ProbeStepCell。GPU の probe_conduct.hlsl と CPU の ProbeReference)でも、反応の上限に当たった印
(RxWaitStep::limits)を (セル, 刻み) の数で数え、CPU と GPU で同じ数になることを gpu_probe_sim(_warp)で確かめる。
GPU の数える場所は刻みのハッシュの表か WgStats のゲージ(どちらか軽い方)。仮の世界の表(燃える木箱)では 0 のまま(T-0163 の測定で 1 セル最大 6 種・進む規則は最大 4)。
仮の世界を多重解像度の世界に置き換える予定なら、このチケットはやらない(BACKLOG と照らす)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
