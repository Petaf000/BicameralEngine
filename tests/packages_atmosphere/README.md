# tests/packages_atmosphere — world_atmosphere_test の試験のパッケージ(T-0499)

1 つのフォルダ = 1 つのパッケージの置き場(packageRoot)で、中のパッケージをゲーム本体として読む。依存の標準パッケージは data/standard から。

| 置き場 | 中身 |
|---|---|
| uniform | 一様な大気の書き方(分類 atmosphere + atmosphere_temperature_mk。T-0373)= 高さ 0 の点 1 つ |
| settled | earth_environment を写し、補助 atmosphere.settled で重い物が下に溜まる点を作り、set_point で海面の点を書き直す |
| conflict | earth_environment(高さの点の列)の上に一様な書き方を足す(一緒に書けないので読み込みの誤り) |
| unsorted | 高さが低い順に並んでいない点の列(ベイクの誤り) |
