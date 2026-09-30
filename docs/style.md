# コーディング規約(C++ / HLSL)

2026-09-29 にユーザーが決めた流儀(T-0006)。2026-09-30〜10-01 に変更: { } なしの if は改行して字下げ・入れ子の名前空間と ComPtr の短い名前・処理の区切りの空行・宣言を折り返さない・構造体の欄をまとまりに分ける。見た目は `.clang-format`、名前は `.clang-tidy` が機械的に確かめる。
ここに無いことが出てきたら、ユーザーに聞いてから足す。

## 見た目(clang-format が揃える)
- インデント 4、`{` は同じ行、1 行の上限 120 桁(日本語コメントは 1 文字 2 桁で数えられる)。
- `public:` / `private:` はクラスの行頭に揃える。名前空間の中もインデントする(入れ子の名前空間も 1 段ずつ)。
- 中身が 1 行の if・else if・else・for・while は `{ }` を省き、**改行して字下げする**(同じ行には書かない):
  ```cpp
  if (!(values[2] > 0.0f))
      return nullopt;

  if (kind == Kind::Solid)
      density = SOLID_DENSITY;
  else if (kind == Kind::Liquid)
      density = LIQUID_DENSITY;
  else
      density = GAS_DENSITY;
  ```
- if / else の連鎖は分岐ごとに決める。中身が 1 行の分岐は `{ }` なし、2 行以上の分岐は `{ }` を付ける:
  ```cpp
  if (argument == L"--caps")
      options.runCaps = true;
  else if (argument == L"--log-level" && hasValue) {
      const std::string name = ToUtf8(arguments[++i]);
      options.hasLogLevel = ParseLevel(name, options.logLevel);
  } else
      return unexpected(format("知らない引数: {}", ToUtf8(argument)));
  ```
- `{ }` を付けるもの: 中身が 2 行以上(入れ子の if や、折り返した 1 文も 2 行と数える)・中身にコメントがある・
  条件が複数行に渡る(条件の続きと中身の字下げが同じで見分けにくいため)。
- clang-format は同じ行の if を改行するだけで、`{ }` の付け外しはしない(手で守る。.clang-format・.clang-tidy の注記)。
- **宣言・代入は `=` の直後で改行しない。** 長いときは右辺の引数の中で折り返す(clang-format の PenaltyBreakAssignment)。
- **行末のコメントで 1 行に収まらないときは、コメントを上の行に書く**(コメントのせいで宣言が 2 行に割れないように):
  ```cpp
  // 1 セルの長さあたりの濃さ(最大の熱で。熱の 3 乗で薄くし、奥の熱い所が透けて見えるように)
  static const float VOLUME_DENSITY = 0.25;
  ```
- `// clang-format off` の範囲(HLSL のノードの属性)も、同じ見た目(if の改行・`{ }`・空行)に手で揃える。
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
- 1 関数は 1 つの仕事、目安 50 行以内(空行を除く)。長くなったら名前の付いた関数に分ける。
- ファイルの先頭に「このファイルは何をするか・データがどこから来てどこへ行くか」を数行。コメントは「何を」より「なぜ」。
- 処理のまとまりごとに空行と区切りコメント(`// --- 〇〇 ---`)。

## 空行(詰めて書かない)
1 つの処理が終わったら空行を入れる。目安:
- `{ }` のブロック(if・ループ・ラムダ)の閉じ括弧の後。
- `{ }` なしの if(早期リターンなど)の後。
- 3 行以上に渡る文の前後。
- 2 文以上のまとまりの後の、最後の `return` の前(`{` の直後が 1 文 + return だけなら入れない)。
- 3 行以上のまとまりの後に続く `{ }` 付きの if・ループの前。
- 意味の違う処理の境目(準備 → 記録 → 投入 → 後片付け など)。同じことの繰り返し(構造体の欄を足す・同じ検査を並べる)は続けてよい。

## 構造体・クラスの宣言
- 欄(メンバー変数)とメソッドの宣言は、意味のまとまりごとに空行で分け、まとまりの頭に `// --- 〇〇 ---` を付ける:
  ```hlsl
  struct ViewConstants {
      // --- 何をどこに描くか ---
      uint32_t extraction;  // 読む抽出(0〜2)
      float2 viewport;      // 描く大きさ(画素)
      uint32_t flags;       // VIEW_FLAG_* の組み合わせ

      // --- 表示の仕方 ---
      uint32_t mode;           // VIEW_MODE_*
      uint32_t sliceAxis;      // 断面の軸(0 = x・1 = y・2 = z)
      uint32_t slicePosition;  // 断面のセルの番号(クリックがつつく面)
      ...
  };
  ```
- 名前だけで意味や単位が分からない欄には、行末に短いコメント(何か・単位・範囲)。
- GPU と共有する並び(定数バッファ・ファイルの見出し)は並べ替えない。空行とコメントだけ足す。
  クラスの欄を並べ替えるときは、コンストラクタの初期化の順も同じにする(破棄の順も変わるので、GPU の物の前後に注意)。

## 短い名前(長い修飾を書かない)
- 標準ライブラリの**入れ子の名前空間**は `engine/src/core/aliases.h`(namespace bicameral の中)の別名で書く。使うファイルは `"core/aliases.h"` を include する。

  | 書く | 元の名前 |
  |---|---|
  | `fs::` | `std::filesystem::` |
  | `rng::` / `views::` | `std::ranges::` / `std::views::` |
  | `chr::` | `std::chrono::` |

- std 直下の名前(`std::optional`・`std::span`・`std::format`・`std::vector` など)は **`std::` を付けて書く**(2026-10-01 ユーザー)。
- COM のポインタは `ComPtr`(`engine/src/gpu/com_ptr.h`)。`Microsoft::WRL::ComPtr` と書かない。GPU を知らない所(core・bicameral_view)では読まない。
- ほかの長い名前空間(`bicameral::save::` など)は、**そのファイルの中だけ**で短くする。.cpp なら
  `using namespace bicameral;` + `using save::ReplayFile;` や `namespace save = bicameral::save;`。
  ヘッダでは `using namespace` を書かない(読んだファイル全部に広がる)。
- `using namespace std;` は使わない(windows.h の `byte` などとぶつかる)。
- namespace bicameral の外(`std::formatter` の特殊化の中など)では別名が見えないので、元の名前で書く。

## 分け方・つなぎ方
- 機能ごとに `engine/src/<subsystem>/` に分ける。サブシステムどうしは小さなインターフェース(ヘッダの関数・構造体)だけで
  つなぎ、中身(実装の型・グローバル)を他から直接触らせない。依存は引数で渡す。
- グローバルな状態・シングルトンは使わない(どこから壊されたか分からなくなる)。例外はログのように本当に 1 つしか
  要らず、どこからでも呼ぶもの。それも `engine/src/core/singleton.h`(作った順の逆に破棄)を使い、生成と破棄の順序をはっきりさせる。
- 壊れた場所が分かるように、サブシステム名つきのログを出す: `Log(Channel::Reaction, Level::Warning, "...", ...)`。
  HRESULT は `BICAMERAL_CHECK_HR(Channel::Gpu, 式)` で受ける(式・場所・エラーの意味が残る)。詳細は docs/design/03-logging.md・ADR-0006。
- ゲームエンジンで定番の作り方の型は使ってよい(2026-09-29 ユーザー)。このエンジンに合うのは、MVC よりも
  データと処理を分けるデータ指向(世界のデータ = VRAM のバッファ、処理 = カーネル / Work Graph のノード)・
  サブシステム + 一方向の依存・ランタイムとツール(エディタ・ベイク)の分離。型に合わない所は無理に当てはめない。

## ビルド
- プリコンパイルヘッダ `engine/src/pch.h`: Windows・D3D12・標準ライブラリの重いヘッダをここに集め、各 .cpp は自分の
  サブシステムのヘッダだけを include する。pch.h に入れるのは「めったに変わらない外部のヘッダ」だけ(自分のヘッダは入れない)。
- MSVC `/W4 /permissive-` で警告ゼロ。

## HLSL
- SM 6.8。名前の規則は C++ と同じ。インデント 4。
