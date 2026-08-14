#let doc-version = "v1.2"
#let update-date = "2026 年 8 月 14 日"

#set document(title: "PowerFin 飞控用户手册", author: "Humpbacklab")
#set page(
  paper: "a4",
  margin: (x: 2.5cm, y: 2.5cm),
  header: context {
    if counter(page).get().first() > 1 {
      set text(8pt, gray)
      grid(
        columns: (1fr, 1fr),
        [PowerFin 飞控用户手册],
        align(right)[版本：#doc-version],
      )
      v(-.5em)
      line(length: 100%, stroke: .5pt + gray)
    }
  },
  footer: context [
    #align(center, text(9pt, gray)[第 #counter(page).display() 页])
  ],
)
#set text(
  font: ("Noto Sans", "Noto Sans CJK SC"),
  size: 11pt,
  lang: "zh",
  region: "cn",
)
#set par(leading: .8em, spacing: 1.2em)
#set heading(numbering: "1.1")
#set list(indent: 1.2em, body-indent: .55em, spacing: .35em)
#set enum(indent: 1.2em, body-indent: .55em, spacing: .45em)

#show heading: set block(above: 1.2em, below: .6em)
#show outline.entry: set par(leading: 1.2em)

#let callout(title, kind: "info", body) = {
  let is-caution = kind == "danger" or kind == "warning"
  let accent = if is-caution { red } else { blue }
  let background = if is-caution { rgb("#fff5f5") } else { rgb("#f0f8ff") }
  block(
    width: 100%,
    inset: 12pt,
    radius: 4pt,
    fill: background,
    stroke: (left: 4pt + accent),
  )[
    *#title：* #body
  ]
}

// Cover
#align(center + horizon)[
  #block(inset: 3em)[
    #text(28pt, weight: "bold", fill: navy)[PowerFin]
    #v(.4em)
    #text(18pt, weight: "medium")[PowerFin 飞控用户手册]
    #v(1.2em)
    #text(11pt, gray)[Linux 飞控配置、接线与固件维护]
  ]
  #image("powerfin_top.png", width: 100%)
]

#v(1fr)
#align(center)[
  #text(10pt, gray)[文档版本：#doc-version | 最后更新：#update-date]
  #linebreak()
  #text(10pt, gray)[适用硬件：PowerFin Linux 飞控]
]

#pagebreak()

#outline(indent: 2em, depth: 2)

#pagebreak()

= 产品概述

PowerFin 是 Humpbacklab 面向 PX4 的 Linux 飞控。板载 Linux 负责设备驱动、网络与维护服务，PX4 负责飞行控制；Penguin Flight Console 通过 USB RNDIS 提供浏览器配置入口。

== 主要规格

#table(
  columns: (32mm, 1fr),
  inset: 8pt,
  align: horizon,
  [*项目*], [*PowerFin 配置*],
  [处理器], [Rockchip RK3506G2 Linux SoC],
  [飞控软件], [PX4，默认运行目录 `/root/px4`],
  [惯性传感器], [SC7U22，SPI 接口],
  [气压计], [SPA06，I²C 接口],
  [磁力计], [QMC5883P，I²C 接口],
  [电机输出], [4 路 DShot，默认 DShot600，支持双向遥测],
  [安装孔距], [30.5 mm × 30.5 mm（孔中心距）],
  [外部总线], [UART、I²C、SPI、CAN、USB],
  [存储], [microSD 卡与板载 SPI Flash],
  [维护连接], [USB-C RNDIS 网络；设备地址 `192.168.123.100`],
)

= 安全须知

#callout("注意", kind: "danger")[配置、升级或测试飞控前，必须拆下螺旋桨并确保动力系统不会意外启动。更新 PX4 或烧写内核时，应使用稳定电源；操作完成前不要断电、拔掉 USB 或关闭页面。]

- 接线前确认接口定义和电压，不能只凭插头外形或线色判断。
- 只使用与 PowerFin 匹配的 PX4 升级包和 Linux 内核镜像。
- 首次安装或更新固件后，先进行静态检查和无桨地面测试。

= 硬件概览及接口

== 安装方向

正面白色箭头为机头方向。安装时让箭头指向飞行器前方，并在 PX4 中检查传感器方向。四角安装孔应使用合适的减振结构，避免板体弯曲或与导电部件接触。

#pagebreak()

== 正面接口图

#figure(
  image("assets/powerfin_interfaces_top.png", width: 88%),
  caption: [PowerFin PCB 正面接口标注],
)

#callout("针脚方向")[内测版的的I2C1 SCL与SDA丝印标注错误，以本文档的接口图为准]

#pagebreak()

== 背面接口图

#figure(
  image("assets/powerfin_interfaces_bottom.png", width: 88%),
  caption: [PowerFin PCB 背面接口与安装孔距标注],
)

#pagebreak()

== 正面连线图

#figure(
  image("assets/PowerFin_connect_top.png", width: 140%),
  caption: [PowerFin PCB 正面连线图],
)
#pagebreak()

== 背面连线图

#figure(
  image("assets/PowerFin_connect_bottom.png", width: 130%),
  caption: [PowerFin PCB 背面连线图],
)

#pagebreak()

= 快速开始

== 烧录文件系统

1. 插入SD卡到飞控
2. 通过USB连接电脑，电脑会识别一个RNDIS/USB网络设备
3. 打开浏览器，访问 #link("http://192.168.123.100:8080")
4. 上传文件系统镜像powerfin-sdcard.img.gz，写入完成后，点击右上角重启设备

== 通过 USB 连接

1. 给 PowerFin 上电，等待系统启动完成。
2. 用 USB 数据线连接 PowerFin 和电脑。电脑会识别一个 RNDIS/USB 网络设备。
3. 等待电脑自动获得 `192.168.123.2` 至 `192.168.123.20` 范围内的地址。
   通常不需要手动配置网络。
4. 打开浏览器，访问 #link("http://192.168.123.100:8080")[`http://192.168.123.100:8080`]。

#callout("连接参数")[
#table(
  columns: (1fr, 1.8fr),
  inset: 8pt,
  align: horizon,
  [*项目*], [*值*],
  [飞控地址], [`192.168.123.100`],
  [网页端口], [`8080`],
)
]


= Wi-Fi 配置

#callout("注意")[在单独使用USB线给飞控供电的情况下，可能会出现WIFI连接不稳定或者无法连接的情况，建议要使用Wi-Fi时，使用电池供电。]

Wi-Fi 配置保存后不会立即切换，必须重启设备才会生效。USB RNDIS 与 Wi-Fi 相互独立，即使关闭 Wi-Fi，仍可通过 USB 打开控制台。

== AP 热点模式

AP 模式让 PowerFin 创建自己的 Wi-Fi 热点。

1. 在“Wi-Fi 工作模式”中选择“热点模式（AP）”。
2. 填写热点名称和热点密码。默认名称为 `PowerFin`，默认密码为 `powerfin`。
3. 点击“保存 Wi-Fi 配置”。
4. 看到“配置已保存”后，点击右上角“重启设备”。

热点名称最长 32 个字符；密码必须为 8 至 63 个字符。

热点模式下，
飞控的管理界面地址为:192.168.4.1:8080


== STA 客户端模式

STA 模式让 PowerFin 使用系统中已有的客户端网络配置连接其他 Wi-Fi。
选择“客户端模式（STA）”，保存配置并重启设备即可。

== 关闭 Wi-Fi

选择“关闭 Wi-Fi（OFF）”，保存并重启。设置生效后，PowerFin 下次启动不会自动开启 Wi-Fi，但 USB RNDIS 仍然可用。

= 使用 QGroundControl 配置飞控

QGroundControl（简称 QGC）用于配置 PX4 的机架、传感器、遥控器、飞行模式、执行器和安全策略。Penguin Flight Console 用于维护 PowerFin 的 Linux、网络和 PX4 程序，两者用途不同。

#callout("固件更新方式与重启方式", kind: "danger")[PowerFin无法时使用QGC的固件更新方式与重启，请使用 Penguin Flight Console 的 PX4 管理页面进行更新和重启。]

== 安装并连接 QGC

1. 从 #link("https://docs.qgroundcontrol.com/Stable_V5.0/zh/qgc-user-guide/getting_started/download_and_install.html")[*QGroundControl 官方下载页面*]安装桌面版 QGC。
2. 拆下全部螺旋桨，给 PowerFin 稳定供电。
3. 用 USB 数据线连接 PowerFin 和电脑，等待 RNDIS 网络连接完成。
4. 确认 Penguin Flight Console 中 PX4 正在运行，然后启动 QGC。
5. QGC 默认会通过 UDP 自动发现 PowerFin。连接成功后，顶部工具栏会出现飞行器状态，进入“Q 图标 → Vehicle Configuration（飞行器设置）”可看到 PX4 配置页面。

#callout("网络连接")[PowerFin 会通过 USB RNDIS 网络向 QGC 的标准 UDP 端口发送 MAVLink 数据，正常情况下不需要填写 IP 地址或手动建立连接。使用 Wi-Fi 时，电脑和 PowerFin 也必须位于可互通的网络中。]

== 首次配置顺序

新装、恢复参数或更换主要硬件后，建议按以下顺序配置：
1. 配置设备连接端口
2. Sensors（传感器）
3. Radio（遥控器）
4. Flight Modes（飞行模式）
5. Actuators / Motors（执行器与电机）
6. 重启 PX4 并完成无桨检查

QGC 左侧配置项或 Summary（摘要）中出现红色标记，表示还有必需配置未完成。红色标记清除前不要安装螺旋桨或尝试飞行。

== 配置设备连接端口

默认分配：

MSP_OSD_CONFIG->uart2(MSP_OSD)
RC_PORT_CONFIG->uart3(串口接收机，sbus/crsf)

如需修改：
Vehicle Configuration - 参数 - RC_PORT_CONFIG/MSP_OSD_CONFIG，选择对应的端口。修改后需要重启PX4。

端口对应关系:

- TELM1->uart1
- TELM2->uart2
- TELM3->uart3
- TELM4->uart4
- TLEM5->uart5

GPS使用GPS_1_CONFIG配置，其他mavlink传感器，也有对应的参数，详细需参考PX4的官方文档。


== 校准传感器

进入“Vehicle Configuration → Sensors”，按照页面引导完成可用项目：

- *方向*：PowerFin 正面白色箭头应指向机头。正常正向安装时不要额外设置旋转；如飞控不能正向安装，应先设置正确的 Autopilot Orientation（飞控方向），再进行校准。
- *IMU、电子罗盘*：按 QGC 图示依次放置各个朝向，等待当前步骤完成后再移动。
- *水平校准*：将整机放在代表正常平飞姿态的水平面上，再执行 Level Horizon（水平姿态）校准。

校准完成后，回到 QGC 飞行界面，轻微倾斜和转动飞行器，确认姿态地平线与机体动作方向一致；静止时角速度和姿态不应持续明显漂移。

== 配置遥控器

先按照本手册接口图和接收机说明书接线，再进行配置。不要仅凭线色判断电源和信号针脚。

1. 给遥控器和接收机上电，确认两者已经对频。
2. 进入“Vehicle Configuration → Radio”。
3. 点击校准，按 QGC 提示移动油门、横滚、俯仰和偏航摇杆以及所有要使用的开关。
4. 完成后观察通道监视器：摇杆居中值、端点和方向应正确，每个开关只改变预期通道。

== 设置飞行模式

进入“Vehicle Configuration → Flight Modes”，选择模式通道，并为遥控器开关的各个位置分配飞行模式。

- 首次调试至少保留一个可直接切换的手动辅助模式，例如 Stabilized（自稳）。
- 只有 GPS、罗盘和位置估计已经正常时，才使用 Position（位置）、Mission（任务）或 Return（返航）。
- 建议给独立开关分配 Return（返航）；Kill Switch（急停）会立即停止全部电机，使用前必须理解其后果，并避免误触。
- 逐一拨动开关，确认 QGC 高亮的模式与开关位置一致。

== 检查执行器和电机

#callout("必须拆桨", kind: "danger")[进入 Actuators / Motors 页面前必须拆下全部螺旋桨、固定机体并清空电机周围。电机测试会真实驱动输出。]

1. 进入“Vehicle Configuration → Actuators”或“Motors”。不同 QGC 版本名称可能略有不同。
2. 根据所选机架检查 Motor 1 至 Motor 4 的输出分配，不要用交换机架类型的方法修正单个电机接线。
3. 按 QGC 的安全确认要求启用执行器测试，一次只测试一个电机。
4. 对照 QGC 机架图确认实际转动的电机编号和旋转方向。
5. 如编号错误，修正输出映射或信号接线；如旋转方向错误，按电调说明修改方向。完成修改后重新逐个测试。

PowerFin 默认提供 4 路 DShot 输出。不得将舵机或普通 PWM 设备直接接到 DShot 电机输出并按默认配置驱动。

== 设置安全与失控保护

进入“Vehicle Configuration → Safety”，至少检查以下项目：

- 遥控链路丢失后的动作和触发时间；
- 低电量、严重低电量的阈值与动作（仅在电源监测可靠时）；
- 数传链路丢失后的动作；
- Return（返航）高度和降落行为；
- 地理围栏（如任务需要）。

返航和位置保持依赖可靠的定位、Home 点和高度信息。设置完成后，应在拆桨状态下模拟关闭遥控器或断开数传，并观察 QGC 提示和 PX4 状态是否符合预期。不要为了消除解锁报错而关闭传感器检查或失控保护。

== 保存参数并完成地面检查

QGC 中的大多数 PX4 参数会在修改时自动保存。完成全部配置后：

1. 在 Parameters（参数）页面使用保存到文件功能备份参数，并记录对应的机架、接收机、电源模块和 PX4 版本。
2. 通过 Penguin Flight Console 重启 PX4，再重新连接 QGC。
3. 确认 Summary 中没有未处理的红色项目，并查看顶部状态和消息列表中的告警原因。
4. 检查姿态方向、遥控通道、飞行模式、GPS/罗盘、电池显示和失控保护。
5. 保持拆桨，执行一次解锁、各电机低速输出和上锁检查。
6. 安装螺旋桨前，再次核对每个电机的编号、旋转方向和桨叶方向。

#callout("首次飞行", kind: "warning")[QGC 配置完成不代表飞行器已经完成整机验证。首次飞行应在空旷、合法的区域进行，使用保守模式和高度，并由具备相应经验的人员操作。]

== 常见问题
- 起飞机体抖动严重，无法起飞：降低PID参数的D值(MC_PITCHRATE_D、MC_ROLLRATE_D)

= 飞控维护

#callout("维护安全", kind: "danger")[停止、重启、更新 PX4 或烧写内核前，拆下螺旋桨并保持稳定供电；操作完成前不要断电或拔掉 USB。]

== 管理 PX4

控制台会显示当前 PX4 版本；更新后可点击版本旁的刷新按钮确认。PX4 默认安装在 `/root/px4`。

#table(
  columns: (28mm, 1fr),
  inset: 8pt,
  align: horizon,
  [*按钮*], [*作用*],
  [启动 PX4], [启动飞控程序。],
  [停止 PX4], [停止飞控程序。],
  [重启 PX4], [只重启 PX4，不重启 Linux。],
)

== 更新 PX4

使用 PowerFin 专用的 `px4-xxxx.zip` 升级包，文件不得超过 128 MiB，也不要修改压缩包内部目录。

1. 在“PX4 管理”中选择升级包。
2. 点击“更新 PX4”并确认。
3. 等待页面提示更新成功，再检查版本。

更新时控制台会自动停止并重新启动 PX4；如果新版本启动失败，会尝试恢复旧版本。

== 烧写 Linux 内核

#callout("注意", kind: "danger")[大部分情况下不需要用户更新内核，除非新版本发布时明确要求用户更新内核。]


使用 PowerFin SDK 生成的 `zboot.img`。普通模式会检查镜像中的 kernel 和设备树，并在写入后执行 SHA-256 回读校验。

1. 在“内核烧写”中选择 `zboot.img`。
2. 保持“强制模式”未勾选，点击“烧写内核”。
3. 校验成功后点击“重启设备”。



== 重启设备

右上角“重启设备”会同时重启 Linux 和 PX4。USB 网络会暂时断开，RNDIS 恢复后刷新控制台即可。