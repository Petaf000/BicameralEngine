# physics_lab: 物理の試作(double の AVBD。T-0016)。ツールなので浮動小数点を使ってよい(CLAUDE.md 原則 2)。
# 場面は engine の bicameral_physics(整数の定義)から読む。
add_executable(physics_lab
  physics_lab.cpp
  avbd_solver.cpp
  box_collision.cpp
)
target_link_libraries(physics_lab PRIVATE bicameral_physics bicameral_core)
