# timeout: 1800
# テスト: ctest。GPU テスト(CPU リファレンスとの突き合わせ)もここで回す。
#   job.py test
#   job.py test -Filter reaction
param(
  [ValidateSet('debug', 'release', 'profile')][string]$Preset = 'debug',
  [string]$Filter = ''
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_vsenv.ps1"
# 以降の外部コマンド(cmake/ctest/exe)は終了コードで判定する。PS 5.1 は 'Stop' だと stderr への警告だけで止まる
$ErrorActionPreference = 'Continue'
$a = @('--preset', $Preset, '--output-on-failure')
if ($Filter) { $a += @('-R', $Filter) }
ctest @a
exit $LASTEXITCODE
