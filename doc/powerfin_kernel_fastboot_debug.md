# PowerFin 内核快速启动调试方案

## 1. 目标

本文档定义 PowerFin 调试 Linux 内核启动速度时的推荐工作流：

- SPI NOR 只保存稳定的 SPL、OP-TEE、U-Boot 和分区表；
- 调试期间不反复擦写 SPI NOR；
- SD 卡始终插在 PowerFin 上，不需要反复插拔；
- 通过 U-Boot USB Mass Storage（UMS）在主机上更新 SD 卡里的内核镜像；
- 使用极简 initramfs，不依赖完整 Buildroot rootfs；
- 分开测量 BootROM/SPL、U-Boot、Kernel 和 `/init` 各阶段耗时。

推荐启动链：

```text
SPI NOR
  └─ BootROM → SPL → OP-TEE → U-Boot
                              │
                              └─ 从 SD/FAT 加载 powerfin-ramboot.img
                                                   │
RAM                                                ├─ zImage
                                                   ├─ PowerFin DTB
                                                   └─ 极简 initramfs
```

当前已经验证：

- SPI NOR 为 Puya PY25Q128HA，JEDEC ID `85 20 18`，容量 16 MiB；
- SPL 能从 SPI NOR 加载并校验 OP-TEE、U-Boot 和 U-Boot DTB；
- PowerFin SPL 启动顺序已设为 SPI NOR 优先、SD 备用，插卡不会先由 SPL 占用；
- U-Boot 能正确识别 16 MiB SPI NOR；
- SPI NOR 中的 U-Boot 可以进入命令行；
- 当前 U-Boot 已启用 MMC、FAT、EXT4、`bootm` 和 USB Gadget；
- 当前 U-Boot 已完成 `ums` 主机写入、卸载、板端回读验证；
- U-Boot 已实测识别 14.8 GiB SD 卡，4-bit 总线，传输速率 52 MHz。

## 2. 方案选择

### 2.1 推荐：U-Boot UMS + SD 卡 + FIT initramfs

日常迭代流程：

```text
修改 Kernel/DTS
  → 编译 zImage/DTB
  → 打包 powerfin-ramboot.img
  → U-Boot 执行 ums，将板载 SD 映射给主机
  → 主机替换 SD 卡中的 powerfin-ramboot.img
  → 安全卸载主机文件系统
  → U-Boot 从 SD 加载到 RAM并启动
```

优点：

- 不写 SPI NOR；
- 不插拔 SD 卡；
- FIT 将 kernel、DTB、initramfs 合并为一个文件，加载命令简单；
- USB 传输 6～10 MiB 镜像比串口传输快得多；
- 极简 initramfs 启动过程稳定，适合测量 Kernel 自身耗时。

### 2.2 不推荐作为主流程

- `upgrade_tool di -b boot.img`：操作简单，但每次都会擦写 SPI NOR boot 分区；
- 完整 Buildroot rootfs：体积和用户空间服务较多，会混入与 Kernel 无关的耗时；
- 无 rootfs 直接启动：内核最终会因找不到 `/init` 或根文件系统而 panic；
- 串口 YMODEM：当前 U-Boot 未启用 `loadx/loady/loadb`，且大镜像传输慢；
- TFTP：当前 U-Boot 启动日志显示 `No ethernet found`，暂时不可用。

## 3. 一次性 U-Boot 准备

在 `u-boot/configs/rk3506_luckfox_defconfig` 中启用：

```config
CONFIG_CMD_USB_MASS_STORAGE=y
```

PowerFin 使用独立的 U-Boot 配置片段和设备树：

```text
u-boot/configs/powerfin_spinor.config
u-boot/arch/arm/dts/rk3506-powerfin.dts
u-boot/arch/arm/dts/rk3506-powerfin-u-boot.dtsi
```

其中 SPL 启动顺序为：

```dts
u-boot,spl-boot-order = &spi_nor, &mmc;
```

这样 BootROM/SPL 固定优先从 NOR 加载 U-Boot，SD 留给 U-Boot 加载调试内核。不要把 SD 放在 NOR 前面，否则 SPL 探测过 SD 后，当前控制器可能在正式 U-Boot 中重新初始化失败。

当前配置已经具备相关基础能力：

```config
CONFIG_MMC=y
CONFIG_CMD_MMC=y
CONFIG_FS_FAT=y
CONFIG_CMD_FAT=y
CONFIG_CMD_EXT4=y
CONFIG_CMD_FS_GENERIC=y
CONFIG_CMD_BOOTM=y
CONFIG_USB_GADGET=y
CONFIG_USB_GADGET_DOWNLOAD=y
```

修改后执行：

```bash
./build.sh powerfin_buildroot_spinor_defconfig
./build.sh uboot
./build.sh firmware
```

只需要再烧录一次 Loader/U-Boot。确认新 U-Boot 中存在 `ums` 命令后，后续内核调试不再写 SPI NOR。

> 2026-07-20 已完成 Loader/U-Boot 编译和 SPI NOR 烧录，U-Boot 分区回读哈希一致；已验证 NOR 优先启动，以及 UMS 主机写入、安全卸载、U-Boot FAT 回读的完整链路。

## 4. 调试 SD 卡布局

推荐准备一个 FAT32 分区，不要求完整 rootfs：

```text
SD card
└─ partition 1: FAT32
   └─ powerfin-ramboot.img
```

如果 SD 卡已经有多个分区，只要 U-Boot 能访问其中一个 FAT/EXT4 分区即可。

如果主机显示 MBR/FAT32 正常，但 U-Boot 报 GPT CRC 错误、分区列表为空，通常是卡上残留了旧 GPT 主头和备份头。不要让 U-Boot 反复自动修复这份无效 GPT。应先卸载文件系统，再用主机分区工具清除旧签名并重建分区表；如果需要保留现有 MBR/FAT，也可以只清除 LBA 1～33 和磁盘末尾 33 个扇区。清理后应看到：

```text
Partition Type: DOS
Part 1 ... Type 0c
```

第一次进入 U-Boot 后确认 MMC 编号：

```text
mmc list
mmc dev 0
mmc rescan
fatls mmc 0:1
```

如果 `mmc 0` 不是外置 SD 卡，则尝试：

```text
mmc dev 1
mmc rescan
fatls mmc 1:1
```

后续文档中的 `<mmc-dev>` 和 `<part>` 必须替换为实测值，例如 `0` 和 `1`。

## 5. 使用 UMS 更新 SD 卡

在 U-Boot 命令行执行：

```text
ums 0 mmc <mmc-dev>
```

例如外置 SD 为 `mmc 0`：

```text
ums 0 mmc 0
```

此时主机会出现新的 USB Mass Storage 块设备。主机挂载 FAT 分区并更新镜像：

```bash
cp output/powerfin-ramboot.img /mnt/powerfin-sd/powerfin-ramboot.img
sync
```

复制完成后必须执行：

```bash
umount /mnt/powerfin-sd
```

本次实测 UMS USB ID 为 `2207:0010`，Windows 设备名为 `Linux UMS disk 0`。它与 Maskrom/Loader 的 `2207:350f` 是不同的 USB 身份，因此需要分别处理 usbipd 共享/附加状态。

确认主机已卸载文件系统后，才能在 UART 的 U-Boot 终端按 `Ctrl+C` 退出 UMS。

禁止在以下两种情况下同时访问 SD 卡：

- 主机仍挂载 SD 文件系统时，让 U-Boot 执行 `fatload`；
- U-Boot 正在读写 SD 时，让主机重新挂载 UMS 磁盘。

否则可能损坏 FAT 文件系统或得到未完整写入的镜像。

### 5.1 WSL2 注意事项

PowerFin 在 Maskrom、RAM Loader、U-Boot Rockusb 和 UMS 之间切换时会重新枚举 USB。即使 VID/PID 仍为 `2207:350f`，Windows 和 WSL 也可能把它当作一次新的连接。

先在管理员 PowerShell 中查看 BUSID：

```powershell
usbipd list
```

第一次使用时共享设备：

```powershell
usbipd bind --busid <BUSID>
```

附加到 WSL，并在设备重新枚举后自动重新附加：

```powershell
usbipd attach --wsl --busid <BUSID> --auto-attach
```

`--auto-attach` 会持续监控该 BUSID，运行它的 PowerShell 窗口应保持打开。如果执行普通 `attach`，从 Maskrom 下载 Loader 后设备经常会从 WSL 消失，需要再次 attach。

也可以从 WSL 调用 Windows usbipd：

```bash
powershell.exe -NoProfile -Command "usbipd list"
powershell.exe -NoProfile -Command \
    "usbipd attach --wsl --busid <BUSID> --auto-attach"
```

在 WSL 中确认 Rockchip USB 和 UART：

```bash
lsusb
ls -l /dev/ttyUSB* /dev/serial/by-id/*
```

本次实测设备：

```text
Rockchip USB: 2207:350f
UART0:       CH340 1a86:7523，/dev/ttyUSB0
UART0 baud:  1500000
```

Rockchip USB 节点必须允许当前用户读写。若 `upgrade_tool ld` 能枚举，但 `db`、`td` 或写入操作报 `Creating Comm Object failed`，检查：

```bash
ls -l /dev/bus/usb/*/*
```

推荐创建持久 udev 规则，而不是只对动态设备路径执行 `chmod`：

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="2207", MODE="0666"' |
    sudo tee /etc/udev/rules.d/99-rockchip.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

然后重新插拔 Rockchip USB，或重新进入 Maskrom。设备号 `/dev/bus/usb/001/003` 中的最后一段会随重新枚举改变，不能写死。

UART 通常属于 `dialout` 组：

```bash
id
ls -l /dev/ttyUSB0
```

如当前用户不在 `dialout` 组，需要在主机侧处理用户组权限并重新登录。

UMS 启动后，在 WSL 中确认新块设备：

```bash
lsusb
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINTS
```

必须根据 `lsblk` 结果确认设备名，不能把文档中的示例设备名直接用于 `mount`、`mkfs` 或 `dd`。

若卡的分区表暂时损坏，`ums 0 mmc 0` 可能在真正启动 USB Gadget 前报 `Invalid partition 1`。诊断时可用下面的命令显式导出整卡：

```text
ums 0 mmc 0:0
```

修复分区表后恢复使用普通的 `ums 0 mmc 0`。

如果希望由 Windows 资源管理器直接挂载 UMS，而不是 WSL，应先停止自动附加或执行 detach，让 USB Mass Storage 留在 Windows 侧。Windows 和 WSL 不能同时独占同一个 USB 设备。

### 5.2 Maskrom、Loader、Rockusb 和 UMS 模式

这些模式名称容易混淆：

| 状态 | 运行主体 | 是否驻留 Flash | `upgrade_tool ld` 常见显示 | 用途 |
|---|---|---:|---|---|
| Maskrom | BootROM | 否 | `Mode=Maskrom` | 空片恢复、下载 RAM Loader |
| RAM Loader | MiniLoader | 否 | 可能仍显示 `Mode=Maskrom`，Serial 为 `rockchip` | 识别 Flash、读分区、烧写镜像 |
| U-Boot Rockusb | U-Boot gadget | 否 | `Mode=Loader` | U-Boot USB 下载服务 |
| UMS | U-Boot mass-storage gadget | 否 | 不一定被 `upgrade_tool` 识别 | 将 SD 映射成主机磁盘 |
| 正常启动 | SPI NOR SPL/U-Boot | 是 | 通常没有 Rockusb 设备 | 从 NOR 启动并进入 U-Boot/Linux |

#### 从 U-Boot 返回 Maskrom

在 U-Boot 命令行执行：

```text
rbrom
```

设备会重新枚举为 BootROM Maskrom。使用 usbipd 时等待自动重新附加，然后确认：

```bash
upgrade_tool ld
```

预期：

```text
Mode=Maskrom
```

如果当前正运行 U-Boot Rockusb 或 UMS，先在 UART 按 `Ctrl+C` 退出，再执行 `rbrom`：

```text
Ctrl+C
rbrom
```

若 U-Boot 无法进入命令行，则使用 PowerFin 的硬件强制 Maskrom 方法。强制进入时应先确认板级原理图和操作说明，不要猜测或短接未知引脚。Flash 中没有有效 Loader 时，BootROM 通常也会自动停留在 Maskrom。

#### 从 Maskrom 进入临时 RAM Loader

```bash
upgrade_tool db rockdev/MiniLoaderAll.bin
```

成功提示：

```text
Download boot ok.
```

这条命令只把 MiniLoader 下载到 RAM，不写 SPI NOR。UART 会出现 DDR 初始化、Flash ID 和 `UsbBoot` 日志。设备会重新枚举，可能需要等待 usbipd 自动重新附加。

随后用实际能力判断 RAM Loader 是否工作：

```bash
upgrade_tool rfi
upgrade_tool pl
```

本板预期 Flash 信息：

```text
Flash Size: 16MB
Flash CS: <0>
```

不要只依赖 `upgrade_tool ld` 的 Mode 字段：本次实测 RAM Loader 已正常响应 `rfi/pl` 时，`ld` 仍可能显示 `Mode=Maskrom`。

#### 持久更新 Loader

只有确实需要更新 SPL/idblock 时才执行：

```bash
upgrade_tool ul rockdev/MiniLoaderAll.bin -noreset SPINOR
```

它会写 SPI NOR，不是临时操作。`-noreset` 让设备保持下载状态，便于继续写 parameter、U-Boot 或其他分区。

#### 从正常 U-Boot进入 U-Boot Rockusb

PowerFin SPI NOR 对应当前 U-Boot 的 `mtd 2`：

```text
rockusb 0 mtd 2
```

主机通常显示：

```text
Mode=Loader
```

退出方式：

```text
Ctrl+C
```

当前 RK3506 U-Boot Rockusb gadget 可以进入 Loader 模式，但实测 `upgrade_tool pl`、按分区名写入和原始 LBA 写入可能因无法读取 Flash ID/GPT 而失败。因此不要把它作为更新 U-Boot 的可靠路径。

需要更新 U-Boot 时使用：

```text
U-Boot: Ctrl+C → rbrom
Host:   upgrade_tool db MiniLoaderAll.bin
Host:   upgrade_tool rfi && upgrade_tool pl
Host:   upgrade_tool di -uboot uboot.img
Host:   upgrade_tool rd
```

即使用 RAM Loader 进行实际 Flash 写入。

#### 从 RAM Loader恢复正常启动

```bash
upgrade_tool rd
```

设备复位并从 SPI NOR重新启动。如果 `rd` 后 Rockchip USB 从 WSL 消失但 UART 已开始输出 SPL/U-Boot 日志，这是正常现象：正常启动模式不会持续提供 Rockusb gadget。

#### 进入和退出 UMS

在 U-Boot 中进入：

```text
ums 0 mmc <mmc-dev>
```

主机完成复制后：

```bash
sync
umount /mnt/powerfin-sd
```

然后在 UART 按 `Ctrl+C` 退出 UMS。必须先卸载主机文件系统，不能先退出 UMS。

#### UART0 非交互控制

UART0 使用 1500000 波特率：

```bash
stty -F /dev/ttyUSB0 1500000 cs8 -cstopb -parenb \
    raw -echo -crtscts -ixon -ixoff -opost
```

发送 U-Boot 命令示例：

```bash
printf 'help ums\r' >/dev/ttyUSB0
```

退出 Rockusb/UMS 后返回 Maskrom：

```bash
printf '\003\rrbrom\r' >/dev/ttyUSB0
```

其中 `\003` 是 `Ctrl+C`。同一时间只运行一个 UART 读取程序，避免多个终端争抢串口数据。

## 6. 极简 initramfs

### 6.1 内容

建议使用静态链接 BusyBox，initramfs 只包含：

```text
init
bin/busybox
dev/console
dev/null
proc/
sys/
tmp/
```

示例 `/init`：

```sh
#!/bin/busybox sh

/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev

echo "POWERFIN_INIT_READY"
cat /proc/uptime

exec /bin/busybox sh
```

内核已经启用：

```config
CONFIG_BLK_DEV_INITRD=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
```

当前实现使用 SDK 已有 Buildroot ARM 工具链，单独编译静态 BusyBox，不构建完整 Buildroot rootfs。相关输入为：

```text
tools/powerfin_ramboot/busybox-minimal.config
tools/powerfin_ramboot/init
tools/powerfin_ramboot/initramfs.list.in
```

`repack_powerfin_ramboot.sh` 会自动完成 BusyBox、initramfs 和 FIT 打包。它至少满足：

- ARM32 RK3506 工具链；
- BusyBox 静态链接；
- 输出 `rootfs.cpio.gz`；
- 不启用网络、SSH、udev、音频、图形、字体和额外服务；
- `/init` 直接作为 PID 1；
- 压缩后的 initramfs 尽量控制在 2 MiB以内；当前实测为 426 KiB。

由于 ramdisk 使用 gzip，PowerFin 内核配置必须启用：

```config
CONFIG_RD_GZIP=y
```

### 6.2 为什么不使用完整 recovery

SDK 现有 recovery ramboot 包含较完整的恢复系统，适合维修，不适合测量纯内核启动时间。它只作为打包流程参考；当前使用 PowerFin 专用的静态 BusyBox 配置和 initramfs 清单。

## 7. FIT ramboot 镜像

SDK 已提供 FIT ramdisk 模板和打包脚本：

- `device/rockchip/.chips/rk3506/zboot4recovery.its`
- `device/rockchip/common/scripts/mk-ramboot.sh`

当前已新增：

```text
device/rockchip/.chips/rk3506/powerfin-ramboot.its
repack_powerfin_ramboot.sh
```

最终 `powerfin-ramboot.img` 包含：

```text
kernel   kernel-6.1/arch/arm/boot/zImage
fdt      kernel-6.1/arch/arm/boot/dts/rk3506-powerfin-spinor.dtb
ramdisk  minimal rootfs.cpio.gz
```

快速启动调试 FIT 不包含 `resource.img`。minimal initramfs 不显示开机 logo，省去约 1.4 MiB 的存储和读取开销。

FIT 的好处是 U-Boot 只需要加载一个文件，并由 `bootm` 处理 kernel、DTB 和 initramfs 的地址。

建议的日常构建接口：

```bash
./build.sh kernel
./repack_powerfin_ramboot.sh
```

只修改 DTS 时：

```bash
./build.sh kernel-make:rk3506-powerfin-spinor.dtb
./repack_powerfin_ramboot.sh
```

当前脚本输出：

```text
output/powerfin-ramboot/rootfs.cpio.gz
output/powerfin-ramboot/powerfin-ramboot.img
```

当前实测大小为：静态 BusyBox 829 KiB、`rootfs.cpio.gz` 426 KiB、不含 `resource.img` 的完整 FIT 4.64 MiB。

## 8. 从 SD 启动

当前 U-Boot 默认环境未定义 `loadaddr` 和 `kernel_addr_r`，因此使用已验证的固定临时地址 `0x02000000`。该地址与 U-Boot、FDT 及 FIT 内各镜像的最终加载地址不冲突。

加载并启动：

```text
fatload mmc <mmc-dev>:<part> 0x02000000 powerfin-ramboot.img
boot_fit 0x02000000
```

示例：

```text
mmc dev 0
mmc rescan
fatload mmc 0:1 0x02000000 powerfin-ramboot.img
boot_fit 0x02000000
```

原始 Rockchip U-Boot 启用了 `CONFIG_ROCKCHIP_RESOURCE_IMAGE`，`bootm <addr>` 的厂商封装路径会先查找 FIT 的 `/images/resource`，所以无 resource FIT 会报 `bootm can't read dtb`。当前源码已修改为：存在 resource 时保持旧流程，不存在时直接从 `/images/fdt` 加载 DTB。新版 U-Boot 已烧入板卡并验证：`bootm 0x02000000` 输出 `Load DTB from 'images/fdt'`，随后无 resource FIT 正常进入 `/init`。`bootm <addr>` 和 `boot_fit <addr>` 现在都可使用。

预期最终看到：

```text
POWERFIN_INIT_READY
<内核 uptime>
```

随后进入 BusyBox shell。

## 9. Kernel 命令行

调试慢 initcall 时建议：

```text
earlycon=uart8250,mmio32,0xff0a0000
console=ttyFIQ0
rdinit=/init
root=/dev/ram0
rw
printk.time=1
loglevel=8
initcall_debug=1
```

本 SDK 的 `initcall_debug` 是布尔 `core_param`，应显式写成 `initcall_debug=1`。initcall 记录属于 `KERN_DEBUG`，必须设置 `loglevel=8` 才会出现在串口；`loglevel=7` 不会显示。`initcall_debug` 和大量串口输出本身会拖慢启动，因此只用于定位问题。

测量基准速度时建议：

```text
earlycon=uart8250,mmio32,0xff0a0000
console=ttyFIQ0
rdinit=/init
root=/dev/ram0
rw
printk.time=1
quiet
loglevel=4
```

详细日志结果与安静模式结果必须分开记录。

## 10. 时间测量

建议记录以下里程碑：

| 阶段 | 起点 | 终点 |
|---|---|---|
| BootROM/SPL | 上电或复位 | SPL 跳转 OP-TEE/U-Boot |
| U-Boot | U-Boot 入口 | 执行 `boot_fit` |
| Kernel | 第一条 Linux 时间戳 | 执行 `/init` |
| 可用状态 | Kernel 入口 | `POWERFIN_INIT_READY` |

当前 NOR 启动日志中的已知基线：

```text
旧顺序（先探测 SD）：SPL 阶段约 466 ms
NOR 优先顺序：SPL Total 约 53.754 ms
SD FAT 读取含 resource 的 6.05 MiB FIT：约 512 ms，约 11.8 MiB/s
SD FAT 读取无 resource 的 4.64 MiB FIT：约 393 ms，约 11.8 MiB/s
Linux 第一条日志到 POWERFIN_INIT_READY：约 0.537 s
POWERFIN_INIT_READY 时 /proc/uptime：约 0.48 s
关闭 DebugFS、将 SC7U22 改为模块后：约 0.318 s
上述新配置到 POWERFIN_INIT_READY 时 /proc/uptime：约 0.26 s
```

从 SD 加载 FIT 的时间不是最终 NOR 产品启动时间。优化内核阶段时，可以从 Kernel 第一条日志开始计时；最终评估整机启动速度时，应再将稳定版本烧入 NOR 测一次完整链路。

不要用启用了 `initcall_debug`、高日志等级或大量串口输出的结果作为最终启动时间。

首次 `initcall_debug=1 loglevel=8` 结果保存在：

```text
output/powerfin-ramboot/logs/initcall-debug-level8.log
```

耗时最高的项目：

| 项目 | initcall 报告耗时 | 说明 |
|---|---:|---|
| `sc7u22_driver_init` | 208.736 ms | 驱动软复位中固定 `msleep(200)`，IMU 为飞控关键设备，不能直接删除 |
| `jent_mod_init` | 74.821 ms | jitter entropy 初始化，由 crypto DRBG/RNG 配置引入 |
| `of_platform_default_populate_init` | 40.000 ms | 其中 pinctrl probe 约 20 ms，多数为基础平台必需 |
| MMC controller probe | 26.967 ms | SD 枚举后续还需要约 34 ms；initramfs 启动后不依赖 SD rootfs |
| `clk_debug_init` | 18.056 ms | 由 `CONFIG_DEBUG_FS=y` 引入；相对完整启动链影响很小，当前 PowerFin 配置已恢复开启 |
| SPI NOR driver init | 6.804 ms | Linux 运行期访问 NOR 时需要，不能只按耗时删除 |

后续继续做相互独立的 A/B 实验：评估 crypto DRBG 是否为产品安全需求；让 MMC 在 `/init` 后再使用；如果以后必须将 SC7U22 内建，则将固定 200 ms 等待改为带 200 ms 上限的设备就绪轮询。每项单独构建和测量，避免一次改动过多而无法归因。

首次优化实测已完成：

- `CONFIG_DEBUG_FS` 曾关闭用于 A/B 测试，确认对当前整体启动时间影响很小后已恢复为 `CONFIG_DEBUG_FS=y`；
- `CONFIG_IIO_SC7U22=m`，minimal initramfs 不包含该模块，因此启动阶段不会执行 SC7U22 的固定 200 ms 软复位等待；
- Linux 第一条日志到 `/init` 从约 537 ms 降到约 318 ms，缩短约 219 ms（40.8%）；
- 正常 rootfs 需要安装内核模块，并在真正需要 IMU 前执行 `modprobe sc7u22`。模块加载仍会产生约 200 ms 的初始化等待，只是不会阻塞早期用户空间启动；
- 本次生成的模块为 `kernel-6.1/drivers/iio/imu/sc7u22.ko`。

如果飞控应用启动后立即依赖 IMU，应在应用启动脚本中先加载模块并等待对应 IIO 设备节点出现，不能仅以 `/init` 已执行作为飞控可用状态。

## 11. 推荐实施顺序

### 阶段一：验证 UMS

1. 启用 `CONFIG_CMD_USB_MASS_STORAGE=y`；
2. 重编并最后烧录一次 U-Boot；
3. 在 U-Boot 中确认 `ums` 命令；
4. 用调试 SD 卡验证主机可读写；（已完成）
5. 验证安全卸载后 U-Boot 仍能正常读取 FAT。（已完成）

### 阶段二：建立 minimal initramfs

1. 新建静态 BusyBox 配置；（已完成）
2. 创建 `/init`；（已完成）
3. 生成 `rootfs.cpio.gz`；（已完成）
4. 确认 `/init` 能输出 `POWERFIN_INIT_READY` 并进入 shell。（已完成）

### 阶段三：自动打包

1. 新建 `powerfin-ramboot.its`；（已完成）
2. 新建 `repack_powerfin_ramboot.sh`；（已完成）
3. 每次只重编 Kernel/DTB 并重打 FIT；（已完成）
4. 检查镜像大小和 FIT 内容。（已完成）

### 阶段四：启动优化

1. 保存详细启动日志；
2. 使用 `initcall_debug` 定位慢项；
3. 精简 PowerFin Kernel 配置和 DTS 中不用的设备；
4. 切换 quiet 配置测量真实基线；
5. 对每次改动记录配置、镜像大小和启动时间。

### 阶段五：生产链路复测

1. 将稳定的 boot 镜像烧入 SPI NOR；
2. 从 NOR 完整冷启动；
3. 比较 SD 调试路径和 NOR 生产路径；
4. 确认最终 8 MiB boot 分区容量和启动时间。

## 12. 日常操作速查

```text
# 主机：编译和打包
./build.sh kernel
./repack_powerfin_ramboot.sh

# U-Boot：把板载 SD 暴露给主机
ums 0 mmc <mmc-dev>

# 主机：复制并安全卸载
cp output/powerfin-ramboot.img /mnt/powerfin-sd/
sync
umount /mnt/powerfin-sd

# U-Boot UART：Ctrl+C 退出 UMS，然后加载并启动
mmc dev <mmc-dev>
mmc rescan
fatload mmc <mmc-dev>:<part> 0x02000000 powerfin-ramboot.img
boot_fit 0x02000000
```

### 12.1 历史调试布局：SD 同时存放 FIT 和 rootfs

> 本节记录早期从 SD 加载 FIT 的测试布局，已被 12.3 的“NOR FIT + SD 单分区 rootfs”布局取代。

调试 SD 已调整为：

| 分区 | 大小 | 格式 | 内容 |
|---|---:|---|---|
| p1 | 256 MiB | FAT32 | `powerfin-sdroot.img` |
| p2 | 剩余空间 | ext4 | 正式 Buildroot rootfs |

`repack_powerfin_sdroot.sh` 生成不含 initramfs 和 resource 的 kernel/DTB FIT，并将专用 DTB 的根设备设为 `/dev/mmcblk0p2`。本次 FIT 为 4,427,264 字节，从 SD FAT 加载约 358–359 ms。

DebugFS A/B 测试期间必须让 rootfs 中的内核模块与当前内核配置同步。旧模块按 `CONFIG_DEBUG_FS=y` 构建而内核关闭 DebugFS 时，`cfg80211.ko`、`usb-common.ko` 和 `stmmac.ko` 会引用内核不存在的 `debugfs_*` 符号。当前内核已恢复 `CONFIG_DEBUG_FS=y`，rootfs 也恢复在 `/sys/kernel/debug` 挂载 debugfs。

正式 rootfs 自动执行最后一个 init 脚本时输出 `POWERFIN_ROOTFS_READY` 和 `/proc/uptime`。实测结果：

| 场景 | Kernel 启动到 rootfs ready |
|---|---:|
| 新 rootfs 首次启动，含 fsck、在线扩容和首次随机种子处理 | 12.98 s |
| 扩容后首次完整 fsck | 15.01 s |
| 正常 reboot 后稳定状态 | 8.70 s |

稳定状态中，内核启动、等待 SD、挂载 ext4 并执行 `/sbin/init` 约 0.38 s，主要剩余时间在 Buildroot 的 fsck、udev、网络、SSH、USB gadget、fstrim、Luckfox 配置加载等用户空间服务。

### 12.2 Buildroot 用户空间 fastboot 优化

PowerFin 的三个 Buildroot defconfig 均启用了第一阶段 fastboot 策略：

```text
RK_DISK_SKIP_FSCK=y
# RK_ROOTFS_INPUT_EVENT_DAEMON is not set
# RK_ROOTFS_IRQBALANCE is not set
# RK_ROOTFS_FSTRIM is not set
```

板级实现位于：

```text
device/rockchip/.chips/rk3506/overlays/rootfs/powerfin-fastboot/
```

该 overlay 通过 `RK_DEFCONFIG=powerfin_*` 检查限制作用范围，不影响其他 RK3506 板。它执行以下操作：

- 用带 `/proc/uptime` 时间戳的 `rcS` 记录每个 `S??` 脚本耗时；
- 删除遍历并加载全部 53 个内核模块的 `S03modules_init.sh`；
- udev 保留冷插拔 trigger，但删除全局 `udevadm settle --timeout=30`；
- 删除启动时同步运行的 `S99luckfoxconfigload`，仍保留 `luckfox-config` 工具供手工调试；
- 删除 `bluetoothd` 启动服务；
- 保留 debugfs 挂载项，开机后 `/sys/kernel/debug` 可直接用于驱动调试；
- 安装 `blacklist sc7u22`，阻止 udev coldplug 自动初始化 IMU。应用之后显式执行 `modprobe sc7u22` 仍可正常加载；
- 保留 Wi-Fi 功能：`CONFIG_AIC8800_WLAN_SUPPORT=m`、`aic8800_fdrv.ko`、`aic_load_fw.ko` 和 `/lib/firmware/aic8800_fw/` 均保留；
- 保留 ADB + RNDIS，只在 `S06powerfin-usb-gadget` 中加载 `usb_f_fs`、`usb_f_rndis` 及其依赖，不恢复全量模块加载；
- `S51powerfin-rndis` 在后台等待 `usb0`，为板端配置 `192.168.123.100/24`，并用 dnsmasq 提供 `192.168.123.2`～`192.168.123.20` 的 DHCP 地址；
- RNDIS host/device MAC 由 SoC serial 派生并保持稳定，usbdevice 的重复 `update` 事件不会再次写已绑定的 configfs 属性；
- 用 PowerFin 专用 `S60netdevice` 保留 `eth0`/`eth1` 的异步 DHCP，删除通用脚本中等待 8 秒后才配置 USB 网卡的路径。Wi-Fi 初始化不受该脚本影响。
- 用 `S11powerfin-parallel` 协调器代替原来串行的后期 init 脚本：同时启动 ALSA、DBus、ES8311、Wi-Fi 上电、常规网络和 SSH；USB device、RNDIS、`S60netdevice` 及 auto-reboot 独立后台启动；
- 删除通用 `S80dnsmasq`，RNDIS 继续使用独立配置和 pid 文件，避免无配置的重复启动。

实测结果：

| 场景 | Kernel 启动到 `POWERFIN_ROOTFS_READY` |
|---|---:|
| 优化前稳定启动 | 8.70 s |
| 优化后新镜像首次启动，含在线扩容且随机种子未可信 | 8.65～8.94 s |
| 优化后稳定启动 | 2.77 s |
| 恢复 ADB + RNDIS 后稳定启动 | 2.74～2.82 s |
| 所有后期服务并行但同步等待 | 2.80 s |
| 两阶段并行版 `POWERFIN_APP_READY` | 2.01 s |
| 两阶段并行版 `POWERFIN_SYSTEM_READY` | 2.61 s |

稳定启动缩短 5.93 s，约 68.2%。首次启动时 `udevd` 会等待内核 `crng init done`，产生约 5.5 s 的一次性延迟；正常关机保存可信 seed 后，下一次 `S01seedrng` 会在约 1.00 s 时完成 CRNG 初始化，`S10udev` 仅耗时约 0.41 s。

优化后稳定启动的主要串行耗时为：

| 脚本 | 大约耗时 |
|---|---:|
| `S00mountall.sh` | 0.93 s |
| `S10udev` | 0.41 s |
| `S50sshd` | 0.34 s |
| `S30dbus` | 0.24 s |
| `S40network` | 0.24 s |
| `S11alsa-utils` | 0.21 s |

简单地把所有后期服务改为并行，然后在 ready 前等待它们全部完成，实测仍为 2.80 s：SD I/O 和 CPU 争用抵消了并行收益。因此最终采用两阶段语义：

- `POWERFIN_APP_READY` 表示核心用户空间已可启动应用；
- `POWERFIN_ROOTFS_READY` 与 APP ready 同时输出，仅用于兼容已有测量脚本，不再表示所有后台服务都完成；
- `POWERFIN_SYSTEM_READY` 表示 ALSA、DBus、ES8311、Wi-Fi 上电、常规网络和 SSH 并行组已完成；
- RNDIS/ADB 是独立的 USB 就绪路径，不阻塞上述两个标志。

最终稳定启动中，`S10udev` 在 1.57 s 完成，协调器在 1.87 s 返回，APP ready 为 2.01 s，最慢的 SSH 在 2.56 s 完成，SYSTEM ready 为 2.61 s。相对恢复 RNDIS 后的 2.74～2.82 s 基线，核心应用可提前约 0.7～0.8 s 启动。Wi-Fi 模块、AIC8800 固件和 Wi-Fi 上电流程均保留，仅禁用 Bluetooth daemon。

RNDIS 通常在 `POWERFIN_ROOTFS_READY` 之后完成枚举，不阻塞 ready 标记；本机实测约在 Kernel uptime 4.2～4.7 秒可用。Windows 的 RNDIS 驱动在链路重连后可能暂时保留 APIPA 地址而不立即重试 DHCP；本次调试主机将该专用网卡固定为 `192.168.123.2/24`，板端固定为 `192.168.123.100/24`。也可以保留 DHCP 并在需要时执行 `ipconfig /renew`。

当前最终 SD rootfs 镜像为 248,209,408 字节，SHA-256：

```text
aa765e7f6a734e1f95b1b3d737feff6cb0cafa90dc62d1e72d3543afb3edafb3
```

镜像已通过 Windows `Harddisk3Partition1` 原始写入并按 248,209,408 字节回读校验一致。板端验证：`sshd`、`dbus-daemon`、`dnsmasq` 和 `adbd` 均正常运行，`usb0` 为 `192.168.123.100/24`，`/run/powerfin-system-ready` 已生成；开机后 `sc7u22` 不在 `lsmod` 中，显式 `modprobe sc7u22` 可正常 probe，`bluetoothd` 未运行，AIC8800 Wi-Fi 模块和固件文件存在；Windows RNDIS 为 Up，主机 `192.168.123.2` 连续 ping 板端 `192.168.123.100` 成功。

### 12.3 NOR kernel + SD recovery/rootfs 生产式布局

当前已切换为：

```text
SPI NOR: SPL + OP-TEE + U-Boot + kernel/DTB FIT
SD p1: 32 MiB FAT16 LBA recovery 分区
SD p2: Buildroot ext4 rootfs
```

NOR 中的 FIT 不包含 `resource.img` 和 initramfs，DTB 中的 bootargs 为：

```text
root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw
```

该 bootargs 直接定义在 `rk3506-powerfin-spinor.dts` 的 `/chosen` 节点中。打包脚本不再复制或用 `fdtput` 修改编译后的 DTB。RAM-boot 使用独立的 `rk3506-powerfin-ramboot.dts`，其 `rdinit=/init` 同样固化在设备树中，不需要在 U-Boot 命令行临时执行 `setenv bootargs`。

构建和写入命令：

```bash
./build.sh kernel
./repack_powerfin_sdroot.sh --output kernel-6.1/zboot.img
./flash_zboot.sh -f -n
```

`repack_powerfin_zboot.sh` 是上述无 resource 打包流程的兼容入口，不再调用旧的 `zboot.its`、生成 `resource.img` 或选择通用 PowerFin DTB。

`flash_zboot.sh` 只写 `/dev/block/by-name/boot` 并做等长回读 SHA-256 校验，不写 U-Boot/Loader。本次 NOR FIT 为 4,511,232 字节，SHA-256：

```text
0eac34d00ac70ce8beb9865530bf00913337ea06dca8e187845e0e38dc4783b1
```

Buildroot 会生成可直接用 Win32DiskImager 或 `dd` 写入的
`powerfin-sdcard.img`。镜像使用 MBR：p1 从 1 MiB 开始，大小固定为
32 MiB，类型为 FAT16 LBA，卷标为 `RECOVERY`；p2 紧随其后，类型为
Linux，内容为 `rootfs.ext2`（实际格式为 ext4）。恢复分区当前为空，
后续可放入 recovery kernel/FIT，是否同时存放 U-Boot 由恢复流程确定。

镜像中的 p2 只包含构建时所需容量。第一次启动时，
PowerFin overlay 对 SDK 原有的 `S00mountall.sh` 做板级覆盖：脚本先
使用 `parted` 将 MBR 中的 p2 扩展到 SD 卡末尾，再沿用原有的
`mount-helper -> disk-helper` 路径，对 `/dev/mmcblk0p2` 执行在线
`resize2fs`，成功后继续使用 SDK 原有的 `/.resized` 标记。分区扩展
失败不会阻塞正常启动，并会清除旧标记以便下次启动重试完整流程；
分区已占满卡时只进行 sysfs 大小检查。首次启动还可能因未建立可信
随机种子而产生一次性延迟，正常关机保存随机种子后不再有此开销。

此前 p1 布局稳定复测为 `POWERFIN_APP_READY=2.06 s`、`POWERFIN_SYSTEM_READY=2.80 s`；新的双分区布局需在重新写卡后复测。`CONFIG_DEBUG_FS=y` 已生效，debugfs 会按 fstab 自动挂载到 `/sys/kernel/debug`。正常启动链只使用 NOR 中的 FIT 和 SD p2 rootfs。

## 13. 安全原则

- UMS 导出期间，SD 卡同一时间只能由主机或 U-Boot 一方访问；
- 主机必须先 `sync` 和 `umount`，再退出 UMS；
- 使用 `lsblk` 明确确认目标磁盘，禁止凭经验猜测 `/dev/sdX`；
- USB 模式切换后必须等待重新枚举，并重新执行 `lsusb` 或 `upgrade_tool ld` 确认状态；
- Flash 写入前先用 RAM Loader执行 `rfi` 和 `pl`，确认容量、介质和分区；
- `upgrade_tool db` 只下载 RAM Loader，`upgrade_tool ul` 会持久写入 Loader，不能混淆；
- 当前 RK3506 不使用 U-Boot Rockusb gadget 更新 U-Boot，更新时走 `rbrom → db → di`；
- 日常 Kernel 调试不执行 `upgrade_tool di -b`；
- SPI NOR 中保留最后一个可进入命令行的稳定 U-Boot；
- 修改 U-Boot、分区表或 Loader 时才重新写 NOR；
- 最终发布前必须回到真实 NOR 启动路径进行完整验证。
