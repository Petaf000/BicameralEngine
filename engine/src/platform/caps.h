#pragma once

namespace bicameral {
// GPU の対応状況(SM / DXR / Mesh Shader / Work Graphs)を標準出力に書く。成功で 0。
int RunCapsProbe();
}  // namespace bicameral
