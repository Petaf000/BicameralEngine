# T-0037 生成器(整数・GPU・どのレベルでも・生成手順を Luau で)

- Status: Todo(2026-10-09 に司令塔が事前調査を置いた。着手はまだ)
- 設計: 05 §6・17・13 §2・ADR-0015・ADR-0016・ADR-0030〜0033・ADR-0046。関係: T-0038(生成器の出力と同じブロックは捨てて作り直す前提)

## 事前調査(2026-10-09、調べ役。読むだけ。未確認の数値は「推定」「未確認」)

### 今の実装の要点
- 生成器のコードは無い。05 §6 は「シード + Luau の手順(表にベイク)+ 差分」「GPU の整数シェーダーでブロックが要る時に要るレベルで」「CPU リファレンスとビット一致」だけ。
- 木(ADR-0015): 細かくすると子は親と同じ濃度、粗くすると子 8 個の和 ÷ 8 で余りは 64bit の端数。一様なブロックは頁を持たない(T-0102)。静かな所を粗くできるのは許容差の中だけ(D-430・D-435)。枠と頁の番号は累積和(ADR-0016)。
- 使える部品: FxHash64(SplitMix64 を 4 回。04 R6 のカウンタ型乱数)・C++ と HLSL で共有する FX_FN の固定小数点の関数。Luau は殻・パッケージ・型検査・整数だけを名前順でベイク・起動時にベイクまである。

### 結論(推奨)
1. **上から下へ配る生成**(研究の中心): 粗いレベルが物質量の合計を決め、細かいレベルはそれを子 2³ に配り直すだけ。細かいレベルのノイズの値で子を並べ、余りは大きい順に割り当て、同点はハッシュ。どのレベルでも「粗い層 = 細かい層の集計」が端数 0 のまま必ず成り立ち、深さにも上限が無い。下から上へ(最も細かいレベルで評価して足し上げる)はやめる(8m の層に 0.5m のセルを全部数える必要があり重い・それより細かいレベルも作れない)。各レベルのオクターブはそのセルで表せる周波数まで(ナイキスト)。
2. **生成の正準の木 = 許容差(D-435)で畳めない所まで細かくした木**。実体として作るのは要る所だけ(全部を VRAM は不可能。TotK 規模の地表 0.5m で数百 GB 級・推定)。見出しに「生成のまま」の印を状態として持ち、印のある親を細かくする時は一様に写さず生成器で細部を作る → T-0038 の「捨てて作り直す」と D-430(細かい構造を消さない)が両立。
3. **ノイズは整数**: ハッシュは pcg3d / pcg4d(32bit。Jarzynski & Olano の多次元の既定)、int64 の座標の上位ビットは w に畳む。FxHash64 は一度きりの判断に残す。部品は値ノイズ・勾配ノイズ(Q16・5 次のフェード・12 方向の整数の勾配)・3D Simplex(斜交の係数 1/3・1/6 は有理数で整数で正確。特許 US6867776 は 2022-01 に失効)・Worley。FX_FN で CPU と GPU を同じ源に。
4. **Luau → 式の DAG → GPU 用のバイトコード → 1 本の解釈カーネル**(Minecraft の密度関数と同じ考え)。プログラムは全スレッドで同じなので分岐は揃う。CPU リファレンスは同じ解釈器を C++ で。演算: 整数の四則とシフト・clamp/min/max/select・ノイズの部品・domain warp・区分線形のスプライン(整数の表)・2D の列のキャッシュ。規則の表(地層・バイオーム・鉱脈・洞窟)は「導いた場 → 素材の配合」の判定表で、配合は反応表の物質名(ADR-0032 と同じく名前順で ID)。
5. **化学との整合をベイクで検査**: 配合が反応表の物質であること。生成で隣り合いうる物質の組について、周りの条件で規則が進むか(D-429 の起こす刻み)を表にし、速く進む組は誤りか警告(生成したままの世界が化学的に動いていると世界中が wakeTick で起き続ける)。

### 方式の比較
- 生成手順の形: 決まった部品だけ(最速・表現が狭く Mod に向かない)/ **バイトコードの解釈(推奨。速さは未確認、コンパイルの 2〜5 倍遅いと推定)**/ DAG から HLSL を作り DXC でコンパイル(最速だが起動時のコンパイル・浮動小数点の検査・Mod ごとのシェーダー。解釈で遅すぎた時の最適化として後で同じ DAG から)。
- ハッシュ: pcg3d/4d(質と速さの釣り合い)/ xxHash32(多次元はやや重い)/ Squirrel3(質の評価が JCGT の比較に無い・未確認)/ FxHash64(64bit の掛け算が重く格子 8 点 × オクターブに向かない)。
- ノイズ: 値ノイズ(安いが格子の筋)/ Perlin Q16(標準・8 点のハッシュ)/ Simplex(3D で 4 点・筋が少ない)/ Worley(鉱脈の塊・洞窟の部屋・結晶の粒)。Wavelet noise は「粗い = 細かいを帯域で絞ったもの」の参考。
- 事例: Minecraft 1.18 以降の密度関数(JSON の式の木・noise router。Mod の C2ME はコンパイルして約 90% 速いと changelog)/ No Man's Sky(CPU のジョブ・浮動小数点。GPU かは未確認)/ Dwarf Fortress(鉱物が母岩を ENVIRONMENT で指定 = データで書く地質の法則の先例)/ GPU Gems 3 1 章 / Teardown・Astroneer は未確認。

### 発見できる法則の埋め方
- **見えない原因の場を 1 つ生成し、見える結果を複数そこから決める**(例: 古いマグマの貫入の場 → 岩の種類・鉱脈・温泉の成分・魔素の濃さ)。結果どうしの相関が自然に生まれ、プレイヤーは原因を推理できる。
- 地下水の成分はベイク時に反応表で母岩と釣り合わせる(生成と化学が同じ法則)。指標の植物・風化の順(Goldich)・鉱物の共生も同じ形。顕微鏡の鉱物の粒(k = 6〜10)は岩の配合から上から下へ配って作れる。

### 試す順番(D-411)と次の代案へ移る条件
1. 工学: 整数ノイズの部品と pcg を FX_FN で。CPU と GPU でビット一致・質(ヒストグラム・軸の筋)と 1 サンプルの費用 → Perlin が重い・筋が出るなら Simplex か値ノイズ + オクターブ追加。
2. 工学: Luau の DSL → DAG → バイトコード → 解釈カーネル。試験の手順(高さ・地層・洞窟)でビット一致・1 ms あたり何ブロック。
3. 研究: 上から下へ配る生成。合格: レベル −6〜+6 の全部で coarsen(G(k+1)) == G(k) がビット一致・端数 0 / 親の粗い格子の跡が細部に見えない(配った結果と条件なしの候補の食い違いの率)/ k=0 のブロックを祖先の鎖ごと作る費用。代案の順: 大きい順の割り当て → 2 段先を見て被覆を見積もる → 表せない高い周波数の期待値を足す → 地表の帯だけ下から上へ。
4. 工学: 正準の木と「生成のまま」の印。作り直したら捨てる前とビット一致することを T-0038 と結んで確かめる。
5. 工学: 規則の表(地層・鉱脈・水)とベイクでの反応表との整合の検査。
6. 計測: 解釈がコンパイルの 3 倍を超え、かつ T-0038 の範囲を予算内に作れない時だけ HLSL を作る方式へ。64bit 座標の費用が効く時は地域ごとの局所の原点で 32bit に。

### ユーザーに聞く点 → QUESTIONS Q26

### リスクと未確認
- D-312「粗い 8m の層は常に VRAM」と「仮想の正準の木」の関係(粗い層は要約か刻むセルか)を ADR で決める必要。混ざった粗いセルで反応を刻むと観測者で法則が変わる(D-208)恐れ。
- 解釈カーネルの速さ・int64 の掛け算の費用(04: 6.4 命令)・祖先の鎖の費用は未計測。大きい順の割り当ての見た目(格子の跡)は研究。
- Minecraft の決定性: Java 17 以降は浮動小数点がすべて strict(JEP 306)。Bedrock と Java の生成の違いは未確認。

### 出典
- Hash Functions for GPU Rendering(JCGT 2020): https://www.jcgt.org/published/0009/03/02/paper.pdf / https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering
- Squirrel Eiserloh(GDC 2017): https://gdcvault.com/play/1024365/Math-for-Game-Programmers-Noise
- Simplex: https://en.wikipedia.org/wiki/Simplex_noise / https://patents.google.com/patent/US6867776
- Minecraft: https://minecraft.wiki/w/Tutorial%3ACustom_world_generation / https://maven.fabricmc.net/docs/yarn-1.19+build.1/net/minecraft/world/gen/noise/NoiseRouter.html / https://openjdk.org/jeps/306 / C2ME: https://modrinth.com/mod/c2me-fabric/version/0.4.0-beta.1.0+26.1.2
- No Man's Sky: https://www.gdcvault.com/play/1024265/Continuous-World-Generation-in-No / https://gdcvault.com/play/1024514/Building-Worlds-Using
- Dwarf Fortress: https://dwarffortresswiki.org/index.php/Layer / https://www.dwarffortresswiki.org/index.php/v0.34:Inorganic_material_definition_token
- GPU Gems 3 1 章: https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu / Wavelet Noise: https://graphics.pixar.com/library/WaveletNoise/paper.pdf
