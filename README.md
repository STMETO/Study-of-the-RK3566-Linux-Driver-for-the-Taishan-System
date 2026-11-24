# GPIO3_A1 驱动工程（泰山派 RK3566）

本目录是一个**独立 git 仓库**，包含 GPIO3_A1 的内核字符驱动、设备树 overlay、命令行工具和构建/部署脚本。

约定：`/build/work/<项目名>/` 放各个工程，**每个项目独立建 git 仓库**；`/build/work` 本身不再是仓库。
SDK 本体在 `/build/sdk`（只读为主，本工程不修改 SDK 源码）。

## 目录

| 目录/文件 | 用途 |
|---|---|
| `Makefile` | 统一构建入口，最终产物全部写入 `out/` |
| `drivers/gpio3a1/` | out-of-tree 内核字符驱动，生成 `/dev/gpio3a1` |
| `dts/` | overlay（引用 SDK 的 `&gpio3`/`&pinctrl`）与备用整颗板级 dts |
| `apps/gpio3a1_cli/` | 命令行工具 `get` / `set` / `toggle` |
| `scripts/` | fdtoverlay 验证、板端部署脚本 |
| `docs/` | 设计、部署、排查说明 |
| `out/` | 构建产物（`make` 生成，git 忽略） |
| `build/` | 构建中间目录（`make` 自动创建并删除，git 忽略） |

## 快速开始

```bash
make                 # 编译：out/gpio3a1.ko + out/gpio3a1.dtbo + out/gpio3a1_cli
make dtb             # 可选：out/tspi-rk3566-gpio3a1.dtb（整颗板级 DTB）
make verify          # fdtoverlay 验证 overlay（无需板子）
make clean           # 删除 out/ 与 build/

BOARD=root@<板子IP> scripts/install_to_board.sh   # 或按 docs 用 adb push
```

所有最终产物只放在 `out/`；Kbuild/预处理等中间文件放在 `build/`，每个目标结束时
自动删除，不会在源码目录留下 `.o`、`.cmd`、`.mod.c`、`.dtbo` 等中间文件：

```
out/
├── gpio3a1.ko
├── gpio3a1.dtbo
├── gpio3a1_cli
└── tspi-rk3566-gpio3a1.dtb   # 仅在 make dtb 时生成
```

完整说明（包括 pinctrl 嵌套、ubootEnv overlay、adb 部署）见 [docs/gpio3a1.md](docs/gpio3a1.md)。

## 构建依赖

- 内核先构建一次，生成 `.config`、`Module.symvers` 和基准 dtb（只产生编译产物，不改源码）：

  ```bash
  cd /build/sdk
  ./build.sh lunch:rockchip_rk3566_taishanpi_1m_v10_defconfig
  ./build.sh kernel
  ```

- 交叉工具链：`/build/sdk/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/`

- 可用 `make SDK=/path/to/sdk`、`make OUT=/path/to/out` 覆盖默认路径，`make help` 查看全部目标。

## SDK 位置

- 源码：`/build/sdk`
- 板级配置：`/build/sdk/device/rockchip/.chips/rk3566_rk3568/rockchip_rk3566_taishanpi_1m_v10_defconfig`
- 设备树：`/build/sdk/kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-rk3566-*.dts(i)`
- 固件输出：`/build/sdk/rockdev/`
