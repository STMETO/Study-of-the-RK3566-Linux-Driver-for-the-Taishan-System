// SPDX-License-Identifier: GPL-2.0
/*
 * fake_spi_flash.c - 软件模拟的 SPI 控制器 + 一颗 W25Q64
 *
 * 目的：手上还没有真的 SPI flash 芯片，但想在没有硬件的板子上把
 *       "SPI 驱动 -> 字符设备 -> C++ 应用" 整条链路跑通。
 *
 * 做法（和 i2c-stub 的思路一样）：
 *   1. 注册一个假的 struct spi_controller，它没有真实寄存器，
 *      transfer_one_message() 里用一个纯软件状态机充当 SPI 从设备；
 *   2. 这个从设备能应答 W25Q 系列真正会用到的命令：
 *        0x06 WREN           置 WEL
 *        0x05 RDSR1          回状态寄存器
 *        0x9F JEDEC ID       回 0xEF4017（W25Q64, 8MB）
 *        0x03 READ           从内部内存数组读
 *        0x02 PAGE PROGRAM   写进内部内存数组
 *        0x20 SECTOR ERASE   4KB 擦成 0xFF
 *        0xC7 CHIP ERASE     整片擦成 0xFF
 *   3. 控制器注册成功后再用 spi_new_device() 手动挂一颗 modalias = "w25q64"
 *      的设备，于是 w25q64.ko 里的 i2c_device_id/spi_device_id 表就能匹配上，
 *      probe 流程和真芯片一模一样。
 *
 * 注意：这是**独立模块**，和 w25q64.ko 完全分开，不会进产物固件；
 *       真实硬件上不需要它，直接 rmmod 即可。
 *
 * 用法：
 *   insmod fake-spi-flash.ko            # 默认 8MB
 *   insmod fake-spi-flash.ko flash_size=1048576
 *   insmod w25q64.ko
 *   ls /dev/w25q64
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#define FAKE_NAME		"fake-spi-flash"

/* W25Q 命令字（只实现本工程用到的） */
#define W25Q_CMD_WRITE_ENABLE	0x06
#define W25Q_CMD_READ_STATUS1	0x05
#define W25Q_CMD_READ_DATA	0x03
#define W25Q_CMD_PAGE_PROGRAM	0x02
#define W25Q_CMD_SECTOR_ERASE	0x20
#define W25Q_CMD_CHIP_ERASE	0xC7
#define W25Q_CMD_JEDEC_ID	0x9F

#define W25Q_STATUS_BUSY	BIT(0)
#define W25Q_STATUS_WEL		BIT(1)

/* 假闪存的默认参数：8MB / 4KB 扇区 / 256B 页 / JEDEC 0xEF4017 */
#define FAKE_DEFAULT_SIZE	(8u * 1024 * 1024)
#define FAKE_SECTOR_SIZE	4096u
#define FAKE_PAGE_SIZE		256u
#define FAKE_JEDEC_W25Q64	0xEF4017u

/*
 * 从设备在一条 message 内部的解码状态。
 * 真实 SPI flash 是"靠时钟推进状态机"的：CS 拉低后收到的第一个字节是命令，
 * 后面的字节按命令解释；CS 拉高时状态机复位。这里完全照这个模型实现。
 */
enum fake_state {
	FAKE_IDLE,		/* 等命令 */
	FAKE_ADDR,		/* 正在收 24 位地址 */
	FAKE_READ,		/* READ：往 MISO 上吐数据 */
	FAKE_PROGRAM,		/* PAGE PROGRAM：把 MOSI 上的数据写进数组 */
	FAKE_READ_STATUS,	/* RDSR1：下一个字节回状态 */
	FAKE_JEDEC,		/* JEDEC ID：回 3 字节 ID */
};

struct fake_spi {
	struct spi_controller *ctlr;	/* 我们注册的假控制器 */
	u8 *mem;			/* 模拟的 flash 存储区（vmalloc） */
	u32 size;
	u32 sector_size;
	u32 page_size;
	u32 jedec_id;

	/* 以下字段在每条 message 开始时复位 */
	u8 status;			/* 状态寄存器 1 */
	enum fake_state state;
	u8 cmd;
	u32 addr;
	int addr_cnt;
	u32 pos;
};

static unsigned int flash_size = FAKE_DEFAULT_SIZE;
module_param(flash_size, uint, 0444);
MODULE_PARM_DESC(flash_size, "模拟 flash 容量（字节，默认 8MB）");

/* ------------------------------------------------------------------------- */
/* 从设备行为                                                                */
/* ------------------------------------------------------------------------- */

static void fake_cs_assert(struct fake_spi *fs)
{
	/* CS 拉低：状态机归零，准备接收新命令 */
	fs->state = FAKE_IDLE;
	fs->cmd = 0;
	fs->addr = 0;
	fs->addr_cnt = 0;
	fs->pos = 0;
}

static void fake_cs_release(struct fake_spi *fs)
{
	/*
	 * CS 拉高：编程/擦除在真实芯片上就是在这时"提交"的，完成后 WEL 自动清零。
	 * 我们是内存模拟，数据已经写进去了，这里只需要收尾。
	 */
	fs->status &= ~W25Q_STATUS_WEL;
	fs->state = FAKE_IDLE;
}

static void fake_sector_erase(struct fake_spi *fs)
{
	u32 start = fs->addr & ~(fs->sector_size - 1);

	if (start >= fs->size)
		return;
	if (start + fs->sector_size > fs->size)
		return;

	memset(fs->mem + start, 0xFF, fs->sector_size);
}

static void fake_chip_erase(struct fake_spi *fs)
{
	memset(fs->mem, 0xFF, fs->size);
}

/*
 * 核心：收到一个 MOSI 字节，返回一个 MISO 字节。
 * 这就是"SPI 从设备"的全部行为。
 */
static u8 fake_slave_xfer(struct fake_spi *fs, u8 mosi)
{
	u8 miso = 0xFF;

	switch (fs->state) {
	case FAKE_IDLE:
		fs->cmd = mosi;
		switch (mosi) {
		case W25Q_CMD_WRITE_ENABLE:
			fs->status |= W25Q_STATUS_WEL;
			break;
		case W25Q_CMD_READ_STATUS1:
			fs->state = FAKE_READ_STATUS;
			break;
		case W25Q_CMD_JEDEC_ID:
			fs->state = FAKE_JEDEC;
			fs->pos = 0;
			break;
		case W25Q_CMD_READ_DATA:
		case W25Q_CMD_PAGE_PROGRAM:
		case W25Q_CMD_SECTOR_ERASE:
			fs->state = FAKE_ADDR;
			fs->addr = 0;
			fs->addr_cnt = 0;
			break;
		case W25Q_CMD_CHIP_ERASE:
			fake_chip_erase(fs);
			break;
		default:
			/* 不认识的命令：什么都不做，等 CS 拉高 */
			break;
		}
		break;

	case FAKE_READ_STATUS:
		miso = fs->status;
		fs->state = FAKE_IDLE;
		break;

	case FAKE_JEDEC:
		switch (fs->pos++) {
		case 0:
			miso = (fs->jedec_id >> 16) & 0xFF;
			break;
		case 1:
			miso = (fs->jedec_id >> 8) & 0xFF;
			break;
		case 2:
			miso = fs->jedec_id & 0xFF;
			break;
		default:
			miso = 0x00;	/* 多读的字节补 0 */
			break;
		}
		break;

	case FAKE_ADDR:
		fs->addr = (fs->addr << 8) | mosi;
		if (++fs->addr_cnt < 3)
			break;

		/* 24 位地址收完，按命令切换状态 */
		switch (fs->cmd) {
		case W25Q_CMD_READ_DATA:
			fs->state = FAKE_READ;
			fs->pos = 0;
			break;
		case W25Q_CMD_PAGE_PROGRAM:
			fs->state = FAKE_PROGRAM;
			break;
		case W25Q_CMD_SECTOR_ERASE:
			fake_sector_erase(fs);
			fs->state = FAKE_IDLE;
			break;
		}
		break;

	case FAKE_READ:
		if (fs->addr + fs->pos >= fs->size)
			miso = 0xFF;	/* 越界按 NOR 的"未写"读回 0xFF */
		else
			miso = fs->mem[fs->addr + fs->pos];
		fs->pos++;
		break;

	case FAKE_PROGRAM:
		/* 没有 WREN 就不允许编程——这一点和真芯片一致，能帮我们发现漏发 WREN */
		if (!(fs->status & W25Q_STATUS_WEL))
			break;
		if (fs->addr < fs->size)
			fs->mem[fs->addr] = mosi;
		/* 页内回绕：真实芯片在一页内地址低 8 位自动回卷 */
		fs->addr = (fs->addr & ~(fs->page_size - 1)) |
			   ((fs->addr + 1) & (fs->page_size - 1));
		break;
	}

	return miso;
}

/* ------------------------------------------------------------------------- */
/* 控制器回调                                                                */
/* ------------------------------------------------------------------------- */

/*
 * 一条 spi_message 就是一次完整的片选周期：
 *   CS 拉低 -> 依次走完所有 transfer -> CS 拉高。
 * 我们在每个 transfer 里逐字节喂给从设备状态机，并把 MISO 字节填回 rx_buf。
 */
static int fake_transfer_one_message(struct spi_controller *ctlr,
				     struct spi_message *msg)
{
	struct fake_spi *fs = spi_controller_get_devdata(ctlr);
	struct spi_transfer *xfer;

	fake_cs_assert(fs);
	msg->status = 0;
	msg->actual_length = 0;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		const u8 *tx = xfer->tx_buf;
		u8 *rx = xfer->rx_buf;
		unsigned int i;

		for (i = 0; i < xfer->len; i++) {
			/* tx_buf 为空说明是纯读阶段，MOSI 保持高电平（0xFF） */
			u8 mosi = tx ? tx[i] : 0xFF;
			u8 miso = fake_slave_xfer(fs, mosi);

			if (rx)
				rx[i] = miso;
		}

		msg->actual_length += xfer->len;
	}

	fake_cs_release(fs);

	spi_finalize_current_message(ctlr);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* 控制器 / 平台设备                                                         */
/* ------------------------------------------------------------------------- */

static int fake_probe(struct platform_device *pdev)
{
	struct spi_controller *ctlr;
	struct fake_spi *fs;
	struct spi_device *spi;
	struct spi_board_info bi = {
		.modalias = "w25q64",
		.max_speed_hz = 10000000,
		.chip_select = 0,
		.mode = SPI_MODE_0,
	};
	int ret;

	if (flash_size == 0 || flash_size > 64u * 1024 * 1024)
		return -EINVAL;

	ctlr = spi_alloc_master(&pdev->dev, sizeof(*fs));
	if (!ctlr)
		return -ENOMEM;

	fs = spi_controller_get_devdata(ctlr);
	fs->ctlr = ctlr;

	fs->mem = vmalloc(flash_size);
	if (!fs->mem) {
		ret = -ENOMEM;
		goto err_free_ctlr;
	}
	memset(fs->mem, 0xFF, flash_size);	/* 新芯片整片是 0xFF */

	fs->size = flash_size;
	fs->sector_size = FAKE_SECTOR_SIZE;
	fs->page_size = FAKE_PAGE_SIZE;
	fs->jedec_id = FAKE_JEDEC_W25Q64;
	fs->status = 0;

	ctlr->dev.of_node = pdev->dev.of_node;	/* 没有 DT 节点时为 NULL，不影响 */
	ctlr->bus_num = -1;			/* 让内核分配总线号 */
	ctlr->num_chipselect = 1;
	ctlr->mode_bits = SPI_MODE_0 | SPI_MODE_3;

	/*
	 * SPI 核心给从设备分配 buffer 前会检查控制器能不能收发 8 位字，
	 * 不告诉它的话，从设备驱动的 spi_setup() 会失败。
	 */
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->transfer_one_message = fake_transfer_one_message;

	ret = spi_register_controller(ctlr);
	if (ret) {
		dev_err(&pdev->dev, "注册假 SPI 控制器失败: %d\n", ret);
		goto err_free_mem;
	}

	/*
	 * 控制器注册完成后，手动挂一颗 "w25q64" 设备。
	 * 这一步等价于设备树里写一个 spi 子节点，只是我们没法改板级 DT，
	 * 所以用 spi_new_device() 在运行时创建。
	 */
	bi.bus_num = ctlr->bus_num;
	spi = spi_new_device(ctlr, &bi);
	if (!spi) {
		dev_err(&pdev->dev, "创建 w25q64 设备失败\n");
		ret = -ENODEV;
		goto err_unregister;
	}

	platform_set_drvdata(pdev, fs);
	dev_info(&pdev->dev,
		 "假 SPI 控制器就绪: bus %d, %u 字节 flash, JEDEC 0x%06X\n",
		 ctlr->bus_num, fs->size, fs->jedec_id);
	return 0;

err_unregister:
	spi_unregister_controller(ctlr);
err_free_mem:
	vfree(fs->mem);
err_free_ctlr:
	spi_controller_put(ctlr);
	return ret;
}

static int fake_remove(struct platform_device *pdev)
{
	struct fake_spi *fs = platform_get_drvdata(pdev);
	struct spi_controller *ctlr = fs->ctlr;

	/* 注销控制器时 SPI 核心会顺带注销我们手动创建的 w25q64 设备 */
	spi_unregister_controller(ctlr);
	vfree(fs->mem);
	return 0;
}

static const struct of_device_id fake_of_match[] = {
	{ .compatible = "work,fake-spi-flash" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fake_of_match);

static struct platform_driver fake_driver = {
	.probe = fake_probe,
	.remove = fake_remove,
	.driver = {
		.name = FAKE_NAME,
		.of_match_table = fake_of_match,
	},
};

static struct platform_device *fake_pdev;

static int __init fake_init(void)
{
	int ret;

	/* 板级 DT 里没有这个节点，所以手动创建一个平台设备 */
	fake_pdev = platform_device_register_simple(FAKE_NAME, PLATFORM_DEVID_NONE,
						    NULL, 0);
	if (IS_ERR(fake_pdev))
		return PTR_ERR(fake_pdev);

	ret = platform_driver_register(&fake_driver);
	if (ret) {
		platform_device_unregister(fake_pdev);
		return ret;
	}

	return 0;
}

static void __exit fake_exit(void)
{
	platform_driver_unregister(&fake_driver);
	platform_device_unregister(fake_pdev);
}

module_init(fake_init);
module_exit(fake_exit);

MODULE_AUTHOR("fxy");
MODULE_DESCRIPTION("软件模拟的 SPI 控制器 + W25Q64（无硬件测试用，独立于 w25q64.ko）");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" FAKE_NAME);
