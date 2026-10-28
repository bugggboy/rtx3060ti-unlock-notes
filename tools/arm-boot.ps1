# 武装下一次开机：把解锁项设为一次性启动目标（bootsequence）
# 由计划任务在每次登录时调用。Windows 启动时会把固件顺序改回自己第一，
# 所以不能靠改 displayorder，必须在 Windows 里重新武装下一次。
$ErrorActionPreference = 'Stop'
$entryName = 'GPU Headless UNLOCK - EXPERIMENTAL'

function Bcd([string[]]$a) {
  $out = @(& bcdedit.exe @a 2>&1)
  if ($LASTEXITCODE -ne 0) { throw ("bcdedit " + ($a -join ' ') + " 失败: " + ($out -join ' ')) }
  return ($out -join "`r`n")
}

# 找到解锁条目
$entryId = $null
$all = Bcd @('/enum', 'all')
foreach ($b in [regex]::Split($all, '(?m)^\s*[-=]{6,}\s*$')) {
  if ($b -match [regex]::Escape($entryName)) {
    $m = [regex]::Match($b, '(?mi)^\s*(?:标识符|identifier)\s+(\{[0-9a-fA-F-]{36}\})')
    if ($m.Success) { $entryId = $m.Groups[1].Value; break }
  }
}
if (-not $entryId) { throw '找不到解锁启动项，请先运行 deploy.ps1。' }

# 确认它登记在固件启动顺序里（bootsequence 需要是固件可见项）
$fw = Bcd @('/enum', '{fwbootmgr}')
if ($fw -notmatch [regex]::Escape($entryId)) {
  $ids = @([regex]::Matches($fw, '\{(?:bootmgr|[0-9a-fA-F-]{36})\}') | ForEach-Object { $_.Value } | Select-Object -Unique)
  [void](Bcd (@('/set', '{fwbootmgr}', 'displayorder') + $ids + @($entryId)))
}

# 设为下一次启动目标
[void](Bcd @('/set', '{fwbootmgr}', 'bootsequence', $entryId))

# 校验
$after = Bcd @('/enum', '{fwbootmgr}')
$m2 = [regex]::Match($after, '(?mi)^\s*bootsequence\s+(\{[0-9a-fA-F-]{36}\})')
if (-not $m2.Success -or $m2.Groups[1].Value -ne $entryId) {
  throw 'bootsequence 写入后校验失败。'
}
