# timeout: 600
# エンジン実行: GPU を実際に使う確認はすべてこれで行う。
#   job.py run -- --caps
#   job.py run -- --frames 120 --screenshot            → $env:JOB_OUT\frame.png に保存(エンジン側で実装)
#   job.py run -Preset release -- --frames 600 --stats
# 表示が要るのでユーザーがログオンしている必要がある。長時間放置ならタイムアウトで止まる。
param(
  [ValidateSet('debug', 'release', 'profile')][string]$Preset = 'debug',
  [string]$Exe = 'bicameral',
  [string[]]$AppArgs = @()
)
$ErrorActionPreference = 'Stop'
$path = Join-Path (Get-Location) "out\build\$Preset\bin\$Exe.exe"
if (-not (Test-Path $path)) { Write-Host "実行ファイルがありません: $path(先に build)"; exit 2 }
# エンジンの終了コードで判定する。PS 5.1 は 'Stop' だと stderr への出力(デバッグレイヤーのメッセージなど)だけで止まる
$ErrorActionPreference = 'Continue'

$env:BICAMERAL_OUT = $env:JOB_OUT      # エンジンはスクショ・計測をここに書く
Push-Location (Split-Path $path)
try {
  & $path @AppArgs
  $code = $LASTEXITCODE
} finally { Pop-Location }
Write-Host "EXIT $code"
exit $code
