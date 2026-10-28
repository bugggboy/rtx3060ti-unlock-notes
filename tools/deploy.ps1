# 一键部署/修复 GPU Unlock 启动项
# 覆盖：卷校验 → EFI 校验 → 资产哈希校验 → 启动项自动重建 → 启用开机自动执行
# 用法：
#   .\deploy.ps1              完整部署（缺什么补什么）
#   .\deploy.ps1 -Check       只检查，不做任何修改
#   .\deploy.ps1 -DriveLetter G:   内部卷不是 F: 时指定
param(
  [switch]$Check,
  [string]$DriveLetter = 'F:'
)
$ErrorActionPreference = 'Stop'

$entryName = 'GPU Headless UNLOCK - EXPERIMENTAL'
$efiPath   = '\EFI\WindowsGPUUnlock\HEADLESS.EFI'
$childPath = '\EFI\WindowsGPUUnlock\UNLOCK-HEADLESS.EFI'
$assetsRel = 'RTX3090-UNLOCK\ASSETS'

# 5 个固件资产的期望值（大小 + SHA-256），用于完整性校验
$manifest = @(
  @{ n = 'booter_load_ga102.bin';        s = 61256;    h = '056cbc064a888f80717726c54d7f64fcd68044d123870c95a74f37f4975dcaa4' },
  @{ n = 'v67-ga102-610.43.03.bin';      s = 64000;    h = '2a0aa1f86d0b6afff5214d641d87d46dc98ef06ed908ff05d0ddbad5ffb7e197' },
  @{ n = 'gsp_ga10x.bin';                s = 84277400; h = '572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a' },
  @{ n = 'gsp_boot_ga102.bin';           s = 24576;    h = '3df651acb174fd5f5561a1996965b192f50c5d763168446ed0d4ead19d895768' },
  @{ n = 'gsp_boot_desc_ga102.bin';      s = 84;       h = 'cc5a3fc1f4c47f2fe107b1b68f0176d8a448e6dec84b575e5560ccef5262bf15' }
)

$problems = New-Object System.Collections.ArrayList
function Bad($m) { [void]$problems.Add($m); Write-Host ("  [缺] " + $m) -ForegroundColor Red }
function Ok($m)  { Write-Host ("  [ok] " + $m) -ForegroundColor Green }

function Elevate {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $p  = New-Object Security.Principal.WindowsPrincipal($id)
  if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '请以管理员身份运行 PowerShell。'
  }
}
function Bcd([string[]]$a) {
  $out = @(& bcdedit.exe @a 2>&1)
  if ($LASTEXITCODE -ne 0) { throw ("bcdedit " + ($a -join ' ') + " 失败: " + ($out -join ' ')) }
  return ($out -join "`r`n")
}
function FindEntryId {
  $all = Bcd @('/enum', 'all')
  foreach ($b in [regex]::Split($all, '(?m)^\s*[-=]{6,}\s*$')) {
    if ($b -match [regex]::Escape($entryName)) {
      $m = [regex]::Match($b, '(?mi)^\s*(?:标识符|identifier)\s+(\{[0-9a-fA-F-]{36}\})')
      if ($m.Success) { return $m.Groups[1].Value }
    }
  }
  return $null
}

Write-Host ''
Write-Host '=== GPU Unlock 部署检查 ===' -ForegroundColor Cyan
Elevate

# 1. 卷
Write-Host "`n[1/5] 内部卷"
$vol = $DriveLetter.TrimEnd('\')
if (-not (Test-Path -LiteralPath ($vol + '\'))) { Bad ("挂载点不存在: " + $vol); throw '内部卷不可用，请确认分区已挂载并分配盘符。' }
Ok ($vol + " 可访问")

# 2. EFI 文件
Write-Host "`n[2/5] EFI 程序"
foreach ($f in @($efiPath, $childPath)) {
  $full = $vol + $f.Replace('\', '\')
  if (Test-Path -LiteralPath $full) {
    $len = (Get-Item -LiteralPath $full).Length
    Ok ($f + "  (" + $len + " 字节)")
  } else { Bad ($f + " 不存在") }
}

# 3. 资产
Write-Host "`n[3/5] 固件资产"
$assetDir = Join-Path $vol $assetsRel
if (-not (Test-Path -LiteralPath $assetDir)) {
  Bad ("资产目录不存在: " + $assetDir)
} else {
  foreach ($a in $manifest) {
    $p = Join-Path $assetDir $a.n
    if (-not (Test-Path -LiteralPath $p)) { Bad ($a.n + " 缺失"); continue }
    $len = (Get-Item -LiteralPath $p).Length
    if ($len -ne $a.s) { Bad ($a.n + " 大小不符（" + $len + " != " + $a.s + "）"); continue }
    $h = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($h -ne $a.h) { Bad ($a.n + " 哈希不符"); continue }
    Ok ($a.n)
  }
}

# 4. 启动项
Write-Host "`n[4/5] 固件启动项"
$entryId = FindEntryId
if ($entryId) {
  Ok ("已存在: " + $entryId)
} else {
  if ($Check) {
    Bad ('启动项 "' + $entryName + '" 不存在')
  } else {
    Write-Host '  正在重建（复制 {bootmgr} 条目）...'
    $out = Bcd @('/copy', '{bootmgr}', '/d', $entryName)
    $m = [regex]::Match($out, '\{[0-9a-fA-F-]{36}\}')
    if (-not $m.Success) { throw '无法解析新条目 GUID。' }
    $entryId = $m.Value
    [void](Bcd @('/set', $entryId, 'device', ('partition=' + $vol)))
    [void](Bcd @('/set', $entryId, 'path', $efiPath))
    Ok ("已创建: " + $entryId)
  }
}

# 5. 自动执行
Write-Host "`n[5/5] 开机自动执行"
if ($entryId) {
  $txt = Bcd @('/enum', '{fwbootmgr}')
  $first = ([regex]::Match($txt, '(?m)^\s*(?:displayorder|显示顺序)\s+(\{[^}]+\})')).Groups[1].Value
  if ($first -eq $entryId) {
    Ok '已启用（解锁项在固件启动顺序第一位）'
  } else {
    if ($Check) {
      Bad '未启用自动执行'
    } else {
      & (Join-Path $PSScriptRoot 'unlock-manage.ps1') -Enable | Out-Null
      Ok '已启用自动执行'
    }
  }
}

Write-Host ""
if ($problems.Count -eq 0) {
  Write-Host '=== 全部就绪 ===' -ForegroundColor Green
  Write-Host '下次完全关机再开机，会自动执行解锁并进入 Windows。' -ForegroundColor Gray
  exit 0
} else {
  Write-Host ('=== 发现 ' + $problems.Count + ' 个问题 ===') -ForegroundColor Yellow
  foreach ($p in $problems) { Write-Host ('  - ' + $p) }
  if ($Check) { Write-Host '（这是检查模式，未做任何修改）' -ForegroundColor Gray }
  exit 1
}
