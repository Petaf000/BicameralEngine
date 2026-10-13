# T-0452 環境ミキサーのパネル(標準パッケージをボタンで世界に足す・外す)

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分より小さい(T-0373 と同じチャット)
- マイルストーン: D-471 ② 中身の器(docs/plan/ROADMAP.md)
- 設計: docs/design/14-editor-tools.md §2「環境ミキサー」/ 決定: D-457・ADR-0183・ADR-0047・ADR-0055
- 分けた元: T-0373

## 目的(1〜2 行)
エディタに「環境ミキサー」のパネルを足し、標準パッケージ(data/standard)をボタンで今の世界に足す・外す。依存は一緒に足す・外す。
足した表はホットリロードと同じ道で刻みの境界に当たり、記録・再生に残る。

## 完了条件(チェックできる形で)
- [x] editor/mixer_panel(棚の一覧・足す/外すボタン・見せる名前・依存と一緒に外れるもののツールチップ・結果の表示・再生中は押せない)
- [x] frame::TableHotReload::ReplaceSource(読む元を替えてすぐ読み直す・以後そのファイルも見る)とフレームのループへのつなぎ
- [x] 窓の自動の確かめ --auto-mixer(ブランクの世界に全部足す → 当たる → 元素を外す → 初めの表)と記録 → 再生(window_mixer_record/play)
- [x] --mix で起動(window_mix_start)・window_blank_*・--world sample の窓の試験が変わらない

## メモ・参考
- 足した環境の値(重力)と初めのセル(earth_air)は、動いている世界には効かない(表の法則だけ替わる)。世界を作り直すのは T-0453。
- 足した一覧は起動の引数 --mix とエディタの間だけ(世界のファイルへの保存はセーブの形 15)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-13(wt2・並走。T-0373 と同じチャット): editor/mixer_panel(棚の一覧・足す/外す・依存のツールチップ・再生中は押せない)、
  TableHotReload::ReplaceSource(読む元を替えてすぐ読み直す。ファイルを見ない世界でも読み直しの道を持つように m_watchFiles を分けた)、
  --auto-mixer。release で window_mixer_record/play・window_mix_start・window_blank_*・window_hot_reload_*・window_table_edit が通過。
  ブランクの世界(物質 0・規則 0)→ 全部足した表 → 全部外した表(初めの版)の差し替えが GPU で当たり、再生でもハッシュ列が一致した。
