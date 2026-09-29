// Bicameral Engine — ランタイムの入口。
//
// 今はコマンドラインの振り分けだけ。窓・スワップチェイン・フレームループは T-0004 以降。
//   bicameral --caps      GPU の対応状況を表示して終了
#include "platform/caps.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--caps") == 0) return bicameral::RunCapsProbe();
  }
  std::printf("Bicameral Engine (skeleton). Try --caps\n");
  return 0;
}
