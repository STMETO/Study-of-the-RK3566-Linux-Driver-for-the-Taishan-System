# 泰山派 RK3566 工作区

自己写的代码都放这里，SDK 本体在 `/build/sdk`（只读为主，改了要在 SDK 自己的 git 仓库里回溯）。

## 目录

| 目录 | 用途 |
|---|---|
| `drivers/` | out-of-tree 内核驱动模块（`.ko`） |
| `apps/` | 用户态应用 / 测试程序 |
| `scripts/` | 编译、烧录、调试脚本 |
| `docs/` | 笔记、寄存器表、调试记录 |

## 常用命令

```bash
# 编译 out-of-tree 驱动模块（需先 ./build.sh kernel 生成 Module.symvers）
make -C /build/sdk/kernel-6.1 M=$PWD/drivers/<模块名> \
     ARCH=arm64 CROSS_COMPILE=/build/sdk/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu- \
     modules
```

## SDK 位置

- 源码：`/build/sdk`
- 板级配置：`/build/sdk/device/rockchip/.chips/rk3566_rk3568/rockchip_rk3566_taishanpi_1m_v10_defconfig`
- 设备树：`/build/sdk/kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-rk3566-*.dts(i)`
- 固件输出：`/build/sdk/rockdev/`
