#pragma once

namespace bicameral {
    // GPU の対応状況(SM / DXR / Mesh Shader / Work Graphs)をログ(Channel::Platform)に出す。成功で 0。
    int RunCapsProbe();
}  // namespace bicameral
