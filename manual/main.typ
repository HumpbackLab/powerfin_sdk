#let doc-version = "v1.1"
#let update-date = "2026 年 8 月 4 日"

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

#callout("规格边界", kind: "warning")[本手册只列出已由 SDK 配置和板上丝印确认的信息。整板输入电压范围、各电源口最大持续电流、外形尺寸和重量尚未获得硬件规格文件确认，接线时不得自行推断。]

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

#callout("针脚方向")[每个插座旁的白色小三角表示丝印序列的起始端。制作线束时应同时核对三角标记和每根线对应的丝印，不能只凭插头外形判断方向。图内 `NC` 表示保留针脚，不连接。]

#pagebreak()

== 背面接口图

#figure(
  image("assets/powerfin_interfaces_bottom.png", width: 88%),
  caption: [PowerFin PCB 背面接口与安装孔距标注],
)

#pagebreak()

= 快速开始

#block(
  fill: rgb("#e8f5e9"),
  stroke: (left: 4pt + green),
  inset: 12pt,
  radius: 4pt,
  width: 100%,
)[
  *首次使用最短路径*：上电 → 通过 USB RNDIS 连接 → 打开配置页面 →
  检查设备与 PX4 版本 → 按需配置 Wi-Fi 或更新固件 → 重启设备 →
  拆桨完成地面检查。
]

== 需要准备

- 一块已正常启动的 PowerFin 飞控。
- 一根支持数据传输的 USB 线。仅能充电的线无法使用。
- 一台装有现代浏览器的电脑，推荐 Chrome、Edge 或 Firefox。
- 如需更新固件，提前准备正确的 PX4 升级包或 `zboot.img`。

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
  [访问协议], [HTTP，不是 HTTPS],
  [电脑地址], [由飞控自动分配],
)
]

== 判断是否连接成功

页面顶部会显示：

- 飞控型号和板型标识；
- Linux 内核版本；
- 设备树标识；
- 绿色“已连接”状态。

如果页面显示“连接失败”或一直停留在“连接中”，请参考第 9 章“故障排查”。

= Wi-Fi 配置

Wi-Fi 配置保存后不会立即切换，必须重启设备才会生效。USB RNDIS 与 Wi-Fi 相互独立，即使关闭 Wi-Fi，仍可通过 USB 打开控制台。

== AP 热点模式

AP 模式让 PowerFin 创建自己的 Wi-Fi 热点。

1. 在“Wi-Fi 工作模式”中选择“热点模式（AP）”。
2. 填写热点名称和热点密码。默认名称为 `PowerFin`，默认密码为 `powerfin`。
3. 点击“保存 Wi-Fi 配置”。
4. 看到“配置已保存”后，点击右上角“重启设备”。

热点名称最长 32 个字符；密码必须为 8 至 63 个字符。

== STA 客户端模式

STA 模式让 PowerFin 使用系统中已有的客户端网络配置连接其他 Wi-Fi。

#callout("当前限制", kind: "warning")[当前网页只负责切换到 STA 模式，不提供 STA 网络名称和密码编辑。切换前应确保系统中已经存在可用的 STA 配置。]

选择“客户端模式（STA）”，保存配置并重启设备即可。

== 关闭 Wi-Fi

选择“关闭 Wi-Fi（OFF）”，保存并重启。设置生效后，PowerFin 下次启动不会自动开启 Wi-Fi，但 USB RNDIS 仍然可用。

= PX4 管理

PowerFin 的 PX4 默认安装在 `/root/px4`，该路径由板型配置决定，用户无需修改。

== 查看版本

控制台打开时会自动读取一次 PX4 版本。版本右侧的刷新按钮可重新读取版本，适合在更新 PX4 后确认新版本。

== 启动、停止和重启

#table(
  columns: (28mm, 1fr),
  inset: 8pt,
  align: horizon,
  [*按钮*], [*作用*],
  [启动 PX4], [启动当前安装的 PX4 飞控程序。],
  [停止 PX4], [停止飞控程序。停止后飞控功能不可用。],
  [重启 PX4], [只重启 PX4，不重启 Linux 设备。],
)

#callout("飞行安全", kind: "danger")[请勿在飞行中或电机已解锁时停止、重启或更新 PX4。执行这些操作前，应拆除桨叶并确保飞控处于安全状态。]

= 更新 PX4

== 升级包要求

请选择由 PowerFin PX4 构建流程生成的升级包：

- 文件名格式为 `px4-xxxx.zip`，其中 `xxxx` 通常是 Git 提交号；
- 压缩包内应包含完整的 `px4/` 运行目录；
- 不要手动改变压缩包内的目录层级；
- 最大上传大小为 128 MiB。

== 更新步骤

1. 保持 PowerFin 稳定供电，并确认当前没有飞行任务。
2. 在“PX4 管理”区域点击“选择升级包”。
3. 选择正确的 `px4-xxxx.zip`。
4. 点击“更新 PX4”，核对弹窗中的文件名和安装目录后确认。
5. 等待页面显示“PX4 更新成功并已启动”。页面随后会自动刷新版本。

更新过程会停止 PX4、替换运行目录并启动新版本。如果新版本无法启动，系统会尝试恢复并重新启动原版本。

#callout("更新过程中", kind: "danger")[不要断电、拔掉 USB、重复点击更新按钮或关闭浏览器页面。]

= 烧写 Linux 内核

内核烧写使用 PowerFin SDK 生成的 `zboot.img`，目标为设备的 boot MTD 分区。烧写成功后需要手动重启设备。

== 普通模式

普通模式会确认上传文件是 FIT 镜像，并且同时包含：

- Linux `kernel`；
- 设备树 `fdt`。

完成写入后，系统会读取已写入的内容并进行 SHA-256 校验。

1. 准备由正确 PowerFin SDK 生成的 `zboot.img`。
2. 在“内核烧写”区域点击“选择 zboot.img”。
3. 保持“强制模式”未勾选。
4. 点击“烧写内核”，阅读警告后确认。
5. 等待页面显示“内核烧写并校验成功”。
6. 点击右上角“重启设备”，等待 USB 网络重新连接。

== 强制模式

强制模式跳过 FIT 格式检查，直接把所选文件写入 boot MTD 分区。写入后的 SHA-256 回读校验仍会执行。

#callout("高风险功能", kind: "danger")[强制模式可能把错误文件写入启动分区，导致设备无法启动。只有在普通模式无法识别镜像、且你已经确认文件及目标板型完全正确时才可使用。]

强制烧写的操作步骤与普通模式相同，但需要先勾选“强制模式”，并再次确认风险提示。

= 重启设备

页面右上角的“重启设备”会重启整个 PowerFin，包括 Linux 和 PX4。以下情况需要重启：

- 保存 Wi-Fi 工作模式后；
- 内核烧写成功后；
- 系统工作异常，需要完整重启时。

点击后 USB 网络会断开。等待 PowerFin 完成启动和电脑重新识别 RNDIS，然后刷新 `http://192.168.123.100:8080`。

= 故障排查

== 无法打开网页

#table(
  columns: (1fr, 2.2fr),
  inset: 8pt,
  align: horizon,
  [*检查项*], [*处理方法*],
  [USB 线], [更换确认支持数据传输的 USB 线，并尝试电脑上的其他 USB 接口。],
  [启动时间], [上电后等待约 10 秒，再刷新页面。],
  [访问地址], [确认使用 `http://192.168.123.100:8080`，不要使用 HTTPS。],
  [RNDIS 设备], [检查电脑是否出现新的 USB/RNDIS 网络适配器。],
  [网络冲突], [暂时断开占用 `192.168.123.0/24` 网段的 VPN 或其他网络。],
  [浏览器缓存], [使用无痕窗口或换一个浏览器重试。],
)

== PX4 版本读取失败

- 确认 PX4 已正确安装在页面显示的目录中；
- 如果刚完成更新，点击版本旁边的刷新按钮；
- 尝试“启动 PX4”或“重启 PX4”；
- 记录页面显示的完整错误信息并联系技术支持。

== PX4 更新失败

- 确认文件名符合 `px4-xxxx.zip`；
- 确认升级包为 PowerFin 版本，不是其他板型；
- 不要手动修改压缩包内部目录；
- 保持供电后重新选择文件再试一次；
- 如果提示新版本启动失败，系统通常已经恢复旧版本，可刷新版本确认。

== 内核烧写失败

- 优先使用普通模式和 SDK 原始生成的 `zboot.img`；
- 确认文件上传没有中断；
- 如果 SHA-256 回读校验失败，不要重启，保持设备供电并联系技术支持；
- 不要为了绕过未知错误而随意启用强制模式。

== 重启后暂时无法连接

重启时 RNDIS 网卡会从电脑中消失并重新出现。等待电脑重新获取地址后刷新页面。如果超过 30 秒仍无法连接，重新插拔 USB 线并再次访问控制台。

= 维护建议

- 只使用 Humpbacklab 发布或由匹配 SDK 构建的固件。
- 更新前记录当前 Linux 内核版本和 PX4 版本。
- 升级文件应保留原始文件名，避免混淆板型和版本。
- 烧写和更新时使用稳定电源，不要依赖接触不良的 USB 接口。
- 更新完成后先进行静态检查和无桨测试，再恢复飞行。
- 遇到问题时记录飞控型号、版本、文件名和页面完整错误信息。
