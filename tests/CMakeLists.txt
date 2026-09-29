# テスト。GPU カーネルごとに「CPU リファレンス実装との一致」テストを置く方針(docs/design/02-reaction-system.md)。
add_executable(smoke_test smoke_test.cpp)
add_test(NAME smoke COMMAND smoke_test)
