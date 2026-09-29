# git はこのジョブ経由でだけ実行する(Linux 側では status も含めて実行しない)。
#   job.py git status --short
#   job.py git add -A
#   job.py git commit -m "T-0001: caps プローブ"
#   job.py git log --oneline -n 10
# 破壊的な操作は拒否する。必要ならユーザーが自分で実行する。
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$GitArgs)
$deny = @('push --force', 'push -f', 'reset --hard', 'clean -f', 'clean -fd', 'clean -xdf', 'branch -D', 'checkout -- .', 'restore .', 'filter-branch', 'reflog expire', 'gc --prune')
$joined = ($GitArgs -join ' ')
foreach ($d in $deny) {
  if ($joined -like "*$d*") { Write-Host "拒否: '$d' は Claude からは実行しない。必要ならユーザーが手で実行する。"; exit 5 }
}
if ($GitArgs.Count -gt 0 -and $GitArgs[0] -eq 'push') { Write-Host '拒否: push はユーザーが行う(公開タイミングはユーザーが決める)'; exit 5 }
git -c core.quotepath=false -c color.ui=false @GitArgs
exit $LASTEXITCODE
