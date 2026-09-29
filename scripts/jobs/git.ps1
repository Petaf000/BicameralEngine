# git はこのジョブ経由でだけ実行する(Linux 側では status も含めて実行しない)。
#   job.py git status --short
#   job.py git add -A
#   job.py git commit -m "T-0001: caps プローブ"
#   job.py git log --oneline -n 10
#   job.py git push                   (通常の push だけ。force・削除は拒否)
# 破壊的な操作は拒否する。必要ならユーザーが自分で実行する。
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$GitArgs)
$deny = @('push --force', 'push -f', 'reset --hard', 'clean -f', 'clean -fd', 'clean -xdf', 'branch -D', 'checkout -- .', 'restore .', 'filter-branch', 'reflog expire', 'gc --prune')
$joined = ($GitArgs -join ' ')
foreach ($d in $deny) {
  if ($joined -like "*$d*") { Write-Host "拒否: '$d' は Claude からは実行しない。必要ならユーザーが手で実行する。"; exit 5 }
}
# push は origin への通常の push だけ許す(2026-09-29 ユーザー決定: 公開リポジトリへ Claude が push してよい。ADR-0005)。
# 履歴を書き換える・消す push は拒否する
if ($GitArgs.Count -gt 0 -and $GitArgs[0] -eq 'push') {
  foreach ($a in $GitArgs) {
    if ($a -match '^(--force|--force-with-lease|--delete|--mirror|--prune|-d)' -or $a -match '^\+' -or $a -match '^:') {
      Write-Host "拒否: '$a' を含む push は Claude からは実行しない。必要ならユーザーが手で実行する。"; exit 5
    }
  }
}
git -c core.quotepath=false -c color.ui=false @GitArgs
exit $LASTEXITCODE
