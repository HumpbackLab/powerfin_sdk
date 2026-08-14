# PowerFin RK3506 Linux SDK

本文档面向 **PowerFin** 开发板，默认使用 **SPI NOR 启动固件 + SD 卡根文件系统**，对应 SDK 配置：

```text
powerfin_buildroot_spinor_defconfig
```

## 0. 从零拉取 SDK

然后安装 Git LFS 和 `repo`。以下命令适用于 Ubuntu/Debian：

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl git git-lfs

mkdir -p "${HOME}/.local/bin"
curl -fL https://storage.googleapis.com/git-repo-downloads/repo \
  -o "${HOME}/.local/bin/repo"
chmod +x "${HOME}/.local/bin/repo"
export PATH="${HOME}/.local/bin:${PATH}"

git lfs install
repo version
```

建议将 `export PATH="${HOME}/.local/bin:${PATH}"` 加入 `~/.bashrc`。从空目录拉取
完整 SDK：

```bash
git clone https://github.com/HumpbackLab/powerfin_sdk.git rk3506_sdk
cd rk3506_sdk
git lfs pull

repo init \
  --manifest-url=https://github.com/HumpbackLab/manifest.git \
  --manifest-branch=master \
  --manifest-name=powerfin.xml \
  --depth=1
repo sync --current-branch --jobs=4 --fail-fast --no-tags
```

同步后可检查各仓库状态，或导出当前所有仓库对应的精确提交：

```bash
repo status
repo manifest -r -o powerfin-resolved.xml
```

`powerfin-resolved.xml` 只用于排查版本，不应提交。更新已有工作区时，先用
`repo status` 确认没有需要保留的未提交修改，再执行：

```bash
git pull --ff-only
repo sync --current-branch --jobs=4 --fail-fast --no-tags
git lfs pull
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

| PFC | 所在位置 |
| --- | --- |
| 正常系统 PFC | SD 卡 Buildroot 根文件系统 |
| Recovery PFC | SPI NOR `boot.img` 内的 recovery initramfs |

PX4 只在 SD 卡 Buildroot 根文件系统中。`update.img` 不包含 Buildroot 根文件系统、
正常系统 PFC 或 PX4，但它包含随 `boot.img` 打包的 recovery PFC。

## 2. 选择 PowerFin SPI NOR 配置

首次编译、切换过其他板型，或者删除过 `output/` 后，先执行：

```bash
./build.sh rk3506:powerfin_buildroot_spinor_defconfig
```

可用下面的命令确认当前配置：

```bash
readlink -f output/defconfig
grep '^RK_DEFCONFIG=' output/.config
```

输出应指向 `powerfin_buildroot_spinor_defconfig`。后续单独编译 kernel、U-Boot
或 Buildroot 都会复用该配置，不需要每次重新选择。

## 3. 编译完整的 SPI NOR `update.img`

```bash
./build.sh rk3506:powerfin_buildroot_spinor_defconfig
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

Buildroot 根文件系统发生变化后，应重新写入上述 SD 卡镜像；它不包含在 SPI NOR
的 `update.img` 中。

### 完全清理 Buildroot（兜底方法）

当 Buildroot 配置、工具链或多个底层依赖发生变化，软件包级清理仍不能解决问题时：

```bash
./build.sh clean-rootfs
./build.sh buildroot
```

这会删除 `buildroot/output/rockchip_powerfin/` 并完整重编根文件系统，耗时明显更长。

## 7. 产物路径汇总

| 产物 | 路径 | 用途 |
| --- | --- | --- |
| SPI NOR 完整升级包 | `output/firmware/update.img` | 烧写 Loader、U-Boot 和 boot |
| 打包工作目录 | `output/update/Image/` | 查看 `update.img` 实际包含的文件 |
| Loader | `output/firmware/MiniLoaderAll.bin` | SPI NOR 启动 Loader |
| U-Boot 镜像 | `output/firmware/uboot.img` | SPI NOR `uboot` 分区 |
| kernel/FIT 镜像 | `output/firmware/boot.img` | SPI NOR `boot` 分区 |
| Recovery initramfs | `output/powerfin-ramboot/rootfs.cpio.gz` | 包含 NOR recovery PFC |
| kernel 原始镜像 | `kernel-6.1/arch/arm/boot/zImage` | Linux zImage |
| PowerFin NOR DTB | `kernel-6.1/arch/arm/boot/dts/rk3506-powerfin-spinor.dtb` | 设备树二进制 |
| Buildroot 输出目录 | `buildroot/output/rockchip_powerfin/` | Buildroot 编译缓存、工具链和目标目录 |
| 最终 rootfs 目录 | `output/buildroot/target/` | 打包前检查目标文件系统内容 |
| rootfs 镜像 | `buildroot/output/rockchip_powerfin/images/rootfs.ext2` | SD 卡 rootfs 分区内容；`rootfs.ext4` 是其链接 |
| 完整 SD 卡镜像 | `buildroot/output/rockchip_powerfin/images/powerfin-sdcard.img` | 写入 SD 卡 |
| 压缩 SD 卡镜像 | `buildroot/output/rockchip_powerfin/images/powerfin-sdcard.img.gz` | 分发或保存 |
| 构建日志 | `output/log/` | 最近一次构建日志 |

`output/firmware/` 和 `rockdev/` 中很多文件是符号链接；排查产物来源时可使用：

```bash
readlink -f output/firmware/boot.img
readlink -f output/firmware/uboot.img
readlink -f output/firmware/update.img
```

## 8. 常用工作流

### 手动触发完整镜像发布

GitHub Actions 中的 `PowerFin release` 工作流只接受手动触发。它会从空工作区同步
manifest 中的全部仓库，完整编译 PowerFin SPI NOR 配置，并在
`ncer/powerfin_sdk` 创建 Gitee Release。Release 标题为工作流触发时的北京时间，
附件包括：

- `powerfin-sdcard.img.gz`：完整 SD 卡镜像。Gitee 社区版附件单文件上限为
  100 MB，因此发布压缩镜像，写卡前需先解压；
- `update.img`：完整 SPI NOR 升级镜像，包含 Loader、U-Boot 和 boot；
- `zboot.img`：包含 kernel、正常/Recovery DTB 和 Recovery PFC 的 SPI NOR
  `boot` 分区镜像；
- `SHA256SUMS`：上述三个镜像文件的 SHA256。

发布前需在 GitHub 仓库的 `Settings` → `Secrets and variables` → `Actions` 中配置
`GITEE_ACCESS_TOKEN`，该 Gitee 私人令牌必须能够向 `ncer/powerfin_sdk` 创建
Release 并上传附件。然后在 `Actions` → `PowerFin release` 中点击
`Run workflow`。

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
