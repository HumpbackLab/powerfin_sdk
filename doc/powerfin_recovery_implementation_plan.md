# PowerFin NOR Recovery 精简实现计划

## 1. 目标

PowerFin 使用 128 Mbit（16 MiB）NOR Flash。目标是在不改变现有 NOR 分区布局的前提下，让设备具备以下能力：

- SD 卡正常时，从 SD 卡启动 Buildroot；
- SD 卡不存在或系统损坏时，从 NOR 自动进入 Recovery；
- Recovery 通过 USB RNDIS 提供 Penguin Flight Console；
- 用户可以在网页中完整烧写 SD 卡。
- 烧写过程采用流式处理，不受设备 128 MiB 内存限制。

第一版只实现恢复闭环，不加入 A/B、自定义镜像格式、在线升级 U-Boot等扩展功能。

## 2. 启动流程

```text
上电
  -> NOR U-Boot
  -> 检查 SD 卡
       |- 检查通过：启动 FIT 的 conf-normal -> SD 卡 rootfs
       `- 检查失败：启动 FIT 的 conf-recovery -> NOR Recovery
```

## 3. NOR 和 FIT

保持现有 NOR 分区不变：

| 区域 | 大小 | 内容 |
|------|------|------|
| Loader/保留区 | 约 4 MiB | 现有 Rockchip loader |
| U-Boot | 4 MiB | U-Boot proper |
| boot | `0x007f0000`，约 8 MiB | 一个多配置 FIT |

FIT 中包含：

- 一份公共 Linux kernel；
- 正常 DTB；
- Recovery DTB；
- Recovery initramfs；
- `conf-normal`；
- `conf-recovery`。

```text
conf-normal   = kernel + normal.dtb
conf-recovery = kernel + recovery.dtb + recovery-initramfs
```

两个配置共用 kernel，避免 NOR 中保存两份 zboot。`conf-normal` 设为默认配置，便于旧版 U-Boot 在迁移时继续启动。

构建时必须检查最终 FIT 小于 boot 分区的 `0x007f0000` 字节容量，超出时直接失败。

## 4. U-Boot 自动选择

PowerFin 当前 SD 卡采用 MBR，第一版只做最小有效性检查：

1. SD 卡能够初始化；
2. 分区 1 存在，并能从 FAT 读取 `/powerfin-layout`；
3. 分区 2 存在，并能从 ext4 读取 `/etc/powerfin-release`。

全部通过时启动 `conf-normal`；任意一步失败时启动 `conf-recovery`。

两个标记文件由 SD 卡镜像构建流程写入。第一版只检查文件存在且可读，不校验精确扇区、容量、UUID 或复杂版本规则。

U-Boot 修改限定在 PowerFin 配置下，不影响其他 RK3506 板卡。

### FIT 启动注意事项

选中的配置名必须贯穿整个 Rockchip FIT 启动流程，包括：

- FIT 配置查找；
- 加载范围计算；
- kernel、DTB 和 ramdisk 读取；
- 镜像校验；
- 最终启动。

不能只在最后执行 `bootm ...#conf-recovery`，否则前面的加载过程仍可能使用默认配置。

因此，最好在比较早的阶段决定用哪个conf，这样后续流程就用那个conf就行了。

### 手动进入 Recovery

Penguin Flight Console 的“进入 Recovery”按钮优先执行：

```sh
reboot recovery
```

如果当前平台不能可靠传递 recovery reboot flag，停下来询问。

## 5. Kernel 和 DTB

正常系统和 Recovery 共用 kernel。Recovery 无法从 SD 卡加载模块，因此 USB gadget、configfs 和 RNDIS 相关功能需要编进 kernel。

正常 DTB 保留现有硬件配置，bootargs 指向 `/dev/mmcblk0p2`。Recovery DTB 继承相同硬件定义，只修改启动参数，使 kernel 使用 initramfs 的 `/init`。

正常系统现有的 RNDIS 脚本也应兼容驱动已经内建的情况，不能把 `modprobe` 成功作为启动条件。

## 6. Recovery initramfs

Recovery 只包含烧写所需组件：

- BusyBox；
- `ip`、`mount`、`umount`；
- `gzip`、`dd`、`sha256sum`、`sync`；
- configfs 和 RNDIS；
- 静态链接的 Penguin Flight Console；
- PowerFin Recovery shell 脚本。

不包含 Wi-Fi、PX4、SSH、ADB、D-Bus、udev、字体、FFmpeg 和显示组件。

Recovery `/init` 的启动顺序：

1. 挂载 `/proc`、`/sys`、`/dev` 和 configfs；
2. 创建 USB RNDIS gadget；
3. 配置设备地址 `192.168.123.100/24`；
4. 启动最小 DHCP 服务；
5. 启动 Penguin Flight Console。

VID/PID、MAC 生成方式和 DHCP 范围沿用正常系统现有配置。

## 7. Penguin Flight Console

使用同一份 Rust 源码构建两个版本：

| 版本 | 位置 | 功能 |
|------|------|------|
| 正常版 | SD 卡 Buildroot | 现有功能，并增加“进入 Recovery” |
| Recovery 静态版 | NOR initramfs | SD 状态、烧写、日志、重启 |

Recovery 版本使用 `armv7-unknown-linux-musleabihf` 交叉编译，并检查最终 ELF 没有动态解释器和共享库依赖。

保持项目现有的简单结构：

```text
网页按钮 -> Rust API -> PowerFin shell 脚本
```

Rust 只处理 HTTP、固定大小缓冲和脚本调用，不在 Rust 中实现具体烧写逻辑。

Recovery 页面只提供：

- SD 卡检查结果；
- 完整烧写 SD 卡；
- 进度和错误日志；
- 重启设备。

烧写操作使用全局锁，同一时间只允许一个任务。目标设备固定在 board 配置中，HTTP 请求不能指定任意设备路径。

## 8. 流式烧写

### 8.1 完整烧写 SD 卡

上传文件：

```text
powerfin-sdcard.img.gz
```

调用 `sdcard-flash.sh`，数据链路为：

```text
HTTP 请求体 -> Rust 固定缓冲 -> 脚本 stdin
              -> gzip -dc -> dd of=/dev/mmcblk0 -> sync
```

用于空白卡、分区损坏和完整恢复。成功后重新检查两个标记文件。


## 9. 发布产物

| 文件 | 用途 |
|------|------|
| `powerfin-boot.itb` | NOR boot 分区的多配置 FIT |
| `powerfin-sdcard.img.gz` | 完整 SD 卡恢复镜像 |
| `powerfin-sdcard.img.gz.sha256` | 完整镜像校验值 |


U-Boot 和整个 NOR 的首次迁移继续使用 RKDevTool/upgrade_tool，不通过网页更新。

## 10. 实施顺序

### 阶段一：多配置 FIT

1. 合并正常和 ramboot ITS；
2. 生成两个配置并共用 kernel；
3. 手动验证两个配置都能启动；
4. 增加 FIT 容量检查。

### 阶段二：U-Boot 自动选择

1. 增加 SD 卡和两个标记文件检查；
2. 将配置名传入完整 FIT 加载流程；
3. 在串口日志中打印选择结果和原因；
4. 验证正常卡、空卡、坏卡和无卡启动。

### 阶段三：Recovery 环境

1. 将 RNDIS 依赖编进 kernel；
2. 精简并补齐 Recovery BusyBox；
3. 配置 RNDIS、固定 IP 和 DHCP；
4. 将静态 Penguin Flight Console 打进 initramfs。

### 阶段四：网页烧写

1. 增加 `sdcard-flash.sh`；
2. 增加流式上传 API；
3. 增加全局锁、固定目标和 SHA256 校验；
4. 增加进度、错误和重启界面；
5. 正常系统增加“进入 Recovery”按钮。

## 11. 测试和验收

必须覆盖：

- 无 SD 卡、空白卡或缺少任一标记时进入 Recovery；
- 正常 SD 卡进入 Buildroot；
- Linux 和 Windows 能通过 USB 访问 `192.168.123.100`；
- 能烧写完整 SD 卡镜像；
- 上传大于设备内存的镜像时，内存占用保持稳定且 `/tmp` 无完整镜像；
- gzip 错误、SHA256 错误、客户端断开和重复点击不会误报成功；
- 烧写成功后重启进入正常系统；
- 其他 RK3506 板卡的启动行为不受影响。

完成标准：用户在 SD 卡损坏甚至未插卡时，仍能通过 USB 打开 Penguin Flight Console，烧写系统并重新启动到正常 Buildroot。

## 12. 第一版不做

- 自定义 PFCIMG 格式；
- A/B rootfs；
- 复杂断电事务和恢复状态机；
- FIT 签名或镜像加密；
- 网页升级 U-Boot/Loader；
- PX4 参数自动迁移；
- SD 卡精确容量、UUID 和扇区布局校验；
- Recovery 中的 Wi-Fi、PX4、SSH 等正常系统功能。
