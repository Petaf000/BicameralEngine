# Visual Studio (2026 優先) の開発者環境をこの PowerShell に読み込む。build / test から dot-source する。
# 手元で Visual Studio を開いている必要はない。vswhere で最新の VS を探す。
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe が見つかりません。Visual Studio がインストールされていますか?' }

$vs = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
if (-not $vs) { throw 'C++ ワークロード入りの Visual Studio が見つかりません' }

if (-not $env:VSCMD_VER) {
  Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
# VS 同梱の vcpkg を使う(自前の vcpkg を使うなら VCPKG_ROOT を先に設定しておく)
if (-not $env:VCPKG_ROOT) {
  $bundled = Join-Path $vs 'VC\vcpkg'
  if (Test-Path $bundled) { $env:VCPKG_ROOT = $bundled }
}
Write-Host "VS: $vs"
Write-Host "VCPKG_ROOT: $env:VCPKG_ROOT"
