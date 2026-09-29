# T-0003 GPU デバッグ基盤

- Status: Done(2026-09-30)
- 種類: 工学
- PC: 必須
- マイルストーン: M1(2026-09-30 T-0009 で範囲を見直し。設計は docs/design/16-debug-test.md §1)
- 見積もり: チャット 2〜3 回分

## 目的
GPU 常駐ロジックは「症状から原因に辿る手段」が無いと進まない。化学を書き始める前に道具を作る。

## 完了条件
- [x] シェーダーから書ける printf/assert のリングバッファ(フレームごとに読み戻して CPU のログ(ADR-0006)へ)。
      **Work Graphs のノードからも書ける形にする**(T-0008 がノード名・世代・レコード ID つきで使う)
- [x] D3D12 debug layer / GPU-based validation / DRED(デバイス除去時の情報)を debug プリセットで有効化

- [x] 引数は整数だけ(シミュのシェーダーに浮動小数点を持ち込まない。04 R1)

移したもの: `--frames`/`--screenshot` → T-0017、再生の骨組み → T-0012、CPU との突き合わせ → T-0005

## 作業ログ
- 2026-09-30: リング(shaders/common/debug_ring.hlsli + debug_formats.hlsli、engine/src/gpu/debug_ring.*)を作った。書式の番号 + 整数の引数 6 個まで・`__LINE__`、
  u0 space1、4096 件 / フレーム、溢れは数える。compute と Work Graphs のノード(Leaf)から書けることを GPU・WARP で確認。FX_ASSERT もつないだ(Debug のシミュのシェーダーだけ有効)。
- `gpu::CreateDevice` を `gpu::Device::Create(kind, DeviceOptions)` に替えた。debug プリセットで debug layer・GBV・DRED。報告は ID3D12InfoQueue1 でログへ流して数え、
  GPU のテストは 0 件を確かめる。デバイス喪失は `LogDeviceRemoved`(ImmediateQueue が呼ぶ)。ルート署名は `CreateRootSignature(RootSignatureLayout)` に。
- テスト 19/19(debug / release)。GBV + Work Graphs は NVIDIA・WARP ともエラー 0。DRED の「止まったコマンド」表示は本物のハングで未確認(16 §4)。
