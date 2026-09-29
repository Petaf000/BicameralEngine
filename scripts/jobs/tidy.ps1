# timeout: 1800
# clang-tidy(.clang-tidy の規則)を engine/ と tests/ の C++ にかける。警告があれば失敗(tools/tidy/run_clang_tidy.py)。
#   job.py tidy                 # debug のビルドフォルダの compile_commands.json を使う(先に job.py build)
#   job.py tidy -Preset release
# clang-tidy は Visual Studio 同梱のもの(VC\Tools\Llvm\x64\bin。「C++ Clang tools」コンポーネント)。
param(
  [ValidateSet('debug', 'release', 'profile')][string]$Preset = 'debug'
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_vsenv.ps1"
$ErrorActionPreference = 'Continue'
$tidy = Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang-tidy.exe'
if (-not (Test-Path $tidy)) { Write-Host "clang-tidy が見つかりません: $tidy"; exit 2 }
$build = "out\build\$Preset"
if (-not (Test-Path "$build\compile_commands.json")) { Write-Host "$build\compile_commands.json がありません。先に job.py build -Preset $Preset"; exit 2 }
python tools\tidy\run_clang_tidy.py --build-dir $build --clang-tidy $tidy
exit $LASTEXITCODE
