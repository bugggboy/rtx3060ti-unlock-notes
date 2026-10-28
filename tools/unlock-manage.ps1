# GPU Unlock 启动项管理（本机脚本，不含任何 NVIDIA 二进制）
# 用法：
#   .\unlock-manage.ps1 -Status     查看当前状态
#   .\unlock-manage.ps1 -Enable     开机自动执行解锁（不用再按 F11）
#   .\unlock-manage.ps1 -Disable    恢复为默认（开机直接进 Windows）
#   .\unlock-manage.ps1 -Run        仅下次开机执行一次（一次性）
param(
  [switch]$Status,
  [switch]$Enable,
  [switch]$Disable,
  [switch]$Run
)
$ErrorActionPreference = 'Stop'
$entryName = 'GPU Headless UNLOCK - EXPERIMENTAL'
$efiPath   = '\EFI\WindowsGPUUnlock\HEADLESS.EFI'
$stateDir  = Join-Path $PSScriptRoot 'state'
$stateFile = Join-Path $stateDir 'bootstate.json'

function Bcd([string[]]$a) {
  $out = @(& bcdedit.exe @a 2>&1)
  if ($LASTEXITCODE -ne 0) { throw ("bcdedit " + ($a -join ' ') + " 失败: " + ($out -join ' ')) }
  return ($out -join "`r`n")
}

function Elevate {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $p  = New-Object Security.Principal.WindowsPrincipal($id)
  if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '请以管理员身份运行 PowerShell 后再执行本脚本。'
  }
}

function FindEntryId {
  $all = Bcd @('/enum', 'all')
  $blocks = [regex]::Split($all, '(?m)^\s*[-=]{6,}\s*$')
  foreach ($b in $blocks) {
    if ($b -match [regex]::Escape($entryName)) {
      $m = [regex]::Match($b, '(?mi)^\s*(?:标识符|identifier)\s+(\{[0-9a-fA-F-]{36}\})')
      if ($m.Success) { return $m.Groups[1].Value }
    }
  }
  return $null
}

function FirmwareOrder {
  $txt   = Bcd @('/enum', '{fwbootmgr}')
  $lines = $txt -split "\r?\n"
  $ids   = New-Object System.Collections.ArrayList
  $in    = $false
  foreach ($ln in $lines) {
    if ($ln -match '(?i)^\s*(?:displayorder|显示顺序)\s*(.*)$') {
      $in = $true
      foreach ($g in [regex]::Matches($Matches[1], '\{(?:bootmgr|[0-9a-fA-F-]{36})\}')) { [void]$ids.Add($g.Value) }
      continue
    }
    if ($in) {
      $t = $ln.Trim()
      if ($t.StartsWith('{')) {
        foreach ($g in [regex]::Matches($t, '\{(?:bootmgr|[0-9a-fA-F-]{36})\}')) { [void]$ids.Add($g.Value) }
      } else { $in = $false }
    }
  }
  return @($ids.ToArray())
}

function SaveState($obj) {
  if (-not (Test-Path -LiteralPath $stateDir)) { New-Item -ItemType Directory -Path $stateDir | Out-Null }
  $obj | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $stateFile -Encoding utf8
}

function LoadState {
  if (Test-Path -LiteralPath $stateFile) {
    return Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
  }
  return $null
}

function ShowStatus($entryId, $order) {
  Write-Host ''
  Write-Host '=== GPU Unlock 启动状态 ===' -ForegroundColor Cyan
  if (-not $entryId) {
    Write-Host '启动项: 不存在（需要重新注册）' -ForegroundColor Red
  } else {
    Write-Host ("启动项: " + $entryId)
    $d = Bcd @('/enum', $entryId)
    $dev  = ([regex]::Match($d, '(?mi)^\s*(?:device|设备)\s+(.+)$')).Groups[1].Value.Trim()
    $path = ([regex]::Match($d, '(?mi)^\s*(?:path|路径)\s+(.+)$')).Groups[1].Value.Trim()
    Write-Host ("  设备: " + $dev)
    Write-Host ("  路径: " + $path)
    if ($path -ne $efiPath) { Write-Host '  警告: 路径与预期不一致' -ForegroundColor Yellow }
  }
  Write-Host ("固件启动顺序: " + ($order -join ' '))
  $taskMode = Get-ScheduledTask -TaskName 'GPUUnlock-ArmNextBoot' -ErrorAction SilentlyContinue
  if ($entryId -and $taskMode) {
    Write-Host '模式: 自动（每次登录武装下一次开机，开机先解锁）' -ForegroundColor Green
  } else {
    Write-Host '模式: 手动（开机按 F11 选择执行）' -ForegroundColor Yellow
  }
  $fwtxt = Bcd @('/enum', '{fwbootmgr}')
  $bs = [regex]::Match($fwtxt, '(?mi)^\s*bootsequence\s+(\{[0-9a-fA-F-]{36}\})')
  if ($bs.Success) { Write-Host ("下次启动目标: " + $bs.Groups[1].Value + "（一次性）") } else { Write-Host '下次启动目标: 未武装' }

  $s = LoadState
  if ($s -and $s.OriginalOrder) {
    Write-Host ("已保存的原始顺序: " + ($s.OriginalOrder -join ' '))
  }
  Write-Host ''
}

Elevate
$entryId = FindEntryId
$order   = FirmwareOrder

if (-not ($Status -or $Enable -or $Disable -or $Run)) { $Status = $true }

if ($Status) {
  ShowStatus $entryId $order
  exit 0
}

if (-not $entryId) { throw ('找不到名为 "' + $entryName + '" 的启动项，请先重新注册（见 README）。') }

if ($Run) {
  [void](Bcd @('/set', '{fwbootmgr}', 'bootsequence', $entryId))
  Write-Host '已设置：下次开机执行一次解锁，之后恢复默认。' -ForegroundColor Green
  exit 0
}

if ($Enable) {
  # Windows 每次启动都会把固件顺序改回自己第一，所以不能只改 displayorder，
  # 必须注册一个登录时执行的任务，在 Windows 内重新武装下一次启动。
  $state = LoadState
  if (-not $state -or -not $state.OriginalOrder) {
    SaveState ([ordered]@{ EntryId = $entryId; OriginalOrder = $order; SavedUtc = (Get-Date).ToUniversalTime().ToString('o') })
    $state = LoadState
  }
  $armScript = Join-Path $PSScriptRoot 'arm-boot.ps1'
  if (-not (Test-Path -LiteralPath $armScript)) { throw ('缺少 ' + $armScript) }
  $act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $armScript + '"')
  $trg = New-ScheduledTaskTrigger -AtLogOn
  $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
  $prn = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Highest
  Register-ScheduledTask -TaskName 'GPUUnlock-ArmNextBoot' -Action $act -Trigger $trg -Settings $set -Principal $prn -Description '每次登录时把解锁项设为下一次启动目标' -Force | Out-Null
  & $armScript
  Write-Host '已启用自动模式：每次登录会武装下一次开机，开机时先解锁再进 Windows。' -ForegroundColor Green
  Write-Host '如需取消，运行：.\unlock-manage.ps1 -Disable' -ForegroundColor Gray
  exit 0
}

if ($Disable) {
  $t = Get-ScheduledTask -TaskName 'GPUUnlock-ArmNextBoot' -ErrorAction SilentlyContinue
  if ($t) { Unregister-ScheduledTask -TaskName 'GPUUnlock-ArmNextBoot' -Confirm:$false; Write-Host '已删除自动武装任务。' }
  try { [void](Bcd @('/deletevalue', '{fwbootmgr}', 'bootsequence')); Write-Host '已清除下次启动目标。' } catch { }
  $state = LoadState
  if ($state -and $state.OriginalOrder) {
    [void](Bcd (@('/set', '{fwbootmgr}', 'displayorder') + @($state.OriginalOrder)))
    Write-Host '已恢复原始固件启动顺序。'
  } else {
    Write-Host '没有保存的原始顺序，displayorder 未改动。' -ForegroundColor Yellow
  }
  Write-Host '已恢复默认：开机直接进入 Windows。' -ForegroundColor Green
  exit 0
}
