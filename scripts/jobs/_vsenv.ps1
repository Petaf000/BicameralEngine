# Visual Studio (2026 優先) の開発者環境をこの PowerShell に読み込む。build / test から dot-source する。
# 手元で Visual Studio を開いている必要はない。vswhere で最新の VS を探す。
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe が見つかりません。Visual Studio がインストールされていますか?' }

$vs = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
if (-not $vs) { throw 'C++ ワークロード入りの Visual Studio が見つかりません' }

# VsDevCmd.bat を cmd /u で動かし、終わった後の環境変数を UTF-16 のファイルに書かせて読み込む(T-0455)。
# Enter-VsDevShell(DevShell.dll)は使わない: 環境を写す時に ASCII の外の文字を含む値(ユーザーのフォルダを指す USERPROFILE・
# LOCALAPPDATA・APPDATA・USERNAME・Path の一部など)を化けさせる。化けた環境から起動したプロセスでは GPU のドライバが
# シェーダーのキャッシュを使わず、ctest の GPU の試験がパイプラインを作るたびにコンパイルし直していた
# (release の溢れの場面 1 回 約 60 秒。run.ps1 はこの環境を読まないので 1〜2 秒。どの変数が効くかは docs/tickets/T-0455)。
function Import-VsDevEnvironment([string]$InstallPath) {
  $batch = [IO.Path]::Combine([IO.Path]::GetTempPath(), "bicameral-vsenv-$PID.cmd")
  $dump = [IO.Path]::Combine([IO.Path]::GetTempPath(), "bicameral-vsenv-$PID.txt")
  # バッチは ASCII だけにする(パスは環境変数で渡す。cmd はバッチを OEM のコードページで読む)
  $env:BICAMERAL_VSDEVCMD = Join-Path $InstallPath 'Common7\Tools\VsDevCmd.bat'
  $env:BICAMERAL_VSENV_DUMP = $dump
  $lines = @(
    '@echo off',
    'call "%BICAMERAL_VSDEVCMD%" -arch=x64 -host_arch=x64 -no_logo >nul 2>&1',
    'set > "%BICAMERAL_VSENV_DUMP%"'
  )
  [IO.File]::WriteAllLines($batch, [string[]]$lines, [Text.Encoding]::ASCII)
  try {
    & $env:ComSpec /d /u /c "`"$batch`""   # /u: 内部コマンド(set)のファイルへの出力を UTF-16 にする
    if (-not (Test-Path $dump)) { throw 'VS の開発者環境を読めません(VsDevCmd.bat の後の set が書かれていない)' }
    $vars = [IO.File]::ReadAllLines($dump, [Text.Encoding]::Unicode)
  } finally {
    Remove-Item $batch, $dump -ErrorAction SilentlyContinue
    Remove-Item env:BICAMERAL_VSDEVCMD, env:BICAMERAL_VSENV_DUMP -ErrorAction SilentlyContinue
  }

  foreach ($line in $vars) {
    $at = $line.IndexOf('=')
    if ($at -le 0) { continue }   # 「=C:=...」(cmd の隠れた変数)は飛ばす
    $name = $line.Substring(0, $at)
    if ($name -like 'BICAMERAL_VS*') { continue }
    $value = $line.Substring($at + 1)
    if ([Environment]::GetEnvironmentVariable($name) -cne $value) { [Environment]::SetEnvironmentVariable($name, $value) }
  }
  if (-not $env:VSCMD_VER) { throw 'VS の開発者環境を読めません(VSCMD_VER が無い。VsDevCmd.bat が失敗した?)' }
}

if (-not $env:VSCMD_VER) { Import-VsDevEnvironment $vs }
# VS 同梱の vcpkg を使う(自前の vcpkg を使うなら VCPKG_ROOT を先に設定しておく)
if (-not $env:VCPKG_ROOT) {
  $bundled = Join-Path $vs 'VC\vcpkg'
  if (Test-Path $bundled) { $env:VCPKG_ROOT = $bundled }
}
Write-Host "VS: $vs"
Write-Host "VCPKG_ROOT: $env:VCPKG_ROOT"
