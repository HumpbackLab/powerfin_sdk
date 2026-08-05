# Dshot Debug 环境说明

插入sigrok fx2lafw 逻辑分析仪
使用`/home/ncer/sigrok-local/bin/sigrok-cli --driver fx2lafw --scan` 可以扫描到设备

# powerfin 的dshot电路连接

powerfin的dshot 使用rk3506的flexbus

为了支持双向dshot，使用flexbus0 作为发送，使用flexbus1作为接收

flexbus0与dshot通道的对应关系：
d0-m0
d1-m1
d2-m2
d3-m3

并且由于flexbus0的引脚只是普通IO口，不支持双向，在flexbus0 的d0-d3上，连接二极管（阴极朝flexbus0的引脚），并在二极管后面连接一个上拉电阻(2K)到3.3v，
并将二极管阳极连接到flexbus1的数据线上和电调的信号线上，用这种方式来实现双向。

flexbus1与dshot通道的对应关系：
d0-m3
d1-m2
d2-m1
d3-m0


# 飞控的连接
当前飞控的4个dshot输出，第一个m0 ，接到了AM32电调上
m1-m3，接到了逻辑分析的d1-d3

飞控设备的IP地址为：192.168.123.100
用户名：root
密码：luckfox

登录到飞控设备后，执行：
```
cd px4
./bin/px4 -d -s posix-configs/powerfin/px4_mc.config &
```
来启动飞控程序。
等待500ms之后，就可以控制dshot输出了。

还是在px4目录下，
执行：`./bin/px4-flexbus_dshot  cmd  -m 0 -c 2` 
就可以给m0 发命令2
当然，也可以执行`./bin/px4-flexbus_dshot --help`  获取帮助

驱动会在每次发送时通过 flexbus1 同步采样双向 DShot 回传。可以直接查看最近一次
通过校验的电气转速（单位 eRPM，依次为 m0-m3）：
```
cat /sys/class/misc/rk-flexbus-dshot/motor_erpm
cat /sys/class/misc/rk-flexbus-dshot/telemetry_status
```
`valid` 和 `no_response` 是通道位图。机械转速需要结合电机磁极数计算：
`RPM = eRPM * 2 / 磁极数`。

# 替换内核
如果修改了flexbus-dshot的驱动，需要重新编译并烧写内核，可以执行：
`./build.sh kernel && ./flash_zboot.sh -f`

如果只是修改了设备树，重新编译设备树后，执行
`./repack_powerfin_zboot.sh && ./flash_zboot.sh -f`


# TIPS
可以用m1-m3 输出dshot波形，然后通过逻辑分析看看波形是否合理。然后用m0实际测试
抓波形的时候，建议用triger的方式来抓

