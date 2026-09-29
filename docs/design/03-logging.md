# 03 ログ(CPU 側)

決定は ADR-0006。コードは `engine/src/core/`。

## 部品
| ファイル | 役目 |
|---|---|
| `log.h` / `log.cpp` | `Channel`・`Level`・`Logger`(`Singleton<Logger>`)・`Log()`・1 行への整形。Windows を知らない |
| `log_sinks.h` / `log_sinks.cpp` | 出力先 3 種(コンソール・OutputDebugString・ファイル)と `OpenLogFile()`・`DefaultLogDirectory()` |
| `hresult.h` / `hresult.cpp` | `BICAMERAL_CHECK_HR`・`CheckHresult()`・`DescribeHresult()` |
| `unicode.h` / `unicode.cpp` | UTF-8 ⇔ UTF-16。エンジンの中の文字列は UTF-8、Windows の W 系 API の境界でだけ変換 |

## 流れ
```
Log(Channel, Level, "書式 {}", 値)  ─┐
BICAMERAL_CHECK_HR(Channel, 式)      ─┤→ Singleton<Logger>::Write ─(鍵の中で)→ 全シンク
                                          ├ コンソール   [   0.004] I platform  | 本文         (Warning 以上は "  (caps.cpp:88)")
                                          ├ デバッガ     Warning 以上は "フルパス(88): warning platform | 本文"(VS でダブルクリック)
                                          └ ファイル     exe の横の logs/bicameral-日時.log(常に場所つき)
```
- 呼んだ場所は `std::source_location` で自動で付く(`FormatWithLocation` が書式文字列と一緒に受け取る)。
- `MinLevel` より軽いログは整形もしない。既定は Debug ビルドで debug、Release で info。`--log-level` で変える。
- Warning 以上を書いたら全シンクを Flush する(直後に落ちても残る)。

## チャンネルを足すとき
`log.h` の `Channel` と `log.cpp` の `CHANNEL_NAMES` の両方に足す(順番を合わせる)。

## まだやっていないこと
- GPU からのログ(T-0003 のリングバッファを読み戻して `Log()` に流す)。
- スレッド ID の表示・非同期書き込み(スレッドや毎フレームのログが増えたら)。
