# コーディング規約(C++ / HLSL)

2026-09-29 にユーザーが決めた流儀(T-0006)。見た目は `.clang-format`、名前は `.clang-tidy` が機械的に確かめる。
ここに無いことが出てきたら、ユーザーに聞いてから足す。

## 見た目(clang-format が揃える)
- インデント 4、`{` は同じ行、1 行の上限 120 桁(日本語コメントは 1 文字 2 桁で数えられる)。
- `public:` / `private:` はクラスの行頭に揃える。名前空間の中もインデントする(入れ子の名前空間も 1 段ずつ)。
- 1 行の if は `{ }` を省いて同じ行に書く: `if (device == nullptr) return false;`
  2 行以上になる if・else 付き・ループは `{ }` を付ける。
- 日本語のコメントは自動で折り返さない(自分で切れ目を選んで改行する)。

## 名前
| 種類 | 形 | 例 |
|---|---|---|
| 型(class / struct / enum / using) | 大文字始まり | `ReactionGraph`, `NodeState` |
| 関数・メソッド | 大文字始まり(UpperCamel) | `Dispatch()`, `RunCapsProbe()` |
| class のメンバー変数 | `m_` + 小文字始まり | `m_maxDepth` |
| static メンバー変数 | `s_` + 小文字始まり | `s_instance` |
| struct のメンバー | 小文字始まり(接頭辞なし) | `temperature`, `nodeCount` |
| 関数の中の変数・引数 | 小文字始まり | `hogeHuga`, `commandList` |
| 定数(constexpr・static const・グローバルの const) | 全部大文字のスネークケース | `MAX_FINALIZERS`, `MAX_DEPTH` |
| enum の値 | 大文字始まり(UpperCamel) | `NotSupported`, `Tier1_0` |
| マクロ(#define) | 全部大文字のスネークケース | `BICAMERAL_GPU_VALIDATION` |
| 名前空間 | 小文字 | `bicameral`, `bicameral::reaction` |

- 定数・マクロの全部大文字の名前は windows.h のマクロと衝突しうる(`MAX_PATH`・`ERROR`・`DELETE` など)。
  衝突したら、その名前だけ別の語にする(`MAX_PATH` → `MAX_PATH_LENGTH` など)。enum の値は UpperCamel なので衝突しない。
- 略語を避ける。単位が要る値は名前に単位を付ける(`timeoutMs`, `sizeBytes`)。

## 書き方
- **C++23**(MSVC では `/std:c++latest`)。簡潔に書ける所は新しい機能を使う:
  `auto`・範囲 for・構造化束縛・`std::span`・`std::string_view`・`std::format`・`std::expected`(失敗しうる関数の戻り値)・
  指示付き初期化子(`D3D12_... desc{ .Width = w }`)・`enum class`・`[[nodiscard]]`。
  ただし読みにくくなるなら使わない(テンプレートの技巧・長いラムダの連鎖は避ける)。
- **ネストを深くしない。** 失敗や対象外は先に return / continue する(早期リターン)。ネストは 3 段まで。
- 1 関数は 1 つの仕事、目安 50 行以内。長くなったら名前の付いた関数に分ける。
- ファイルの先頭に「このファイルは何をするか・データがどこから来てどこへ行くか」を数行。コメントは「何を」より「なぜ」。
- 処理のまとまりごとに空行と区切りコメント(`// --- 〇〇 ---`)。

## 分け方・つなぎ方
- 機能ごとに `engine/src/<subsystem>/` に分ける。サブシステムどうしは小さなインターフェース(ヘッダの関数・構造体)だけで
  つなぎ、中身(実装の型・グローバル)を他から直接触らせない。依存は引数で渡す。
- グローバルな状態・シングルトンは使わない(どこから壊されたか分からなくなる)。例外はログのように本当に 1 つしか
  要らず、どこからでも呼ぶもの。それも `engine/src/core/singleton.h`(作った順の逆に破棄)を使い、生成と破棄の順序をはっきりさせる。
- 壊れた場所が分かるように、サブシステム名つきのログを出す(`[reaction] ...`)。HRESULT の失敗は、呼んだ API と
  場所を必ずログに残す。ログの仕組みは T-0007 で作る。

## ビルド
- プリコンパイルヘッダ `engine/src/pch.h`: Windows・D3D12・標準ライブラリの重いヘッダをここに集め、各 .cpp は自分の
  サブシステムのヘッダだけを include する。pch.h に入れるのは「めったに変わらない外部のヘッダ」だけ(自分のヘッダは入れない)。
- MSVC `/W4 /permissive-` で警告ゼロ。

## HLSL
- SM 6.8。名前の規則は C++ と同じ。インデント 4。
