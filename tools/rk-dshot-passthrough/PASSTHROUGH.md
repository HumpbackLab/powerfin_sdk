# RK3506 Flexbus BLHeli32 Passthrough Notes

本文记录当前 RK3506 Flexbus DShot passthrough 的实现、构建部署方式、BLHeliSuite32 使用方式，以及当前已知状态。

## 当前状态

已经实现并验证的能力：

- 内核 `rockchip-flexbus-dshot` 提供 `/dev/rk-flexbus-dshot` misc 设备。
- 用户态可通过 `RK_DSHOT_IOC_PASSTHROUGH_XFER` 做 Flexbus raw sample TX/RX。
- `rk-blheli32-info` 可以进入 BLHeli32 bootloader 并读取基础信息/Flash。
- `rk-blheli32-bf4way` 可以模拟 Betaflight/Cleanflight MSP + serial 4way 接口，让 BLHeliSuite32 连接到 ESC。
- BLHeliSuite32 可以连接、读 setup、写 setup；实测写入内容会生效。
- 2026-07-11 已重新编译并刷写内核 `6.1.99 #37`，确认 `RK_DSHOT_PASSTHROUGH_MAX_SAMPLES = 524288` 的内核可正常启动、创建设备节点并完成 32x BLHeli32 bootloader 读取。

当前未完全解决的问题：

- BLHeliSuite32 写 setup 后仍可能弹出 `Write Setup section to ESC Failed!`。
- 调试中确认 BLHeli32 bootloader 对写入返回成功，额外内部 verify 也返回 `0x30` 成功；问题更像是 BLHeliSuite32 写后收尾/状态判断未完全满足。
- 真实读取 `0x7c00` 设置区曾出现不稳定/随机数据。当前 `rk-blheli32-bf4way` 对写后的同地址读使用写缓存返回，以保证 Suite 写后读回比较能看到刚写入的数据。这是当前工程状态，不是最终理想实现。

## 硬件与连接

当前测试环境：

- 板端 IP：`192.168.123.100`
- 用户名：`root`
- 密码：`luckfox`
- BLHeliSuite32 所在 PC 通过 USB-UART 连接板端 UART4。
- 板端 UART4 设备：`/dev/ttyS4`
- PC 侧串口通常是：`/dev/ttyUSB0`
- Flexbus passthrough 设备：`/dev/rk-flexbus-dshot`

当前布线/通道约定：

- host TX/RX 走 UART4，用于 BLHeliSuite32 和板端 daemon 通信。
- Flexbus TX 输出到 ESC 信号线。
- Flexbus RX 从 ESC 信号线采样返回。
- 当前不依赖 PX4，测试 passthrough 时应关闭 PX4，避免占用 DShot/Flexbus 输出。

注意：

- 当前 ESC 不支持双向 inverted DShot，而当前板端只能发 inverted DShot，所以调试 BLHeli32 passthrough 时不要发 DShot 命令。
- BLHeli32 bootloader 通信使用近似 19200 baud 的单线协议，由 Flexbus raw sample 发送/采样完成。

## 内核驱动

相关文件：

- `kernel-6.1/drivers/misc/rockchip/rockchip-flexbus-dshot.c`

新增/当前使用的 ioctl：

```c
#define RK_DSHOT_IOC_PASSTHROUGH_XFER \
    _IOWR('D', 0x04, struct rk_dshot_passthrough_xfer)

struct rk_dshot_passthrough_xfer {
    __u64 tx_buf;
    __u64 rx_buf;
    __u32 tx_samples;
    __u32 rx_samples;
    __u32 sample_rate;
    __u32 channel;
    __u32 timeout_ms;
    __u32 flags;
};
```

flags：

```c
#define RK_DSHOT_PASSTHROUGH_F_TX_INVERT BIT(0)
#define RK_DSHOT_PASSTHROUGH_F_RX_INVERT BIT(1)
```

当前关键限制：

- `RK_DSHOT_PASSTHROUGH_MAX_SAMPLES = 524288`
- 常驻 DMA coherent 内存：TX 256 KiB + RX 256 KiB，合计 512 KiB。
- sample rate 允许范围：`100000` 到 `10000000`
- channel 范围：`0..3`

驱动行为：

- `tx_samples > 0 && rx_samples > 0`：一次 ioctl 内同时启动 TX 和 RX。
- `tx_samples > 0 && rx_samples == 0`：只发送。
- `tx_samples == 0 && rx_samples > 0`：只采样接收。
- ioctl 返回时会把实际设置到的 `sample_rate` 写回结构体。
- passthrough 完成后驱动会恢复 Flexbus DShot 硬件初始化状态。

修改内核驱动后编译烧写：

```sh
./build.sh kernel && ./flash_zboot.sh -f
```

## 用户态工具

目录：

```sh
tools/rk-dshot-passthrough
```

本机编译：

```sh
cd tools/rk-dshot-passthrough
make
```

交叉编译单个工具示例：

```sh
prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-gcc \
  -O2 -Wall -Wextra \
  -o /tmp/rk-blheli32-bf4way-arm \
  tools/rk-dshot-passthrough/rk-blheli32-bf4way.c
```

### rk-blheli32-bf4way

用途：模拟 Betaflight/Cleanflight 的 MSP + serial 4way 接口，供 BLHeliSuite32 使用。

常用运行方式：

```sh
/tmp/rk-blheli32-bf4way-arm \
  -s /dev/ttyS4 \
  --host-baud 115200 \
  --esc-count 1
```

完整参数：

```text
Usage: rk-blheli32-bf4way -s /dev/ttyS4 [-d /dev/rk-flexbus-dshot]
          [--host-baud N] [--bit-us N] [--oversample N]
          [--rx-window-ms N] [--timeout-ms N] [--esc-count N]
          [--tx-invert] [--rx-invert] [-v]
```

关键参数：

- `-s /dev/ttyS4`：连接 BLHeliSuite32 的板端 UART。
- `--host-baud 115200`：BLHeliSuite32 到 daemon 的串口波特率。
- `--esc-count 1`：上报给 Suite 的 ESC 数量；当前按 1 个 ESC 测试。
- 写入相关命令默认允许执行，不再需要额外打开写入选项。
- `--bit-us 52`：默认接近 19200 baud。
- `--oversample`：每 bit 采样点数，`rk-blheli32-bf4way` 默认 32x。

当前 4way 对齐点：

- serial 4way protocol：`108`
- interface version：`20.06`
- interface name：`m4wFCIntf`
- `0xff` 在 serial 4way 命令层不作为合法命令处理，按 Betaflight 默认返回 `ACK_I_INVALID_CMD`。
- bootloader 层命令使用独立常量名，例如 `BL_CMD_SET_ADDRESS = 0xff`，避免和 serial 4way 命令层混淆。

部署并启动示例：

```sh
sshpass -p luckfox ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  root@192.168.123.100 \
  'killall rk-blheli32-bf4way-arm 2>/dev/null || true; rm -f /tmp/rk-blheli32-bf4way.log'

sshpass -p luckfox scp -O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  /tmp/rk-blheli32-bf4way-arm \
  root@192.168.123.100:/tmp/rk-blheli32-bf4way-arm

sshpass -p luckfox ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  root@192.168.123.100 \
  'chmod +x /tmp/rk-blheli32-bf4way-arm; \
   nohup /tmp/rk-blheli32-bf4way-arm -s /dev/ttyS4 --host-baud 115200 --esc-count 1 \
     >/tmp/rk-blheli32-bf4way.log 2>&1 &'
```

查看日志：

```sh
sshpass -p luckfox ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  root@192.168.123.100 \
  'tail -f /tmp/rk-blheli32-bf4way.log'
```

### rk-blheli32-info

用途：直接通过 Flexbus passthrough 与 BLHeli32 bootloader 通信，读取 boot 信息和 flash 数据。

参数：

```text
Usage: rk-blheli32-info [-d /dev/rk-flexbus-dshot] [-c 0..3]
          [--esc-baud N] [--bit-us N] [--oversample N] [--rx-window-ms N]
          [--rx-skip-ms N]
          [--timeout-ms N] [--flash-addr N] [--read-len 0..256]
          [--boot-high-ms N] [--boot-low-ms N] [--boot-zeros N]
          [--tx-invert] [--rx-invert] [-v]
```

典型用途：

```sh
./rk-blheli32-info -d /dev/rk-flexbus-dshot -c 0 --flash-addr 0xf7e8 --read-len 16
```

## BLHeliSuite32 使用流程

板端：

```sh
killall px4 2>/dev/null || true
killall rk-blheli32-bf4way-arm 2>/dev/null || true

/tmp/rk-blheli32-bf4way-arm \
  -s /dev/ttyS4 \
  --host-baud 115200 \
  --esc-count 1 \
  >/tmp/rk-blheli32-bf4way.log 2>&1 &
```

PC 端 BLHeliSuite32：

1. 选择 PC 侧串口，例如 `/dev/ttyUSB0`。
2. 选择 Betaflight/Cleanflight serial 4way/passthrough 类接口，不选 `4-way/usb_com`。
3. 连接。
4. Read setup。
5. Write setup。

预期现状：

- connect 能成功。
- read setup 能读到 ESC 类型，例如 `Hobbywing_BL32_AT421 - Rev. 32.9 - Multi`。
- write setup 实际会写入 ESC。
- write setup 后 Suite 仍可能报写入失败，这是当前未解决问题。

## 调试结论记录

已经验证过的点：

- `SET_BUFFER` 需要 header+CRC 与 payload+CRC 尽量连续发送；用户态已经合并成一次 Flexbus TX，避免 ioctl 间隔过大。
- `DeviceWrite len=0` 表示 256B，用户态 parser 已按 256B payload 读取。
- 256B 写入时 ioctl timeout 需要覆盖完整 TX/RX 时间，用户态已按窗口动态计算。
- 内核源码中的 sample 上限当前为 `524288`，`rk-blheli32-bf4way` 默认 oversample 为 32x。该内核已在板端刷写验证，启动版本为 `Linux 6.1.99 #37 SMP PREEMPT Sat Jul 11 16:47:20 CST 2026`。
- 新内核下 `rk-blheli32-info --oversample 32 --flash-addr 0xf7e8 --read-len 16 -v` 验证通过：实际 sample rate 为 `600000` Hz，可读到 bootloader `471m`、signature `0x1506`、boot ver `7`、boot pages `4`，并能读取 `0xf7e8..0xf7f7`。
- 新内核下 `rk-blheli32-bf4way` 已用默认参数启动验证：`/tmp/rk-blheli32-bf4way-arm -s /dev/ttyS4 --host-baud 115200 --esc-count 1`，不需要显式传 `--oversample 32`。
- 16x 采样实验中，内核实际 rate 会 round 到约 `375000` Hz；按实际 rate 调整 RX oversample 后仍收不到 ESC bootloader 响应。32x 时实际 rate 约 `600000` Hz，可稳定解出 bootloader 响应。因此 bf4way 默认保持 32x。
- `MSP_SET_PASSTHROUGH` 后进入 4way，`InterfaceExit` 后 daemon 回到 MSP 循环，不直接退出进程。
- `0xff` 在 serial 4way 层按 Betaflight 默认 invalid command 处理；真实 BLHeli bootloader 的 set-address 命令在内部 bootloader 层单独使用。
- 额外内部 verify 测试曾证明 BLHeli bootloader 对写入块返回 `0x30` 成功，但该 verify 不是当前保留路径。

不要再重复尝试的方向：

- 不要把 serial 4way 层的 `0xff` 转发成 bootloader `SetAddress`。实测 ESC 会回成功，但 BLHeliSuite32 写入收尾仍失败，而且这不符合 Betaflight 命令层行为。
- 不要把 `DeviceRead len=0` 的 256B 读强制改成底层 16B 分段读。实测会影响 BLHeliSuite32 read setup。
- 不要为了该问题发送 DShot 命令；当前 ESC/板端方向不适合双向 inverted DShot 调试。

## 后续待查

- 为什么真实读取 `0x7c00` 设置区不稳定，而 `0xf7e8` 读取稳定。
- BLHeliSuite32 写 setup 后反复执行 `DeviceWrite 0x7c00/256B -> 0xff -> Alive -> Read 0xf7e8/16` 的具体成功判据。
- 是否需要更精确复现 Betaflight GPIO bit-bang 的 RX 采样时序，而不是当前 Flexbus 长窗口采样。
- 是否需要抓一份真实 Betaflight passthrough 和 BLHeliSuite32 的 4way 交互日志，对比写入后的 `0xf7e8` 判断。
