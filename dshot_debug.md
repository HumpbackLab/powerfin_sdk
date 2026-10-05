# Dshot Debug 环境说明

插入sigrok fx2lafw 逻辑分析仪
使用`/home/ncer/sigrok-local/bin/sigrok-cli --driver fx2lafw --scan` 可以扫描到设备

# powerfin 的dshot电路连接

powerfin的dshot 使用rk3506的flexbus


# 飞控的连接
当前飞控的4个dshot输出，第一个m0 ，接到了AM32电调上,也接到了逻辑分析仪上。其他电机没接。

飞控设备的IP地址为：192.168.123.100
用户名：root
密码：powerfin

登录到飞控设备后，执行：
```
cd px4
./bin/px4 -d -s posix-configs/powerfin/px4_mc.config &
```
来启动飞控程序, 默认已经设置为开机自启动，可以不需要手动执行。
等待500ms之后，就可以控制dshot输出了。

可以在PX4内，通过actuator 测试，让电机转。PX4代码：/home/ncer/PX4-Autopilot

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

# 当前问题
接入Blheli 电调（使用Bluejay 固件，无回传），no response = 0xf（表示4个电机都没有）

# TIPS
抓波形的时候，建议用triger的方式来抓.
建议不需要让电机转，因为当前只是要调试回传问题。默认即使发0油门，对面的回传虽然转速是0，也不应该是no response吧

