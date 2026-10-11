# GPU の順番待ち(2026-10-12 ユーザー決定): 同じ PC の全部のランナー(本体・各作業ツリー)で、GPU を使う仕事を 1 本ずつにする。
# ビルドと CPU だけのテストは並走のまま。名前付きミューテックス(同じログオンのセッションで共有)で待つ。
# 持っていたジョブが殺されると放棄されたミューテックスになるが、その時は取れたものとして扱う。
$script:GpuLockName = 'Local\BicameralEngineGpu'
$script:GpuLock = $null

# GPU を使うテストの名前(ctest の名前)。CPU だけのテストは待たない
$script:GpuTestPattern = '^(gpu_|window_|image_)'

function Enter-GpuLock([string]$What) {
  $script:GpuLock = New-Object System.Threading.Mutex($false, $script:GpuLockName)
  $start = Get-Date
  Write-Host "[gpu] 順番待ち: $What"
  try {
    [void]$script:GpuLock.WaitOne()
  } catch [System.Threading.AbandonedMutexException] {
    Write-Host '[gpu] 前の持ち主が終わらずに消えた(取れたものとして続ける)'
  }
  $waited = [int]((Get-Date) - $start).TotalSeconds
  Write-Host "[gpu] 取れた(待った $waited 秒): $What"
}

function Exit-GpuLock {
  if ($null -eq $script:GpuLock) { return }
  try { $script:GpuLock.ReleaseMutex() } catch { }
  $script:GpuLock.Dispose()
  $script:GpuLock = $null
}
