# RK3506 SDK：SD 卡镜像布局与 update.img 说明

## 一、传统嵌入式 SD 卡上的排布

传统嵌入式 Linux 的 SD 卡，本质上就是一个带 GPT/MBR 分区表的块设备。典型布局如下：

```
┌─────────────────────────────────────────────────────┐
│                    SD 卡 (block device)              │
├───────┬──────┬──────────┬───────────────────────────┤
│ 隐藏区 │uboot │  boot    │        rootfs             │
│ (4MB) │(4MB) │  (12MB)  │      (剩余空间)            │
├───────┴──────┴──────────┴───────────────────────────┤
│  ↑               ↑                                  │
│  GPT表          内核(zImage+DTB)                     │
│  + MiniLoader    打包成 boot.img                      │
└─────────────────────────────────────────────────────┘
```

在 RK3506 SDK 中，分区表定义在 `device/rockchip/.chips/rk3506/parameter-lyra-sdmmc.txt`：

```
CMDLINE: mtdparts=rk29xxnand:
    0x00002000@0x00002000(uboot),    ← 起始扇区 0x2000, 大小 0x2000 扇区 = 4MB
    0x00006000@0x00004000(boot),     ← 起始扇区 0x4000, 大小 0x6000 扇区 = 12MB
    -@0x00010000(rootfs:grow)        ← 起始扇区 0x10000, 占满剩余空间
```

换算成字节偏移（每扇区 512 字节）：

| 分区 | 起始偏移 | 大小 | 内容 |
|------|---------|------|------|
| 隐藏区 | 0 | 4 MB | GPT 表头 + MiniLoaderAll.bin（SPL） |
| uboot | 4 MB | 4 MB | U-Boot proper（uboot.img） |
| boot | 8 MB | 12 MB | 内核 FIT Image（zImage + DTB + resource） |
| rootfs | 32 MB | 剩余 | Buildroot 生成的根文件系统（ext2） |

### 传统做法：用 `dd` 直接写入

```bash
dd if=MiniLoaderAll.bin of=/dev/sdX seek=0
dd if=uboot.img        of=/dev/sdX seek=8192   # 0x2000扇区 × 512
dd if=boot.img         of=/dev/sdX seek=16384  # 0x4000扇区 × 512
dd if=rootfs.img       of=/dev/sdX seek=32768  # 0x10000扇区 × 512
```

---

## 二、为什么要打包成 update.img？

RK SDK 最终输出 `update.img`，根源在于 **Rockchip 的烧录体系设计**。

### 2.1 update.img 的打包过程

```
各独立镜像                       afptool                    rkImageMaker
MiniLoaderAll.bin ─┐                              ┌─→ update.raw.img ─┐
uboot.img ────────┼─→ package-file ─→ afptool ─→│                    ├─→ update.img
boot.img ─────────┤                             └─→ (AFPT容器)      │
rootfs.img ───────┘                                                  └─→ 加Rockchip头 + MiniLoader
```

具体步骤：

1. **生成 package-file** — 脚本 `mk-updateimg.sh` 自动从分区表生成：
   ```
   # NAME        PATH
   package-file  package-file
   parameter     parameter.txt
   bootloader    MiniLoaderAll.bin
   uboot         uboot.img
   boot          boot.img
   rootfs        rootfs.img
   ```

2. **afptool 打包** — 把各个镜像按 package-file 打包成 AFPT（Android Firmware Pack Tool）容器：
   ```bash
   afptool -pack ./ update.raw.img
   ```

3. **rkImageMaker 封装** — 在前面附加 MiniLoaderAll.bin 并写入 Rockchip 芯片识别头：
   ```bash
   rkImageMaker -RK3506 MiniLoaderAll.bin update.raw.img update.img -os_type:androidos
   ```

### 2.2 为什么不用 dd 直接写 SD 卡？

Rockchip 的设计是 **USB 线刷优先**，不是 SD 卡量产优先：

- **开发调试阶段**：板子进入 RockUSB/MaskRom 模式，通过 USB 连接 PC，用 `upgrade_tool uf update.img` 一键烧录 — 即使完全没有可启动的固件也能刷
- **量产阶段**：使用 `SDDiskTool`（Windows GUI 工具）或 `Firmware_Merger` 将 update.img 转成 SD 卡镜像
- **OTA 升级阶段**：`update.img` 可以直接放在 SD 卡/U 盘里，U-Boot 检测到后自动升级

### 2.3 update.img 的实际优势

- **单一文件分发**：把 4 个独立镜像 + 分区表 + 元数据打包成一个文件
- **芯片校验**：Rockchip BootROM 能识别 header 中的芯片 ID，防止烧错固件
- **与 upgrade_tool 深度绑定**：`upgrade_tool` 能按分区名单独烧录，更新单个分区不必重写全盘：
  ```bash
  upgrade_tool di -b boot.img    # 只烧 boot 分区
  ```

---

## 三、SDK 中的替代方案

### 3.1 Yocto WIC 生成标准 GPT 镜像

`yocto/meta-rockchip/wic/generic-gptdisk.wks.in` 定义了完整的分区布局：

```
part --source rawcopy --sourceparams="file=idblock.img" --align 32 --no-table
part --source rawcopy --sourceparams="file=uboot.img" --part-name uboot --align 8192
part --source rawcopy --sourceparams="file=boot.img"  --part-name boot
part / --source rootfs --fstype ext4 --part-name rootfs
```

使用 `wic create` 即可生成可直接 `dd` 到 SD 卡的标准 GPT 磁盘镜像。

### 3.2 rkflash.sh 按分区烧录

```bash
rkflash.sh                  # 烧完整的 update.img
rkflash.sh uboot            # 只烧 uboot 分区
rkflash.sh boot             # 只烧 boot 分区
```

### 3.3 Windows 下的 SDDiskTool

`tools/windows/SDDiskTool_v1.78.zip` — 用于将 update.img 转换成 SD 卡量产镜像的 Windows 工具。

---

## 四、Buildroot 编译完整流程

```
Source components
    |
    +-- U-Boot sources
    |       +-- ./make.sh rk3506_luckfox
    |       |       +-- rk3506_spl_loader_*.bin  --> MiniLoaderAll.bin
    |       |       +-- uboot.img                     --> uboot.img
    |
    +-- Kernel 6.1
    |       +-- make rk3506g-luckfox-lyra-sd.img
    |       |       +-- zImage + DTB + resource
    |       |       +-- mkfitimage.sh (if FIT used)
    |       |       +-- zboot.img                  --> boot.img
    |
    +-- Buildroot
    |       +-- make for rk3506_luckfox
    |       |       +-- rootfs.ext2                --> rootfs.img
    |
    +-- mk-firmware.sh
    |       +-- symlinks everything into $RK_FIRMWARE_DIR/
    |       +-- copies parameter.txt
    |
    +-- mk-updateimg.sh
            +-- generates package-file from partition table
            +-- afptool -pack ./ update.raw.img
            +-- rkImageMaker -RK3506 MiniLoaderAll.bin update.raw.img update.img
```

### 最终输出文件

| 文件 | 来源 | 用途 |
|------|------|------|
| `MiniLoaderAll.bin` | U-Boot build（SPL + DDR init） | 一级引导程序 |
| `uboot.img` | U-Boot build | U-Boot proper |
| `boot.img` | Kernel build（zboot.img / FIT） | 内核镜像 |
| `rootfs.img` | Buildroot output（rootfs.ext2） | 根文件系统 |
| `parameter.txt` | Chip-specific parameter file | 分区定义 |
| `package-file` | Auto-generated | afptool 打包清单 |
| `update.img` | afptool + rkImageMaker | **最终固件容器** |

---

## 五、总结

| 方式 | 适用场景 | 工具 |
|------|---------|------|
| `update.img` + USB 线刷 | 开发调试、空板救砖 | `upgrade_tool`（Linux） / `RKDevTool`（Windows） |
| `update.img` + SD 卡升级 | 已运行系统的 OTA | U-Boot 检测 SD 卡中的 update.img |
| SDDiskTool 制作卡量产镜像 | 批量生产 | `SDDiskTool`（Win） |
| Yocto WIC 生成 GPT 镜像 | Linux 下的标准流程 | `wic create` |
| 手写 dd 脚本 | 理解底层、自定义需求 | shell 脚本 |

Rockchip 选择 `update.img` 作为默认输出的核心原因：其 BootROM 设计使得 USB 是最可靠的 first-boot 路径（即使 Flash 全空也能工作），而 `update.img` 正是为这个流程优化过的容器格式。
