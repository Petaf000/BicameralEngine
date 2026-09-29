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
$a = @('--preset', $Preset, '--output-on-failure')
if ($Filter) { $a += @('-R', $Filter) }
ctest @a
exit $LASTEXITCODE
