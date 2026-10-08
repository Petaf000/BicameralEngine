# ADR-0030 Luau は vcpkg の port で組み込み、許したものだけを見せる殻(上限つき・実行ごとに種を戻す)で動かす

- Status: Accepted(殻〔T-0020〕の形。パッケージ〔T-0138〕とホットリロード〔T-0139〕もこの殻の上に作る)
- 日付: 2026-10-09
- 決めた人: Claude(実装の細部。CLAUDE.md §5。Luau を使う・最初からサンドボックス・Mod 前提はユーザーが決めた〔D-313・D-318・D-320〕。T-0020)

## 背景
- ゲームを作る人が触るものは全部 Luau(D-318)。Mod も同じ環境で動かす(D-320)ので、最初からサンドボックスにする必要がある。
- 13「未確認」: Luau を C++ にどう入れるか(vcpkg にあるか、自前でビルドするか)。
- 原則 3(完全な決定性)。Luau は CPU のオーサリング用(原則 1・2)だが、ベイクの結果(GPU に渡す表)は同じ入力から同じバイト列でなければならない(13「テスト」)。
  素の Luau には決定性を壊すものがある: `math.random` の既定の種は時計とアドレス(lmathlib.cpp)、`os.clock`・`os.time`、`gcinfo`(メモリの量)。

## 決定
1. **vcpkg の `luau` port**(builtin-baseline の版 0.729)を使う。CMake は `find_package(unofficial-luau)`、`unofficial::luau::Luau.VM` と `Luau.Compiler` をつなぐ。
   - この port はソースにパッチを当てる(vcpkg が git apply を使う)。ユーザー名が日本語だと git が化けた `%USERPROFILE%\.config\git\config` を開こうとして落ちるので、
     CMakePresets.json で `XDG_CONFIG_HOME`・`GIT_CONFIG_GLOBAL` を `out/vcpkg/git-config` に向け、`VCPKG_KEEP_ENV_VARS` で port のビルドへ渡す。
2. **殻 `script::LuauSandbox`**(engine/src/script/luau_sandbox.*)。見せるものは**許したものだけ**(許可の一覧。禁止の一覧ではない):
   - 標準: base(`gcinfo`・`getfenv`・`setfenv`・`newproxy` を除く。`print` はログか呼び手の受け口へ)・coroutine・table・string・math・utf8・bit32・buffer・vector。
   - 開かない: os・debug・require(require はパッケージ〔T-0138〕で、パッケージの中だけを読む形で足す)。io・load 系はもともと Luau に無い。
   - ホストの関数は Seal() の前に 1 つの表へ登録する(例 `host.emit`)。Seal() で `luaL_sandbox`(グローバルと標準ライブラリを読み取り専用)。
   - Run ごとに自分用のスレッド(`luaL_sandboxthread`)。グローバルへの書き込みはそのスレッドの表に入り、ほかの Run(ほかのパッケージ・Mod)と混ざらない。
3. **上限**: 安全点(ループの折り返し・呼び出し。Luau の interrupt)の数と、殻全体のメモリ(確保の関数で数える)。超えたら error で止め、理由の種類を返す。
   超えた後は安全点ごとに投げ続ける(pcall で捕まえても止まる)。メモリで止まったらすぐ GC する(次の Run が巻き添えにならないように)。
   命令の数ではなく安全点の数にしたのは、Luau が命令ごとの呼び戻しを持たないため。安全点の数は同じバイトコードなら決まった数になる(決定性の検査にも使える)。
4. **決定性**: Run の始めに `math.randomseed(SandboxLimits::randomSeed)` で種を戻す。戻り値の文字列はアドレスを含めない(表・関数は `<table>` のように型の名前だけ)。
   - 残る非決定性(殻では消せないので、書く側の規則にする。13 §2): 関数・表・userdata を**キーにした**表を `pairs` で回す順番はアドレスで決まる。ベイクはキーを並べてから使う。
   - 機種をまたぐ決定性: 倍精度の四則と平方根は IEEE で決まるが、超越関数(`math.sin`・`exp`・`log`・`^` が使う pow)は C ランタイム次第(**未確認**)。
     ベイクがこれらを使うかどうかは反応表のベイク(T-0021)で決める(使うなら決定的な自前の実装に差し替える)。

## 検討した代案と、採らなかった理由
- 自前で Luau をビルド(FetchContent など): 版の固定とキャッシュを vcpkg に揃えたほうが、ほかの依存(DirectX 系)と同じ手順で済む。
- `luaL_openlibs` で全部開いてから危ないものを消す(禁止の一覧): Luau の版が上がって新しいライブラリが増えたとき、黙って見えてしまう。許可の一覧なら増えない。
- 命令の数で止める(Lua 5.x の count フック): Luau には無い。時計で止める: 決定的でない(同じ Mod が速い PC では通り、遅い PC では止まる)。

## 影響(何がしやすく/しにくくなるか)
- しやすい: Mod とゲーム本体が同じ殻で動き、ファイル・OS・ネットワークに触れられない。無限ループやメモリの食いつぶしでエディタが固まらない。
  上限で止まるかどうかが PC の速さに依らない。
- しにくい: スクリプトから時計やファイルを直接使えない(要るものはホストの関数として、決まった形で見せる)。
- 注意: 新しい Luau の版に上げるときは、増えた標準関数を許可の一覧に入れるか確かめる(luau_sandbox_test の「無いもの」の一覧も)。
