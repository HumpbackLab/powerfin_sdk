# PowerFin 普通 DShot overlay

默认设备树继续使用上拉、`rockchip,dshot-polarity = "invert"`。
`rk3506-powerfin-dshot-normal.dtbo` 将 GPIO1_D3、D2、D1、D0 的
FlexBUS0 数据引脚改为下拉，并设置 `rockchip,dshot-polarity = "normal"`。
它不改变 DShot 速率，也不启用被其他配置禁用的 FlexBUS 节点。

驱动在 `normal` 模式下使用普通校验和和 TX-only，空闲电平为低。
设备树中继承的 `rockchip,telemetry` 和用户态设置回传的请求均不会启用
双向收发；`RK_DSHOT_IOC_TELEMETRY_XFER` 返回 `-EOPNOTSUPP`。
现有 PX4 `flexbus_dshot` 收到该返回值后会回退到 `SEND_FRAME`，因此
启动参数中保留 `-t` 也不会使普通 DShot 变为双向模式。
`invert` / `inverted` 模式仍可通过原有 telemetry 开关控制回传。
原始 passthrough 接口不受此 DShot 协议模式限制。

## 构建和选择

```sh
./build.sh kernel-make:rk3506-powerfin-dshot-normal.dtbo
./build.sh kernel
```

首次使用需要更新内核，因为旧驱动不会根据 normal polarity 禁止回传。
构建流程会将 overlay 打包为 FIT 的 `dshot-normal` 配置。
默认 `conf-normal` 不自动应用它。FIT 已加载到 U-Boot 的 `${loadaddr}` 后：

```text
bootm ${loadaddr}#conf-normal#dshot-normal
```

恢复环境可使用 `#conf-recovery#dshot-normal`。可与 `#spi1-pwm` 组合；
不要与 `#motor-pwm` 组合，因为后者会禁用 FlexBUS。
PFC 网页可保存 `powerfin_dshot_mode=normal` 到 boardcfg；配套新版 U-Boot 会在
正常启动时选择此 overlay。升级 PFC 时需同时保证 U-Boot 支持该变量。
默认未启用时仍使用反相 DShot；Recovery 不自动应用用户 overlay。

## 离线验证

```sh
fdtoverlay -i kernel-6.1/arch/arm/boot/dts/rk3506-powerfin-spinor.dtb \
  -o /tmp/powerfin-dshot-normal.dtb \
  kernel-6.1/arch/arm/boot/dts/rk3506-powerfin-dshot-normal.dtbo
fdtget /tmp/powerfin-dshot-normal.dtb /flexbus@ff880000/dshot rockchip,dshot-polarity
```

应输出 `normal`。四路 `rockchip,pins` 均引用 overlay 的
`/pinctrl/dshot-normal-pull-down` 节点，该节点含 `bias-pull-down`。
启动后 `telemetry` sysfs 应为 `0`；通过逻辑分析仪检查低电平空闲和
普通 DShot 帧。加载 overlay 和驱动变化需要在启动时生效。
