# timeout: 1800
# テスト: ctest。GPU テスト(CPU リファレンスとの突き合わせ)もここで回す。
#   job.py test
#   job.py test -Filter reaction
#   job.py test -Filter fixed -Show     # テストが表示した行(誤差の測定値など)も出す
param(
  [ValidateSet('debug', 'release', 'profile')][string]$Preset = 'debug',
  [string]$Filter = '',
  [switch]$Show
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_vsenv.ps1"
# 以降の外部コマンド(cmake/ctest/exe)は終了コードで判定する。PS 5.1 は 'Stop' だと stderr への警告だけで止まる
$ErrorActionPreference = 'Continue'
$a = @('--preset', $Preset, '--output-on-failure')
if ($Filter) { $a += @('-R', $Filter) }
if ($Show) { $a += @('-V') }
ctest @a
exit $LASTEXITCODE
