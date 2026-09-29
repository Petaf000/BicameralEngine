# T-0001 環境確認と caps プローブ

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分

## 目的
この PC で DX12 Ultimate + SM6.8 + Work Graphs が使えることを、エンジン自身の出力で確定させる。
以後のすべての作業の土台なので、ここで環境の不明点をなくす。

## 完了条件
- [ ] `job.py check` が OK(ランナー疎通・鍵 ID 一致)
- [ ] `job.py env` の結果(GPU・ドライバ・VS・cmake)を docs/perf.md の「環境」に記録
- [ ] `job.py build` が通る(debug)
- [ ] `job.py test` が通る(smoke)
- [ ] `job.py run -- --caps` で SM / DXR / Mesh Shader / WorkGraphsTier が表示される
- [ ] Agility SDK(Work Graphs 対応版)を導入し、WorkGraphsTier が 1.0 になる
      - D3D12SDKVersion / D3D12SDKPath のエクスポートと、D3D12Core.dll を bin\D3D12\ に置く配線
      - vcpkg のポート(directx12-agility など)か NuGet か、どちらで入れるかは実際に確かめてから決め、ADR に残す
      - DXC(SM6.8 に対応する版)も同時に入れる
- [ ] VS2026 で「フォルダーを開く」→ プリセット選択 → F5 で --caps が動くことをユーザーが確認
- [ ] HANDOFF / NEXT 更新・コミット

## メモ・参考
- 骨格はまだ一度もビルドしていない。最初のエラーは vcpkg の場所(VCPKG_ROOT)か directx-headers の見つけ方の可能性が高い。
- Agility SDK は「最新版」を仮定しない。使う版をその時点で調べて決める。

## 作業ログ
