# RTX 3060 Ti 半速率锁解锁（Windows / 无 U 盘）

在 **Windows 原生环境**下解锁 RTX 3060 Ti 的 FP16/BF16/TF32 半速率限制，
全程不装 Linux、不用 U 盘启动、不刷 VBIOS、不改 NVIDIA 驱动。

基于 [`Zagitta/gpu-unlock`](https://github.com/Zagitta/gpu-unlock) 的思路与
[Windows 移植版](https://github.com/MinSeong-Cho/Windows-GPU-UNLOCK) 的构建链，
补充了本机（微星 B660M + 非公 3060 Ti）适配与运维脚本。

---

## 实测结果

图灵/安培消费卡出厂把 **FP16/BF16 输入、FP32 累加的 Tensor Core 指令发射速率**
限制为专业卡的一半。解锁后实测（8192³ GEMM，5 次中位数）：

| 精度路径 | 解锁前 | 解锁后 | 提升 |
|---|---|---|---|
| BF16 + FP32 累加 | 35.97 TFLOPS | **63.40 ~ 67.69** | **+76% ~ +88%** |
| FP16 + FP32 累加 | 35.77 TFLOPS | **64.42** | +80% |
| TF32 张量核心 | 17.99 TFLOPS | **32.83** | +82% |
| FP32（CUDA 核心） | 13.96 TFLOPS | 13.27 | 不变 |

归一化到每 SM 每时钟：**约 500 → 约 861 FLOP/clk/SM**（专业 GA102 为 1024）。

### 收益边界

| 场景 | 收益 |
|---|---|
| GEMM / 训练 / LoRA / 微调 | +76% ~ +88% |
| SD / 扩散模型推理 | 明显提升 |
| LLM 预填充（prompt 处理） | +14% ~ +36% |
| **LLM 逐 token 生成** | **≈0**（受显存带宽限制） |
| 游戏 | 无（走 CUDA 核心 FP32） |

纯 GGUF 量化推理（llama.cpp）基本没感觉 —— 瓶颈是显存带宽，本机实测 487.6 GB/s
已接近 448 GB/s 规格上限。

---

## 环境

- CPU：i5-12400F（**无核显**）
- 主板：MSI B660M MORTAR MAX WIFI DDR4
- 显卡：RTX 3060 Ti 8GB，GA104，`24c910de` / 子系统 `16fb10de`
- 系统：Windows，Secure Boot **关闭**，BitLocker 关闭
- 独立 FAT32 分区（≥1GB，本机 2GB）作内部 ESP

> 上游只验证过 5 组「主板 + 显卡」组合，本机不在其中。适配过程见 `docs/排查记录.md`。

---

## 目录

```
tools/deploy.ps1          一键部署：卷/EFI/资产/启动项/自动执行 全链路检查与修复
tools/unlock-manage.ps1   启动项管理：查看 / 启用自动 / 关闭 / 单次执行
tools/fw_boot_entry.py    直接用固件 NVRAM 变量创建 Boot#### 启动项（备用路径）
tools/gucold.c            只读冷状态探针，用于确认能否读到显卡寄存器
tools/push-via-api.py     git 协议被阻断时，改用 Git Data API 推送
docs/实战复盘.md           完整技术长文（原理→流程→踩坑→收益边界）
docs/实测结果.md           完整实测数据与部署结构
docs/排查记录.md           踩坑记录（含两个真实根因）
```

---

## 使用

> 全部命令需在**管理员 PowerShell**中执行。

### 一条命令完成部署 / 修复

```powershell
.\tools\deploy.ps1
```

它会依次检查并补齐：内部卷可访问 → 两个 EFI 存在 → 5 个固件资产哈希正确 →
固件启动项存在（缺失则自动用 `{bootmgr}` 副本重建）→ 已设为开机自动执行。

只想看状态、不改动任何东西：

```powershell
.\tools\deploy.ps1 -Check
```

### 开机自动执行（不用再按 F11）

```powershell
.\tools\unlock-manage.ps1 -Enable
```

> **注意实现方式**：不能只把解锁项排到固件启动顺序第一位。实测发现
> **Windows Boot Manager 每次启动都会把固件顺序改回自己第一**，改动一次就会被覆盖。
>
> 正确做法是注册计划任务 `GPUUnlock-ArmNextBoot`，在每次登录时用
> `bcdedit /set {fwbootmgr} bootsequence <解锁项>` **武装下一次启动**（一次性）。
> 下次开机会先进解锁，再链式启动 Windows，登录后又自动武装，形成闭环。

### 恢复默认

```powershell
.\tools\unlock-manage.ps1 -Disable
```

会删除计划任务、清除 `bootsequence`、并把固件启动顺序还原成备份值。

### 只执行一次

```powershell
.\tools\unlock-manage.ps1 -Run
```

---

## 重要事实

1. **解锁是易失的** —— 断电即失效，每次开机都要重跑一次。
2. **失败不会锁死机器** —— `BootNext` 是一次性的；`BootOrder` 不变；
   开机按 F11 始终能直接选 Windows。
3. **安全启动必须关闭**（本项目运行在预启动环境）。
4. **不修改 VBIOS、不修改驱动** —— 只在驱动绑定前改写 GPU 内部速度选择器，
   随后用 FLR 让它生效。

---

## 两个真实根因（值得一看）

排查花了很久，最后发现都不是硬件问题：

### 1. 包装层漏了 `-DGU_PROFILE_LOCAL`

`machine-profile.h` 只有定义该宏才会包含生成的 `profile-local.h`，
否则回落到**默认的 3090 身份**。于是拿 3090 的 ID 去匹配 3060 Ti，
`found=0` → 返回 `EFI_NOT_FOUND`。

### 2. 把 `0xE` 当成了 `EFI_NOT_READY`

- `EFI_NOT_READY` = `0x8000000000000006`
- `EFI_NOT_FOUND` = `0x800000000000000E`

就这一个错误码误读，让排查方向错了很久（一直以为"显卡不是冷状态"）。

### 3. 固件资产路径

5 个资产必须放在 `\RTX3090-UNLOCK\ASSETS\`（相对程序 Parent 目录），
不是分区根目录的 `\ASSETS\`。

---

## 许可与合规

- 本仓库只包含**自己编写的脚本与文档**，**不含** NVIDIA 固件、VBIOS、驱动或上游源码。
- 固件资产需自行从 NVIDIA 官方渠道获取并按上游说明生成，SHA-256 必须匹配。
- 上游 `gpu-unlock` 为 GPL-2.0；本仓库的脚本与之独立。
- 修改 GPU 内部寄存器属于非官方操作，**可能损坏硬件并失去保修**，风险自担。
