# timeout: 2400
# ビルド: CMakePresets.json のプリセットで configure + build。
#   job.py build                   → debug
#   job.py build -Preset release
#   job.py build -Clean            → ビルドフォルダを消してから
param(
  [ValidateSet('debug', 'release', 'profile')][string]$Preset = 'debug',
  [switch]$Clean,
  [string]$Target = ''
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_vsenv.ps1"

$bin = Join-Path (Get-Location) "out\build\$Preset"
if ($Clean -and (Test-Path $bin)) { Remove-Item -Recurse -Force $bin }

cmake --preset $Preset
if ($LASTEXITCODE) { Write-Host "CONFIGURE FAILED ($LASTEXITCODE)"; exit $LASTEXITCODE }

$args2 = @('--build', '--preset', $Preset)
if ($Target) { $args2 += @('--target', $Target) }
cmake @args2
if ($LASTEXITCODE) { Write-Host "BUILD FAILED ($LASTEXITCODE)"; exit $LASTEXITCODE }
Write-Host "BUILD OK ($Preset)"
