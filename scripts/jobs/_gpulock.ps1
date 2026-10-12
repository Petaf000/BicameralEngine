# GPU の順番待ち(2026-10-12 ユーザー決定): 同じ PC の全部のランナー(本体・各作業ツリー)で、GPU を使う仕事を 1 本ずつにする。
# ビルドと CPU だけのテストは並走のまま。名前付きミューテックス(同じログオンのセッションで共有)で待つ。
# 持っていたジョブが殺されると放棄されたミューテックスになるが、その時は取れたものとして扱う。
$script:GpuLockName = 'Local\BicameralEngineGpu'
$script:GpuLock = $null
$script:GpuWaitTotalSec = 0

# 順番待ちの秒をランナーに知らせる(ランナーはジョブの上限からこの分を除く。T-0426)。
# 書く形は「待ち終えた秒の合計 [待ち始めた時刻(UNIX 秒)]」。$env:JOB_GPU_WAIT が無い(古いランナー)なら何もしない
function Write-GpuWaitFile([string]$Since) {
  if (-not $env:JOB_GPU_WAIT) { return }
  try { [IO.File]::WriteAllText($env:JOB_GPU_WAIT, ("$script:GpuWaitTotalSec $Since").Trim()) } catch { }
}

# GPU を使うテストの名前(ctest の名前)。CPU だけのテストは待たない
$script:GpuTestPattern = '^(gpu_|window_|image_)'

function Enter-GpuLock([string]$What) {
  $script:GpuLock = New-Object System.Threading.Mutex($false, $script:GpuLockName)
  $start = Get-Date
  Write-Host "[gpu] 順番待ち: $What"
  Write-GpuWaitFile ([string][DateTimeOffset]::UtcNow.ToUnixTimeSeconds())
  try {
    [void]$script:GpuLock.WaitOne()
  } catch [System.Threading.AbandonedMutexException] {
    Write-Host '[gpu] 前の持ち主が終わらずに消えた(取れたものとして続ける)'
  }
  $waited = [int]((Get-Date) - $start).TotalSeconds
  $script:GpuWaitTotalSec += $waited
  Write-GpuWaitFile ''
  Write-Host "[gpu] 取れた(待った $waited 秒): $What"
}

function Exit-GpuLock {
  if ($null -eq $script:GpuLock) { return }
  try { $script:GpuLock.ReleaseMutex() } catch { }
  $script:GpuLock.Dispose()
  $script:GpuLock = $null
}
