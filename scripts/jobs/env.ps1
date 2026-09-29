# 環境確認: 最初のセッションと、ドライバ/VS を更新したときに実行する。
$ErrorActionPreference = 'Continue'
Write-Host '== OS =='
$os = Get-CimInstance Win32_OperatingSystem
Write-Host "$($os.Caption) build $($os.BuildNumber)"
Write-Host "開発者モード: $((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense)"

Write-Host '== GPU =='
Get-CimInstance Win32_VideoController | ForEach-Object { Write-Host "$($_.Name)  driver $($_.DriverVersion)" }

Write-Host '== Visual Studio =='
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
  # PS 5.1 の ConvertFrom-Json は行ごとに流すと失敗するので、1 つの文字列にまとめてから渡す
  (& $vswhere -all -prerelease -products * -format json -utf8 | Out-String) | ConvertFrom-Json | ForEach-Object { $_ } |
    ForEach-Object { Write-Host "$($_.displayName)  $($_.installationVersion)  $($_.installationPath)" }
} else { Write-Host 'vswhere なし' }

Write-Host '== ツール =='
foreach ($t in 'git', 'cmake', 'ninja', 'dxc', 'python') {
  $c = Get-Command $t -ErrorAction SilentlyContinue
  if ($c) { Write-Host "$t : $($c.Source)" } else { Write-Host "$t : (PATH に無い。VS 開発者環境内にある場合あり)" }
}
. "$PSScriptRoot\_vsenv.ps1"
foreach ($t in 'cmake', 'ninja', 'cl', 'dxc') {
  $c = Get-Command $t -ErrorAction SilentlyContinue
  if ($c) { Write-Host "[VS] $t : $($c.Source)" } else { Write-Host "[VS] $t : なし" }
}
cmake --version | Select-Object -First 1
# DXC の版(SM6.8 = lib_6_8 / cs_6_8 が通るか)
$dxc = Get-Command dxc -ErrorAction SilentlyContinue
if ($dxc) {
  & dxc --version 2>&1 | ForEach-Object { "dxc: $_" }
  $prof = (& dxc -help 2>&1 | Out-String)
  foreach ($t in 'lib_6_8', 'cs_6_8', 'lib_6_9') { Write-Host ("dxc profile {0} : {1}" -f $t, $(if ($prof -match $t) { 'あり' } else { 'なし' })) }
}
Write-Host '== git =='
git -c color.ui=false status --short --branch
