# GPIO3_A1 驱动示例（泰山派 RK3566）

用 out-of-tree 方式控制 **GPIO3_A1**：内核字符驱动 + 设备树 overlay + 命令行小工具。
所有代码都在本工程目录 `/build/work/gpio3a1` 内（`/build/work` 只作为多项目容器，每个项目是独立 git 仓库），SDK 源码零修改（只读取 SDK 的头文件、设备树 include 路径和交叉工具链）。

## 1. 目录

```
gpio3a1/
├── Makefile                   # 统一构建入口，产物写入 out/
├── drivers/gpio3a1/
│   ├── gpio3a1.c              # platform + misc 字符驱动，/dev/gpio3a1
│   └── Kbuild                 # obj-m 片段，由顶层 Makefile 调用
├── dts/
│   ├── gpio3a1-overlay.dts    # 主用：/plugin/ overlay，引用 &gpio3、&pinctrl
│   └── tspi-rk3566-gpio3a1.dts   # 备用：整颗板级 DTS 包装
├── apps/gpio3a1_cli/
│   └── gpio3a1_cli.c          # get / set / toggle
├── scripts/
│   ├── verify_dtbo.sh         # 用 fdtoverlay 对基准 dtb 验证 overlay
│   ├── install_to_board.sh    # ssh/scp 部署到板子
│   └── set_overlay.sh         # 板端登记 ubootEnv overlay
├── out/                       # 构建产物（make 生成，gitignore）
│   ├── gpio3a1.ko
│   ├── gpio3a1.dtbo
│   ├── gpio3a1_cli
│   └── tspi-rk3566-gpio3a1.dtb   # 仅 make dtb 时生成
├── build/                     # 中间文件（自动创建/删除，gitignore）
└── docs/gpio3a1.md            # 本文
```

## 2. 硬件与设备树

- GPIO3_A1 = GPIO bank 3、group A、pin 1，对应 `RK_PA1`（值 1），Linux GPIO 编号 `3*32+1 = 97`。
- 设备树节点：

```dts
gpio3a1: gpio3a1 {
        compatible = "work,gpio3a1";
        status = "okay";
        gpios = <&gpio3 RK_PA1 GPIO_ACTIVE_HIGH>;
        pinctrl-names = "default";
        pinctrl-0 = <&gpio3a1_pin>;
};
```

- pinctrl 组节点必须放在**功能节点**下面（`/pinctrl/<function>/<group>`）：

```dts
&pinctrl {
        gpio3a1 {
                gpio3a1_pin: gpio3a1-pin {
                        rockchip,pins = <3 RK_PA1 RK_FUNC_GPIO &pcfg_pull_none>;
                };
        };
};
```

  如果直接把组节点挂在 `/pinctrl` 下，驱动 probe 时会报
  `rockchip-pinctrl pinctrl: unable to find group for node gpio3a1-pin`，
  平台设备绑定失败，也就不会生成 `/dev/gpio3a1`。

- overlay 里的 `&gpio3`、`&pinctrl`、`&pcfg_pull_none` 都是 SDK 基准设备树
  （`kernel/arch/arm64/boot/dts/rockchip/rk356x.dtsi`、`rk3568-pinctrl.dtsi`）里已有的标签。
  编译 `.dtbo` 时 dtc 生成 `__fixups__`，应用时从基准 dtb 的 `__symbols__` 解析。
  SDK 的 `rockchip_linux_defconfig` 打开了 `CONFIG_DTC_SYMBOLS=y`，
  所以 U-Boot 打出来的基 dtb 自带符号表，overlay 才能按标签引用 SDK 节点。

> `OF_OVERLAY`/configfs 运行时叠加在本 SDK 的内核配置里没有打开，所以 overlay
> 走 SDK U-Boot 自带的 `/boot/ubootEnv.txt` 机制，在启动时合并进 FDT；
> 也可以用备用的整颗 `.dtb` 替换 boot.img 里的 dtb。

## 3. 编译

所有构建都通过工程根目录的顶层 `Makefile` 完成，最终产物统一写入 `out/`；
Kbuild、cpp、dtc 产生的中间文件放在 `build/` 下，目标结束时会自动删除。

先准备一次内核（生成 `.config`、`Module.symvers` 和基准 dtb；只产生编译产物，不改源码）：

```bash
cd /build/sdk
./build.sh lunch:rockchip_rk3566_taishanpi_1m_v10_defconfig
./build.sh kernel
```

然后：

```bash
cd /build/work/gpio3a1
make                    # .ko + .dtbo + CLI -> out/
make dtb                # 可选：整颗 dtb -> out/tspi-rk3566-gpio3a1.dtb
make verify             # 无需板子，验证 overlay 能叠加
make clean              # 删除 out/ 与 build/
```

`make` 后 `out/` 中只有最终产物：

```
out/gpio3a1.ko
out/gpio3a1.dtbo
out/gpio3a1_cli
```

`verify_dtbo.sh`（`make verify` 调用）会用 `fdtoverlay` 把 `out/gpio3a1.dtbo`
叠到 SDK 基准 dtb 上，并打印合并后的 `gpio3a1` 节点，确认标签引用正确；
临时合并的 dtb 用完即删，不会留在 `out/`。

可用变量覆盖默认路径，例如 `make SDK=/path/to/sdk OUT=/tmp/out`；
`make help` 会列出全部目标和变量。

## 4. 部署到板子

方式 A（推荐，不动 boot.img）：SDK U-Boot 支持从 rootfs 读 `/boot/ubootEnv.txt`：

```bash
BOARD=root@192.168.1.100 /build/work/gpio3a1/scripts/install_to_board.sh
```

脚本会：

1. 把 `gpio3a1.ko`、`gpio3a1.dtbo`、`gpio3a1_cli` 拷到板子 `/root/gpio3a1/`；
2. 把 dtbo 放进 `/boot/overlays/gpio3a1.dtbo`；
3. 在 `/boot/ubootEnv.txt` 里登记 `overlays=gpio3a1.dtbo`；
4. 把 CLI 安装为 `/usr/bin/gpio3a1`。

然后重启板子，加载驱动：

```bash
reboot
insmod /root/gpio3a1/gpio3a1.ko
```

方式 B（备用）：`make dtb` 生成 `out/tspi-rk3566-gpio3a1.dtb`，用它替换 boot.img 中的 dtb 后烧录。
适合不重启 U-Boot overlay 机制、或要固定进固件的场景。

## 5. 使用

```bash
gpio3a1 get        # GPIO3_A1 = 0
gpio3a1 set 1      # 输出高
gpio3a1 set 0      # 输出低
gpio3a1 toggle     # 翻转并回读
```

也可以直接用设备节点：

```bash
cat /dev/gpio3a1
echo 1 > /dev/gpio3a1
echo 0 > /dev/gpio3a1
```

驱动加载成功时 `dmesg` 会打印：

```
gpio3a1 gpio3a1: GPIO3_A1 ready: /dev/gpio3a1, current level 0
```

## 6. 排查

| 现象 | 原因 / 处理 |
|---|---|
| `insmod` 报 `unknown symbol` / vermagic 不匹配 | 用的不是同一份内核，重新 `./build.sh kernel`，并确保 `.ko` 与板上内核版本一致 |
| `insmod` 成功但 `/dev/gpio3a1` 不存在 | 设备树节点没生效：确认 `/boot/ubootEnv.txt` 有 `overlays=gpio3a1.dtbo`、dtbo 在 `/boot/overlays/`，且重启过 |
| probe 报 `-EPROBE_DEFER` / `-ENOENT` | overlay 未应用或 `gpios` 属性不对；用 `ls /proc/device-tree/gpio3a1` 确认节点存在 |
| 电平不变化 | 确认引脚未被其他外设复用（GPIO3_A1 在 SDK 板级 dts 中没有被占用）；必要时用万用表/示波器量测 |
| U-Boot 没应用 overlay | 该 U-Boot 需 `CONFIG_OF_LIBFDT_OVERLAY=y`（SDK 的 `rk3568_defconfig`/`CHIP_defconfig` 已打开）；检查启动 log 中的 overlay 相关输出 |
