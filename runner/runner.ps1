<#
  Bicameral Engine ジョブランナー (NMS Web Viewer ランナーの改良版)

  Claude が runner\queue\ に置いた署名付きスクリプトを 1 つずつ実行し、結果を runner\logs\ に書く。

  起動(通常の PowerShell で。ウィンドウは開いたままにする):
    powershell -NoProfile -ExecutionPolicy Bypass -File H:\BicameralEngine\runner\runner.ps1

  止め方: ウィンドウを閉じる / Ctrl+C / runner\STOP を置く

  鍵(固定):
    - <リポジトリ>\.bicameral-runner\key.txt。初回起動時に作り、以後ずっと同じ鍵を使う
    - git 管理外(.gitignore)。Claude はリポジトリのフォルダの接続だけで鍵を読める(ユーザーが貼る必要はない)
      → 「ランナーが動いていて、リポジトリのフォルダを接続したチャット」がジョブを実行できる
    - 鍵を作り直すとき: -RotateKey(以前の鍵で署名されたジョブは拒否される)

  その他の仕組み:
    - heartbeat.json に鍵 ID(鍵の SHA-256 先頭 8 桁)を出す。Claude は投入前に一致を確かめる
    - 同じ署名のジョブは 1 回の起動中に 1 回しか実行しない(done から queue へ戻す再実行を防ぐ)
    - ジョブごとの成果物フォルダ runner\out\<job>\ を $env:JOB_OUT で渡す
    - result.json にログ末尾とエラー行を入れる(Claude がログ全体を読まずに済むように)
    - 1 行目の "# sig: <HMAC-SHA256>" が合わないジョブは実行せず runner\rejected\ に隔離する
    - 管理者権限では動かさない / ジョブは 1 つずつ、時間制限付き
    - -MaxHours N を付けると N 時間で自動停止(既定は無制限)
#>
param(
  [int]$DefaultTimeoutSec = 1800,
  [int]$PollMs = 1500,
  [double]$MaxHours = 0,
  [switch]$RotateKey,
  [switch]$ShowKey,
  [int]$TailLines = 60
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$Root    = Split-Path -Parent $MyInvocation.MyCommand.Path     # ...\runner
$Project = Split-Path -Parent $Root                            # ...\BicameralEngine

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Host '管理者として起動されています。通常の PowerShell で起動し直してください。' -ForegroundColor Red
  exit 1
}

$Dirs = @{}
foreach ($n in 'queue', 'running', 'done', 'rejected', 'logs', 'out') {
  $p = Join-Path $Root $n
  New-Item -ItemType Directory -Force -Path $p | Out-Null
  $Dirs[$n] = $p
}
Remove-Item -LiteralPath (Join-Path $Root 'STOP') -ErrorAction SilentlyContinue

# ---- 固定鍵(リポジトリの中の git 管理外。PC 固有のパスを書かないよう $Project から作る) ----
$KeyDir  = Join-Path $Project '.bicameral-runner'
$KeyFile = Join-Path $KeyDir 'key.txt'
if ($RotateKey -or -not (Test-Path $KeyFile)) {
  New-Item -ItemType Directory -Force -Path $KeyDir | Out-Null
  $rnd = New-Object byte[] 32
  [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($rnd)
  $hex = ([BitConverter]::ToString($rnd) -replace '-', '').ToLower()
  [IO.File]::WriteAllText($KeyFile, $hex)
  Write-Host "鍵を新しく作りました: $KeyFile" -ForegroundColor Yellow
}
$KeyHex = ([IO.File]::ReadAllText($KeyFile)).Trim().ToLower()
if ($KeyHex -notmatch '^[0-9a-f]{64}$') { Write-Host "鍵の形式が不正です: $KeyFile(-RotateKey で作り直せます)" -ForegroundColor Red; exit 1 }
$Key = New-Object byte[] 32
for ($i = 0; $i -lt 32; $i++) { $Key[$i] = [Convert]::ToByte($KeyHex.Substring($i * 2, 2), 16) }
$sha    = [Security.Cryptography.SHA256]::Create()
$KeyId  = (([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($KeyHex))) -replace '-', '').ToLower()).Substring(0, 8)
$Seen   = New-Object 'System.Collections.Generic.HashSet[string]'
$StartedAt = Get-Date

function Get-VerifiedBody([byte[]]$raw) {
  # 1 行目 = "# sig: <hex>"、2 行目以降(バイト列そのまま)に対する HMAC-SHA256
  $nl = [Array]::IndexOf($raw, [byte]10)
  if ($nl -lt 0) { return $null }
  $first = [Text.Encoding]::UTF8.GetString($raw, 0, $nl).Trim([char[]]@([char]0xFEFF, [char]13, [char]32))
  if ($first -notmatch '^#\s*sig:\s*([0-9a-f]{64})$') { return $null }
  $sig  = $Matches[1]
  $body = New-Object byte[] ($raw.Length - $nl - 1)
  [Array]::Copy($raw, $nl + 1, $body, 0, $body.Length)
  $h    = New-Object Security.Cryptography.HMACSHA256 (, $Key)
  $calc = ([BitConverter]::ToString($h.ComputeHash($body)) -replace '-', '').ToLower()
  if ($calc -ne $sig) { return $null }
  return , @($sig, $body)
}

function Write-Result($name, $obj) {
  $json = $obj | ConvertTo-Json -Depth 4
  [IO.File]::WriteAllText((Join-Path $Dirs.logs "$name.result.json"), $json, (New-Object Text.UTF8Encoding $false))
}

function Get-LogSummary($log, $err) {
  $lines = @()
  foreach ($f in @($log, $err)) { if (Test-Path $f) { $lines += @(Get-Content -LiteralPath $f -Encoding UTF8 | ForEach-Object { [string]$_ }) } }
  $errors = $lines | Where-Object { $_ -match '(?i)(\berror\b|fatal|exception|failed|D3D12 ERROR|DXGI ERROR|TDR|DEVICE_REMOVED|Assertion)' } | Select-Object -First 40
  $tail   = $lines | Select-Object -Last $TailLines
  return [ordered]@{ lines = $lines.Count; errors = @($errors); tail = @($tail) }
}

function Write-Heartbeat($state) {
  $hb = [ordered]@{ time = (Get-Date).ToString('o'); keyId = $KeyId; state = $state; project = $Project }
  [IO.File]::WriteAllText((Join-Path $Root 'heartbeat.json'), ($hb | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
}

Write-Host ''
Write-Host 'Bicameral Engine ジョブランナー' -ForegroundColor Cyan
Write-Host "  プロジェクト : $Project"
Write-Host "  キュー       : $($Dirs.queue)"
Write-Host "  鍵 ID        : $KeyId"
Write-Host "  鍵ファイル   : $KeyFile"
if ($ShowKey) { Write-Host "  鍵           : $KeyHex" -ForegroundColor Yellow }
Write-Host ''
Write-Host '  Claude のチャットで「続き」と送れば、Claude が鍵フォルダの接続を求めて自分で鍵を読みます'
if ($MaxHours -gt 0) { Write-Host "  $MaxHours 時間後に自動で止まります" }
Write-Host '  止めるときはこのウィンドウを閉じてください'
Write-Host ''

try {
  while ($true) {
    if (Test-Path (Join-Path $Root 'STOP')) { Write-Host 'STOP ファイルを検出したので終了します'; break }
    if ($MaxHours -gt 0 -and ((Get-Date) - $StartedAt).TotalHours -ge $MaxHours) { Write-Host "起動から $MaxHours 時間経ったので終了します"; break }
    Write-Heartbeat 'idle'

    $job = Get-ChildItem -Path $Dirs.queue -Filter '*.ps1' -File | Sort-Object Name | Select-Object -First 1
    if (-not $job) { Start-Sleep -Milliseconds $PollMs; continue }

    $name = $job.BaseName
    Start-Sleep -Milliseconds 300
    $raw = [IO.File]::ReadAllBytes($job.FullName)
    $v   = Get-VerifiedBody $raw

    if ($null -eq $v) {
      Move-Item -LiteralPath $job.FullName -Destination (Join-Path $Dirs.rejected $job.Name) -Force
      Write-Result $name ([ordered]@{ job = $name; exit = 'rejected'; reason = '署名が無いか一致しない(鍵が作り直された可能性)' })
      Write-Host "[拒否] $name(署名が無いか一致しない)" -ForegroundColor Red
      continue
    }
    $sig = $v[0]; $body = [byte[]]$v[1]
    if (-not $Seen.Add($sig)) {
      Move-Item -LiteralPath $job.FullName -Destination (Join-Path $Dirs.rejected $job.Name) -Force
      Write-Result $name ([ordered]@{ job = $name; exit = 'rejected'; reason = '同じジョブはこのセッションで実行済み' })
      Write-Host "[拒否] $name(再実行)" -ForegroundColor Red
      continue
    }

    $run = Join-Path $Dirs.running $job.Name
    [IO.File]::WriteAllBytes($run, [byte[]]([byte[]](0xEF, 0xBB, 0xBF) + $body))
    Remove-Item -LiteralPath $job.FullName -Force

    $text    = [Text.Encoding]::UTF8.GetString($body)
    $timeout = $DefaultTimeoutSec
    if ($text -match '(?m)^#\s*timeout:\s*(\d+)') { $timeout = [int]$Matches[1] }

    $out = Join-Path $Dirs.out $name
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $log = Join-Path $Dirs.logs "$name.log"
    $err = Join-Path $Dirs.logs "$name.err.log"
    $inner = @"
[Console]::OutputEncoding = [Text.Encoding]::UTF8
`$OutputEncoding = [Text.Encoding]::UTF8
`$ProgressPreference = 'SilentlyContinue'
`$env:JOB_OUT = '$out'
`$env:JOB_NAME = '$name'
Set-Location -LiteralPath '$Project'
try {
  & '$run' *>&1 | Out-String -Stream -Width 400
  if (`$LASTEXITCODE) { exit `$LASTEXITCODE } else { exit 0 }
} catch {
  `$_ | Out-String
  exit 1
}
"@
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($inner))

    $started = Get-Date
    Write-Heartbeat "running:$name"
    Write-Host "[実行] $name(上限 $timeout 秒)" -ForegroundColor Green
    $p = Start-Process -FilePath 'powershell.exe' `
           -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $enc) `
           -WorkingDirectory $Project -RedirectStandardOutput $log -RedirectStandardError $err `
           -NoNewWindow -PassThru
    $null = $p.Handle
    if ($p.WaitForExit($timeout * 1000)) { $p.WaitForExit(); $code = $p.ExitCode }
    else { & taskkill.exe /T /F /PID $p.Id | Out-Null; $code = 'timeout' }

    $ended = Get-Date
    $outFiles = @(Get-ChildItem -LiteralPath $out -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($Root.Length + 1) })
    $res = [ordered]@{
      job = $name; exit = $code; keyId = $KeyId
      started = $started.ToString('o'); ended = $ended.ToString('o')
      seconds = [int]($ended - $started).TotalSeconds
      outputs = $outFiles
    }
    $sum = Get-LogSummary $log $err
    foreach ($k in $sum.Keys) { $res[$k] = $sum[$k] }
    Write-Result $name $res
    Move-Item -LiteralPath $run -Destination (Join-Path $Dirs.done $job.Name) -Force
    Write-Host "[完了] $name exit=$code($($res.seconds) 秒)"
  }
} finally {
  Write-Heartbeat 'stopped'
}
