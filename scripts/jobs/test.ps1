# timeout: 1800
# テスト: ctest。GPU テスト(CPU リファレンスとの突き合わせ)もここで回す。
#   job.py test
#   job.py test -Filter reaction
#   job.py test -Filter fixed -Show     # テストが表示した行(誤差の測定値など)も出す
# GPU を使うテスト(gpu_・window_・image_)は、全部のランナーで 1 本ずつになるよう順番を待つ(_gpulock.ps1)。
# CPU だけのテストは先に待たずに流す。
param(
  [ValidateSet('debug', 'release', 'profile')][string]$Preset = 'debug',
  [string]$Filter = '',
  [switch]$Show
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_vsenv.ps1"
. "$PSScriptRoot\_gpulock.ps1"
# 以降の外部コマンド(cmake/ctest/exe)は終了コードで判定する。PS 5.1 は 'Stop' だと stderr への警告だけで止まる
$ErrorActionPreference = 'Continue'

# --- 流すテストの名前を集め、GPU を使うものと使わないものに分ける ---
$listArgs = @('--preset', $Preset, '-N')
if ($Filter) { $listArgs += @('-R', $Filter) }
$names = @(& ctest @listArgs | ForEach-Object { if ($_ -match '^\s*Test\s+#\d+:\s+(\S+)') { $Matches[1] } })
$gpuNames = @($names | Where-Object { $_ -match $GpuTestPattern })
$cpuNames = @($names | Where-Object { $_ -notmatch $GpuTestPattern })

function Invoke-Ctest([string[]]$Selected) {
  $a = @('--preset', $Preset, '--output-on-failure', '-R', ('^(' + (($Selected | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'))
  if ($Show) { $a += @('-V') }
  & ctest @a | Out-Host   # 出力は関数の戻り値に混ぜない
  return $LASTEXITCODE
}

$code = 0
if ($names.Count -eq 0) {
  Write-Host 'テストが見つかりません(-Filter を確かめる)'
  exit 1
}
if ($cpuNames.Count -gt 0) {
  $c = Invoke-Ctest $cpuNames
  if ($c -ne 0) { $code = $c }
}
if ($gpuNames.Count -gt 0) {
  Enter-GpuLock ("test " + $gpuNames.Count + " 本")
  try {
    $c = Invoke-Ctest $gpuNames
    if ($c -ne 0) { $code = $c }
  } finally { Exit-GpuLock }
}
exit $code
