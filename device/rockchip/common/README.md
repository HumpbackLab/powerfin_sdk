# PowerFin RK3506 Linux SDK

本文档面向 **PowerFin** 开发板，默认使用 **SPI NOR 启动固件 + SD 卡根文件系统**，对应 SDK 配置：

```text
powerfin_buildroot_spinor_defconfig
```

所有命令均在 SDK 根目录执行：

```bash
cd /path/to/rk3506_sdk
```

## 1. 存储布局

PowerFin 的 SPI NOR 只保存启动所需内容：

- `MiniLoaderAll.bin`
- `uboot.img`
- `boot.img`（Linux 内核、正常/恢复设备树和 recovery initramfs）

Buildroot 根文件系统保存在 SD 卡中。SPI NOR 分区表
`device/rockchip/.chips/rk3506/parameter-powerfin-spinor.txt` 只有 `uboot`
和 `boot` 分区。PowerFin 实际上有两份 PFC：

| PFC | 所在位置 | 更新方式 |
| --- | --- | --- |
| 正常系统 PFC | SD 卡 Buildroot 根文件系统 | 重编 Buildroot 并重写 SD 卡镜像 |
| Recovery PFC | SPI NOR `boot.img` 内的 recovery initramfs | 重打 `zboot.img` 和 `update.img`，再更新 NOR |

PX4 只在 SD 卡 Buildroot 根文件系统中。`update.img` 不包含 Buildroot 根文件系统、
正常系统 PFC 或 PX4，但它包含随 `boot.img` 打包的 recovery PFC。

## 2. 选择 PowerFin SPI NOR 配置

首次编译、切换过其他板型，或者删除过 `output/` 后，先执行：

```bash
./build.sh powerfin_buildroot_spinor_defconfig
```

可用下面的命令确认当前配置：

```bash
readlink -f output/defconfig
grep '^RK_DEFCONFIG=' output/.config
```

输出应指向 `powerfin_buildroot_spinor_defconfig`。后续单独编译 kernel、U-Boot
或 Buildroot 都会复用该配置，不需要每次重新选择。

## 3. 编译完整的 SPI NOR `update.img`

### 3.1 首次编译前准备 PFC

PowerFin 的 Buildroot 配置默认启用 Penguin Flight Console（PFC）。PFC 以预编译
程序的形式进入正常根文件系统和 recovery initramfs。单独编译 Buildroot 前先生成它：

```bash
cd tools/penguin-flight-console
./build-cross.sh
cd ../..
```

该脚本需要 Rust `cross`，以及 Docker 或 Podman。生成的程序位于：

```text
tools/penguin-flight-console/dist/penguin-flight-console
tools/penguin-flight-console/dist/penguin-flight-console-update.tar.gz
tools/penguin-flight-console/dist/recovery/penguin-flight-console
```

执行 `./build.sh all` 时，kernel 打包阶段也会自动调用该脚本，因此不需要重复执行。

PX4 源码默认位于 SDK 同级目录：

```text
../PX4-Autopilot/
```

Buildroot 会自动用自己的交叉工具链编译 PX4 的 `humpback_powerfin` 目标。

### 3.2 完整编译

```bash
./build.sh powerfin_buildroot_spinor_defconfig
./build.sh all
```

`all` 会依次编译 U-Boot/Loader、kernel、Buildroot，整理各分区镜像，并自动打包
`update.img`。最终 NOR 升级包位于：

```text
output/firmware/update.img
```

兼容旧工具的路径 `rockdev/update.img` 指向同一个文件。打包工作目录位于：

```text
output/update/Image/
```

如果各组件已经编译完成，只需要重新整理固件并打包，无需重新编译所有源码：

```bash
./build.sh firmware
```

也可以只重新打包已有的分区镜像：

```bash
./build.sh updateimg
```

`updateimg` 不会主动重编 kernel、U-Boot 或 Buildroot；它只打包当前已有产物。

## 4. 单独编译 kernel

```bash
./build.sh kernel
```

PowerFin SPI NOR 配置使用：

```text
kernel defconfig : rk3506_luckfox_defconfig + powerfin.config
device tree      : rk3506-powerfin-spinor.dts
```

主要产物：

```text
kernel-6.1/arch/arm/boot/zImage
kernel-6.1/arch/arm/boot/dts/rk3506-powerfin-spinor.dtb
kernel-6.1/zboot.img
output/firmware/boot.img -> ../../kernel-6.1/zboot.img
```

只修改 DTS 时，可以在已有 kernel 编译结果上重编两个 PowerFin DTB，并快速重打
`zboot.img`：

```bash
./repack_powerfin_zboot.sh --build-dtb
```

kernel 或 DTS 更新后，如需生成新的 NOR 升级包，再执行：

```bash
./build.sh updateimg
```

## 5. 单独编译 U-Boot

```bash
./build.sh uboot
```

该配置使用 `rk3506_luckfox` 基础配置和 `powerfin_spinor` 配置片段。主要产物：

```text
u-boot/uboot.img
u-boot/*_spl_loader_*.bin
output/firmware/uboot.img
output/firmware/MiniLoaderAll.bin
```

U-Boot 更新后，如需生成新的 NOR 升级包，再执行：

```bash
./build.sh updateimg
```

## 6. 单独编译 Buildroot

```bash
./build.sh buildroot
```

PowerFin 使用 `rockchip_powerfin_defconfig`。完整的 SD 卡镜像为：

```text
buildroot/output/rockchip_powerfin/images/powerfin-sdcard.img
buildroot/output/rockchip_powerfin/images/powerfin-sdcard.img.gz
```

其中 SD 卡包含：

- `RECOVERY` FAT 分区；
- Linux `rootfs` 分区。

更新 Buildroot、正常系统 PFC 或 PX4 后，应重新写入上述 SD 卡镜像。它们不在
SPI NOR 的 `update.img` 中。Recovery PFC 是例外：它位于 NOR `boot.img` 内，
更新方法见 7.2 节。

## 7. 更新 PFC/PX4

对于正常系统 PFC 和 PX4，Buildroot 使用 stamp 文件记录软件包是否已经编译。
仅修改本地源码后再次运行
`./build.sh buildroot`，对应软件包可能因为旧 stamp 而不重新编译。要确保最新产物
进入根文件系统，先清理对应软件包的构建目录，再重新编译 Buildroot。

### 7.1 更新 SD 卡正常系统中的 PFC

先重新交叉编译 PFC：

```bash
cd tools/penguin-flight-console
./build-cross.sh
cd ../..
```

然后强制 Buildroot 重新复制并安装 PFC：

```bash
./build.sh bmake:penguin-flight-console-dirclean
./build.sh buildroot
```

PFC 在目标文件系统中的位置：

```text
/usr/bin/penguin-flight-console
/usr/libexec/penguin-flight-console/powerfin/
```

Buildroot 仍将裸二进制安装到 `/usr/bin`，并将 `boards/powerfin/*.sh` 安装到
`/usr/libexec/penguin-flight-console/powerfin/`。网页自更新使用的完整升级包是：

```text
tools/penguin-flight-console/dist/penguin-flight-console-update.tar.gz
```

该升级包作为浏览器上传文件使用，不需要预装进根文件系统。

编译后可在未打包的最终根文件系统中检查：

```bash
ls -l output/buildroot/target/usr/bin/penguin-flight-console
```

### 7.2 更新 NOR 中的 Recovery PFC

Recovery PFC 不是独立的 NOR 分区文件。它被放入 recovery initramfs，再与 kernel
和两个 DTB 一起打包成 `kernel-6.1/zboot.img`，最终作为 `boot.img` 写入 NOR 的
`boot` 分区。

PFC 代码更新后执行：

```bash
./repack_powerfin_zboot.sh
./build.sh updateimg
```

`repack_powerfin_zboot.sh` 会自动完成以下操作：

1. 调用 `tools/penguin-flight-console/build-cross.sh` 重新编译 PFC；
2. 将 `dist/recovery/penguin-flight-console` 放入 recovery initramfs；
3. 生成 `output/powerfin-ramboot/rootfs.cpio.gz`；
4. 重新生成 `kernel-6.1/zboot.img`。

`./build.sh updateimg` 再将新的 `zboot.img` 作为 `boot.img` 打进：

```text
output/firmware/update.img
```

最后按正常 NOR 升级流程烧写该 `update.img`。仅执行 `./build.sh updateimg` 不会
重新编译 PFC 或重建 recovery initramfs，因此不能省略前面的重打包命令。

这条流程不需要执行 Buildroot 的 `penguin-flight-console-dirclean`；该命令只负责
更新 SD 卡正常系统中的 PFC。

> 当前 `flash_zboot.sh` 会拒绝包含 ramdisk 的多配置 FIT，不要用它更新这份
> recovery PFC；使用重新生成的 `update.img`。

### 7.3 更新 PX4

确认更新后的 PX4 源码位于：

```text
../PX4-Autopilot/
```

然后执行：

```bash
./build.sh bmake:px4-powerfin-dirclean
./build.sh buildroot
```

Buildroot 会重新复制 PX4 源码、编译 `humpback_powerfin`，并将运行环境安装到：

```text
/root/px4/bin/
/root/px4/etc/
/root/px4/posix-configs/
```

编译后可检查：

```bash
ls -l output/buildroot/target/root/px4/bin/px4
```

如果 PFC 和 PX4 都有更新，分别执行两个 `dirclean` 命令后，只需运行一次
`./build.sh buildroot`。

### 7.4 完全清理 Buildroot（兜底方法）

当 Buildroot 配置、工具链或多个底层依赖发生变化，软件包级清理仍不能解决问题时：

```bash
./build.sh clean-rootfs
./build.sh buildroot
```

这会删除 `buildroot/output/rockchip_powerfin/` 并完整重编根文件系统，耗时明显更长。

## 8. 产物路径汇总

| 产物 | 路径 | 用途 |
| --- | --- | --- |
| SPI NOR 完整升级包 | `output/firmware/update.img` | 烧写 Loader、U-Boot 和 boot |
| 打包工作目录 | `output/update/Image/` | 查看 `update.img` 实际包含的文件 |
| Loader | `output/firmware/MiniLoaderAll.bin` | SPI NOR 启动 Loader |
| U-Boot 镜像 | `output/firmware/uboot.img` | SPI NOR `uboot` 分区 |
| kernel/FIT 镜像 | `output/firmware/boot.img` | SPI NOR `boot` 分区 |
| Recovery initramfs | `output/powerfin-ramboot/rootfs.cpio.gz` | 包含 NOR recovery PFC |
| Recovery PFC 输入文件 | `tools/penguin-flight-console/dist/recovery/penguin-flight-console` | 打包进 recovery initramfs |
| kernel 原始镜像 | `kernel-6.1/arch/arm/boot/zImage` | Linux zImage |
| PowerFin NOR DTB | `kernel-6.1/arch/arm/boot/dts/rk3506-powerfin-spinor.dtb` | 设备树二进制 |
| Buildroot 输出目录 | `buildroot/output/rockchip_powerfin/` | Buildroot 编译缓存、工具链和目标目录 |
| 最终 rootfs 目录 | `output/buildroot/target/` | 打包前检查目标文件系统内容 |
| rootfs 镜像 | `buildroot/output/rockchip_powerfin/images/rootfs.ext2` | SD 卡 rootfs 分区内容；`rootfs.ext4` 是其链接 |
| 完整 SD 卡镜像 | `buildroot/output/rockchip_powerfin/images/powerfin-sdcard.img` | 写入 SD 卡 |
| 压缩 SD 卡镜像 | `buildroot/output/rockchip_powerfin/images/powerfin-sdcard.img.gz` | 分发或保存 |
| 正常系统 PFC 输入文件 | `tools/penguin-flight-console/dist/penguin-flight-console` | Buildroot 的 PFC 输入文件 |
| PFC 网页自更新包 | `tools/penguin-flight-console/dist/penguin-flight-console-update.tar.gz` | 更新正常系统 PFC 及配套板级脚本 |
| 构建日志 | `output/log/` | 最近一次构建日志 |

`output/firmware/` 和 `rockdev/` 中很多文件是符号链接；排查产物来源时可使用：

```bash
readlink -f output/firmware/boot.img
readlink -f output/firmware/uboot.img
readlink -f output/firmware/update.img
```

## 9. 常用工作流

### 修改 kernel/DTS，并更新 SPI NOR

```bash
./build.sh kernel
./build.sh updateimg
```

### 只修改 DTS，并更新 SPI NOR

```bash
./repack_powerfin_zboot.sh --build-dtb
./build.sh updateimg
```

### 只更新 NOR 中的 Recovery PFC

```bash
./repack_powerfin_zboot.sh
./build.sh updateimg
```

完成后按正常 NOR 升级流程烧写 `output/firmware/update.img`。

### 更新 PFC/PX4，并更新 SD 卡根文件系统

```bash
cd tools/penguin-flight-console
./build-cross.sh
cd ../..
./build.sh bmake:penguin-flight-console-dirclean
./build.sh bmake:px4-powerfin-dirclean
./build.sh buildroot
```

完成后写入：

```text
buildroot/output/rockchip_powerfin/images/powerfin-sdcard.img
```
