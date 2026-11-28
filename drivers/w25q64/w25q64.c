// SPDX-License-Identifier: GPL-2.0-only
/*
 * w25q64.c - 泰山派 RK3566 SPI NOR Flash（W25Q 系列）字符设备驱动
 *
 * 硬件连接（spi3m1，板级 dts 已把 spi3 配好）：
 *   CLK  = GPIO4_C2
 *   MOSI = GPIO4_C3
 *   MISO = GPIO4_C5
 *   CS0  = GPIO4_C6
 *
 * 设备树：
 *   &spi3 {
 *           flash@0 {
 *                   compatible = "work,w25q64";
 *                   reg = <0>;
 *                   spi-max-frequency = <10000000>;
 *           };
 *   };
 *
 * 本驱动的骨架和 at24c02 工程完全一致（裸 cdev + 一套 fops，
 * probe 里 alloc_chrdev_region -> cdev_init -> cdev_add -> device_create，
 * remove 里逆序拆），差别只在"硬件层"：
 *
 *   at24c02 : i2c_smbus_read_i2c_block_data() / i2c_smbus_write_byte_data()
 *   w25q64  : 自己拼 SPI 命令，用 spi_sync() 发一条 spi_message
 *
 * ---------------------------------------------------------------------------
 * SPI 事务要点（读代码前先看这段）
 * ---------------------------------------------------------------------------
 * 1) 一条 spi_message 可以挂多个 spi_transfer，CS 在整条 message 期间
 *    保持低电平，transfer 之间不会抬起。读 flash 时"命令 + 3 字节地址"
 *    和"之后的数据"必须放在同一条 message 里，否则芯片把 CS 上升沿当成
 *    事务结束，数据就读不回来了。
 *
 * 2) spi_write_then_read() 做的其实就是"tx 一个 transfer + rx 一个 transfer
 *    放进同一条 message"，所以小数据量读寄存器可以直接用它。本驱动为了
 *    把过程写清楚，统一手写 spi_message。
 *
 * 3) 全双工：SPI 每时钟一个 bit 进、一个 bit 出。发命令的同时 MISO 上的
 *    内容是无意义的（通常读成 0x00 或 0xFF），所以接收缓冲区要从
 *    "第几个字节之后"开始取数据，这一点在 w25q64_read_jedec() 里最明显。
 *
 * ---------------------------------------------------------------------------
 * NOR Flash 特性（写数据和擦除为什么这么麻烦）
 * ---------------------------------------------------------------------------
 * - 编程只能把 bit 从 1 写成 0；想把 0 变回 1 必须先擦除（擦完是 0xFF）。
 * - 擦除最小单位是扇区（4KB），编程最小单位是页（256B），跨页写会回绕。
 * - 每次编程/擦除前都要发 WREN（写使能），结束后要轮询状态寄存器 BUSY 位，
 *   等芯片内部操作真正完成，否则下一条命令会被忽略。
 */

// 重点：路线 A（标准产品路线，推荐）：扔掉你现在这个裸 cdev 驱动，走内核标准 MTD+UBI 栈
// 改造为异步spi

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/string.h>

#include "w25q64_uapi.h"

#define DEVICE_NAME			"w25q64"

/* 没有芯片（或读 ID 失败）时使用的默认容量：8MB */
#define W25Q64_DEFAULT_SIZE		(8u * 1024u * 1024u)

/* W25Q 系列的固定几何参数 */
/*
	1 Sector = 4096 Byte
	1 Page = 256 Byte
	1 Sector = 16 Pages
*/
#define W25Q64_PAGE_SIZE		256u	// 一次页编程命令最多连续写入 256 字节
#define W25Q64_SECTOR_SIZE		4096u	// 擦除操作最小粒度 4KB

/* 各种操作的超时时间（数据手册的最大值再留一点余量） */
#define W25Q64_PROGRAM_TIMEOUT_MS	1000u		/* 页编程典型 0.7ms，最大 3ms */
#define W25Q64_ERASE_TIMEOUT_MS		5000u		/* 4KB 扇区擦除最大 400ms */
#define W25Q64_CHIP_ERASE_TIMEOUT_MS	180000u		/* 整片擦除几十秒，给足余量 */

/* 指令集（只列本驱动用到的） */
#define W25Q_CMD_WRITE_ENABLE	0x06	/* 写使能 */
#define W25Q_CMD_READ_STATUS1	0x05	/* 读状态寄存器 1 */
#define W25Q_CMD_READ_DATA	0x03	/* 读数据（3 字节地址） */
#define W25Q_CMD_PAGE_PROGRAM	0x02	/* 页编程（3 字节地址 + 数据） */
#define W25Q_CMD_SECTOR_ERASE	0x20	/* 4KB 扇区擦除 */
#define W25Q_CMD_CHIP_ERASE	0xC7	/* 整片擦除 */
#define W25Q_CMD_JEDEC_ID	0x9F	/* 读 JEDEC ID（厂商 + 容量） */

/* 状态寄存器 1 的位定义 */
#define W25Q_STATUS_BUSY	BIT(0)	/* 1 = 芯片正在编程/擦除，此时只有 RDSR 有效 */
#define W25Q_STATUS_WEL		BIT(1)	/* 1 = 写使能已置位 */

/*
 * 每个字符设备实例对应一个 w25q64_dev。
 * 和 at24c02 对比：client 换成了 spi，其余（cdev/dev/devt/lock）完全一样。
 */
struct w25q64_dev {
	struct spi_device *spi;	/* 由 SPI 核心创建，代表 CS0 上的这颗芯片 */
	struct cdev cdev;		/* 字符设备本体 */
	struct device *dev;		/* /sys/class/w25q64/w25q64 */
	dev_t devt;				/* 设备号 */
	struct mutex lock;		/* 保护 size/寄存器访问，串行化 SPI 事务 */
	u32 jedec_id;			/* 例如 W25Q64 是 0xEF4017 */
	u32 size;				/* 容量，字节 */
	u32 sector_size;		/* 固定 4096字节 = 4KB */
	u32 page_size;			/* 固定 256字节 */
};

/* class 在整个模块生命周期内只有一份，module_init 里创建 */
static struct class *w25q64_class;

/* ------------------------------------------------------------------------- */
/* 容量识别表                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * JEDEC ID 的低 8 位是容量码，Winbond 的规则很好记：
 * 0x14=1MB、0x15=2MB、0x16=4MB、0x17=8MB、0x18=16MB …… 每加 1 翻一倍。
 * 0x19(32MB) 及以上要改用 4 字节地址模式，本驱动只实现 3 字节地址，
 * 所以表里不列，遇到时回退到设备树/默认容量并打印警告。
 */
struct w25q64_capacity {
	u32 jedec_id;
	u32 size;
	const char *name;
};

// W25Q 系列 Flash 的 ID → 容量、型号的匹配表
static const struct w25q64_capacity w25q64_capacities[] = {
	{ 0xEF4013, 512u * 1024u,	"W25Q40"  },
	{ 0xEF4014, 1024u * 1024u,	"W25Q80"  },
	{ 0xEF4015, 2u * 1024u * 1024u,	"W25Q16"  },
	{ 0xEF4016, 4u * 1024u * 1024u,	"W25Q32"  },
	{ 0xEF4017, 8u * 1024u * 1024u,	"W25Q64"  },
	{ 0xEF4018, 16u * 1024u * 1024u,	"W25Q128" },
};

/* ------------------------------------------------------------------------- */
/* 硬件层：所有直接碰 SPI 的代码都在这一节                                     */
/* ------------------------------------------------------------------------- */

/* 把 24 位地址拆成 3 个字节，SPI 是高位先发（big-endian） */
static void w25q64_encode_addr(u8 *p, u32 addr)
{
	p[0] = (addr >> 16) & 0xFF;
	p[1] = (addr >> 8) & 0xFF;
	p[2] = addr & 0xFF;
}

/*
 * 最基础的"发一条 message"。
 * tx 和 rx 长度相同（全双工），rx 可以为 NULL（只发不收）。
 *
 * 为什么这里要"多此一举"地把参数拷进 kmalloc 缓冲区：
 *   SPI 控制器如果走 DMA，spi_sync() 内部会把 tx_buf / rx_buf 交给
 *   dma_map_single() 做地址映射。DMA 引擎只认物理地址，而调用者为了省事
 *   往往传的是栈上的局部数组（比如 read_status() 里的 u8 tx[2]）——
 *   栈内存在内核里没有稳定的物理页，DMA 映射轻则打印
 *   "DMA-API: device driver maps memory from stack" 警告，重则直接失败。
 *   所以在这一层统一做一次 kmalloc + 拷贝：
 *     - 调用者可以放心用栈数组写命令；
 *     - DMA 约束只在这一处处理，所有命令共用同一条安全路径。
 *   代价是每条命令多两次小内存分配，相对 flash 的毫秒级时序可以忽略。
 */
static int w25q64_sync(struct w25q64_dev *priv, const void *tx, void *rx, size_t len)
{
	u8 *ktx = NULL;
	u8 *krx = NULL;
	struct spi_transfer xfer = { .len = len };
	struct spi_message msg;
	int ret;

	// 如果上层有发送数据，分配堆ktx，把tx数据拷贝进来
	if (tx) {
		ktx = kmalloc(len, GFP_KERNEL);
		if (!ktx)
			return -ENOMEM;
		memcpy(ktx, tx, len);
		xfer.tx_buf = ktx;
	}

	// 如果上层需要接收数据，分配堆krx
	if (rx) {
		krx = kmalloc(len, GFP_KERNEL);
		if (!krx) {
			kfree(ktx);
			return -ENOMEM;
		}
		xfer.rx_buf = krx;
	}

	// SPI框架标准流程：初始化message，挂transfer
	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	// 阻塞同步SPI传输，底层控制器如果支持DMA就会走DMA
	ret = spi_sync(priv->spi, &msg);

	/* 只有成功时 rx 缓冲区的内容才有意义 */
	if (rx && !ret)
		memcpy(rx, krx, len);

	kfree(krx);
	kfree(ktx);
	return ret;
}

/*
 * 发 WREN（写使能）。
 * 芯片在每次编程/擦除前都要求先置 WEL 位，这个位在操作完成后自动清零。
 */
static int w25q64_write_enable(struct w25q64_dev *priv)
{
	u8 cmd = W25Q_CMD_WRITE_ENABLE;

	return w25q64_sync(priv, &cmd, NULL, 1);
}

/*
* 读状态寄存器 1。
* 发送 1 字节命令，第 2 个字节返回状态；所以 tx/rx 都要 2 字节长，
* 有效数据在 rx[1]。
*/
static int w25q64_read_status(struct w25q64_dev *priv, u8 *status)
{
	u8 tx[2] = { W25Q_CMD_READ_STATUS1, 0 };
	u8 rx[2] = { 0 };
	int ret;

	ret = w25q64_sync(priv, tx, rx, sizeof(tx));
	if (ret)
		return ret;

	*status = rx[1];
	return 0;
}

/**
* @brief 轮询等待Flash芯片空闲，直到BUSY位为0或者超时
* @param priv        设备私有结构体
* @param timeout_ms  最大等待超时时间，单位ms
* @return 成功返回0；SPI读状态失败返回负错误码；超时返回-ETIMEDOUT
*
* 说明：
* 页编程耗时几ms，4K扇区擦除几十~几百ms，整片擦除可达几十秒；
* 不能while(1)空转忙等（会占死CPU），每次查询后msleep让出CPU。
*/
static int w25q64_wait_ready(struct w25q64_dev *priv, unsigned int timeout_ms)
{
	/*
	* jiffies：内核全局滴答计数器，系统启动后不断递增
	* msecs_to_jiffies：把毫秒转换成jiffies滴答数
	* deadline = 当前滴答 + 超时滴答，作为超时截止点
	*/
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	int ret;

	// 无限循环，直到就绪 / 出错 / 超时
	for (;;) {
		u8 status;

		// SPI读状态寄存器（命令0x05），拿到SR1状态字节
		ret = w25q64_read_status(priv, &status);
		if (ret)
			return ret;  // 读状态本身SPI失败，直接返回错误

		// BUSY位(BIT0)为0：芯片空闲，操作完成，正常退出
		if (!(status & W25Q_STATUS_BUSY))
			return 0;

		/*
		* time_after(a,b)：内核安全宏，判断 a > b
		* 专门处理jiffies无符号数溢出回绕的坑，不要直接写 jiffies > deadline
		* 如果当前滴答已经超过截止时间：超时报错
		*/
		if (time_after(jiffies, deadline)) {
			dev_err(&priv->spi->dev,
				"等待芯片空闲超时（%u ms）\n", timeout_ms);
			return -ETIMEDOUT;
		}

		/*
		* 休眠1ms，主动让出CPU，调度器切走进程
		* 避免CPU空转占用100%；Flash操作很慢，1ms查询间隔足够
		*/
		msleep(1);
	}
}
 

/**
* @brief 读取W25Q系列Flash的JEDEC ID，用于识别芯片型号
* @param priv  设备私有结构体指针，包含spi_device、锁、容量等信息
* @return 成功返回0；失败返回负错误码
*
* JEDEC ID 读取命令：0x9F，该命令流程：
* 主机发送 0x9F 命令字节，之后连续读出3字节ID：
*  rx[1]：厂商ID（Winbond华邦固定为 0xEF）
*  rx[2]：内存类型ID
*  rx[3]：容量ID
*  三者拼接成24bit的JEDEC ID，用于查表匹配Flash型号
*/
static int w25q64_read_jedec(struct w25q64_dev *priv)
{
	/* tx[0] = 0x9F(JEDEC命令)，后面3个填充字节只是占位，用于同步SPI传输 */
	u8 tx[4] = { W25Q_CMD_JEDEC_ID, 0, 0, 0 };
	/* rx数组接收SPI返回数据，rx[0]对应发送命令阶段的dummy，有效数据从rx[1]开始 */
	u8 rx[4] = { 0 };
	int ret;

	/* 发起一次同步SPI传输：发送tx，同时接收rx，一共4字节 */
	ret = w25q64_sync(priv, tx, rx, sizeof(tx));
	if (ret)
		return ret;  // SPI传输出错，直接向上返回错误

	/* 合法性判断：厂商ID为0x00或者0xFF，代表没有芯片/总线浮空/芯片未应答 */
	if (rx[1] == 0x00 || rx[1] == 0xFF)
		return -ENODEV;

	/* 拼接24bit JEDEC ID：
	* rx[1] 厂商ID 放在高8bit(bit23~bit16)
	* rx[2] 内存类型 放在中间8bit(bit15~bit8)
	* rx[3] 容量ID 放在低8bit(bit7~bit0)
	* 转u32避免移位溢出，存入私有结构体，给probe查表使用

	* 厂商 ID：0xEF → Winbond 华邦
	* 内存类型（memory type，就是你代码里的 rx [2]）：用来区分**Flash 子系列、总线类型、架构大类
		你表里这批 W25Qxx，全部是 `0x40`，代表：**W25Q Q 系列标准 SPI NOR Flash**（支持单 / 双 / 四线 SPI）
	*/
	priv->jedec_id = ((u32)rx[1] << 16) | ((u32)rx[2] << 8) | rx[3];
	return 0;
}

/*
* 读任意长度数据。
* 这是一条有两个 transfer 的 message：
*   transfer[0]：tx = 0x03 + 3 字节地址（4 字节）
*   transfer[1]：rx = 用户要的数据（len 字节）
* CS 在两个 transfer 之间保持低电平，这正是读 flash 的时序要求。
*/
static int w25q64_read_data(struct w25q64_dev *priv, u32 addr, void *buf, size_t len)
{
	struct spi_transfer xfer[2] = { };
	struct spi_message msg;
	u8 *hdr;
	int ret;

	/*
	 * 命令头必须放在 kmalloc 出来的内存里：它要交给 DMA 映射，
	 * 栈上的数组不行（原因见 w25q64_sync() 的注释）。
	 * buf 由调用者提供，read() 里是用 kmalloc 分配的，满足要求。
	 */
	hdr = kmalloc(4, GFP_KERNEL);
	if (!hdr)
		return -ENOMEM;

	hdr[0] = W25Q_CMD_READ_DATA;
	w25q64_encode_addr(&hdr[1], addr);

	xfer[0].tx_buf = hdr;
	xfer[0].len = 4;
	xfer[1].rx_buf = buf;
	xfer[1].len = len;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer[0], &msg);
	spi_message_add_tail(&xfer[1], &msg);

	ret = spi_sync(priv->spi, &msg);
	kfree(hdr);
	return ret;
}

/*
* w25q64_page_program - W25Q 页编程（PAGE PROGRAM 0x02）
* @priv:      设备私有结构体指针
* @addr:      Flash 起始编程地址（24bit有效地址）
* @buf:       内核态待写入数据缓冲区
* @len:       本次写入字节长度
* 返回值：成功返回0；失败返回负的内核错误码
*
* 硬件约束（重点）：
* 1. 单次页编程命令最多写 256 字节（一页），不能跨页；
* 2. 执行前必须先发 WREN(0x06) 写使能，否则页编程命令会被芯片忽略；
* 3. 发送完页编程SPI报文之后，芯片内部开始烧写，需要轮询 BUSY 位等待完成；
* 4. 硬件特性：只能把bit从1变成0；想要写入新数据，必须提前擦除扇区（擦除后bit全1）；
* 5. 整条报文（命令+3字节地址+数据）放在同一个spi_message下发，CS全程保持低。
*/
 static int w25q64_page_program(struct w25q64_dev *priv, u32 addr,
	const void *buf, size_t len)
{
	u8 *tx;
	int ret;

	/* 合法性校验：长度不能为0，也不能超过一页最大256字节 */
	if (!len || len > priv->page_size)
		return -EINVAL;

	/*
	* 分配发送缓冲区：
	* 总长度 = 1字节命令 + 3字节地址 + len字节数据 = 4 + len
	* kmalloc 分配内核堆内存，保证DMA可以安全映射；
	* 栈内存不能直接给DMA SPI控制器使用，会告警/报错。
	*/
	tx = kmalloc(4 + len, GFP_KERNEL);
	if (!tx)
		return -ENOMEM;

	tx[0] = W25Q_CMD_PAGE_PROGRAM;       // 0x02 页编程命令码
	w25q64_encode_addr(&tx[1], addr);     // 把24bit地址编码成大端，填入tx[1],tx[2],tx[3]
	memcpy(&tx[4], buf, len);             // 把待写数据拷贝到命令包后面

	ret = w25q64_write_enable(priv);      // 第一步：发送WREN(0x06)，打开写使能锁（WEL位置1）
	if (!ret)
	{
		// 第二步：发送整包页编程报文，只发不收，rx传NULL
		ret = w25q64_sync(priv, tx, NULL, 4 + len);
	}

	// SPI发送已经完成，tx缓冲区可以释放
	kfree(tx);

	// 如果前面WREN或者SPI发送出错，直接返回错误
	if (ret)
		return ret;

	// 第三步：等待芯片内部编程完成，轮询状态寄存器BUSY位，带超时
	return w25q64_wait_ready(priv, W25Q64_PROGRAM_TIMEOUT_MS);
}


/**
* @brief 扇区擦除，执行 4KB 扇区擦除命令 0x20
* @param priv  设备私有结构体
* @param addr  待擦除的Flash地址，调用方（ioctl分支）已经提前对齐到4K扇区边界、做越界检查
* @return 成功返回0；SPI传输失败 / 等待超时 返回负错误码
*/
static int w25q64_sector_erase(struct w25q64_dev *priv, u32 addr)
{
	u8 tx[4];   // SPI发送缓冲区：1字节命令 + 3字节24bit地址，一共4字节
	int ret;

	// tx[0] 填充扇区擦除命令码：0x20
	tx[0] = W25Q_CMD_SECTOR_ERASE;
	// 将32位地址编码成Flash要求的24bit大端地址，写入tx[1],tx[2],tx[3]
	// w25q64_encode_addr：把addr的bit23~bit0，按高字节先发的顺序填入tx+1起始位置
	// W25Q SPI 协议规定：地址必须先发高字节，后发低字节（协议层面的大端）
	// CPU 是小端架构，所以把一个 u32 存到内存里的时候，是小端布局
	w25q64_encode_addr(&tx[1], addr);

	// 第一步：发 0x06 写使能命令。Flash擦除/编程前必须打开WEL锁存
	ret = w25q64_write_enable(priv);
	// 如果写使能成功，继续发送扇区擦除命令帧
	if (!ret)
		ret = w25q64_sync(priv, tx, NULL, sizeof(tx));

	// 写使能失败 或者 擦除命令SPI传输失败，直接返回错误
	if (ret)
		return ret;

	// 发送擦除命令之后，硬件开始擦除动作（慢操作，几十ms）
	// 轮询状态寄存器 BUSY位，直到BUSY=0，或者超时
	return w25q64_wait_ready(priv, W25Q64_ERASE_TIMEOUT_MS);
}
 

/* 整片擦除：8MB 芯片大约 20~40 秒，调用期间进程会一直阻塞 */
static int w25q64_chip_erase(struct w25q64_dev *priv)
{
	u8 cmd = W25Q_CMD_CHIP_ERASE;
	int ret;

	ret = w25q64_write_enable(priv);
	if (!ret)
		ret = w25q64_sync(priv, &cmd, NULL, 1);

	if (ret)
		return ret;

	return w25q64_wait_ready(priv, W25Q64_CHIP_ERASE_TIMEOUT_MS);
}

/* ------------------------------------------------------------------------- */
/* 字符设备接口：打开 / 关闭                                                    */
/* ------------------------------------------------------------------------- */

static int w25q64_open(struct inode *inode, struct file *file)
{
	/*
	 * inode->i_cdev 指向 cdev_add 时注册的 cdev，
	 * 而 cdev 是 w25q64_dev 的第一个成员，container_of 一步就能反推回去。
	 * 这里把 priv 存进 file->private_data，后续 read/write 直接取用。
	 */
	struct w25q64_dev *priv = container_of(inode->i_cdev, struct w25q64_dev, cdev);

	file->private_data = priv;
	return 0;
}

static int w25q64_release(struct inode *inode, struct file *file)
{
	(void)inode;
	file->private_data = NULL;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* 字符设备接口：read / write                                                  */
/* ------------------------------------------------------------------------- */

/*
 * read(fd, buf, n)：
 *   *ppos 就是 flash 里的字节地址。读到容量末尾返回 0（表示 EOF），
 *   和普通文件语义一致，用户态可以用 while 循环读完整片。
 *
 * file_operations 的 read 回调：内核在用户调用 read() 时进入这个函数
 * @file：打开的文件句柄，里面存 private_data 指向我们的设备私有结构体
 * @ubuf：用户态缓冲区地址（__user 标记，不能直接内核侧访问）
 * @count：用户想要读取的字节数
 * @ppos：文件偏移指针，【入参】当前读取起始地址；【出参】读完后更新偏移
 * 返回值：成功返回读到的字节数；0=EOF；负数=错误码
 *
 *	W25Q64 的读指令（0x03 普通读 / 0x0B 高速读）本身就是任意 24bit 地址直接读
 */
static ssize_t w25q64_read(struct file *file, char __user *ubuf,
	size_t count, loff_t *ppos)
{
	// 取出我们在 open 时挂载进去的私有设备结构体
	struct w25q64_dev *priv = file->private_data;
	size_t n;          // 本次实际要读的字节数（做边界截断）
	void *buf;         // 内核临时缓冲区，用来存放从flash读出的数据
	int ret;

	// 校验偏移：flash地址不能为负数
	if (*ppos < 0)
		return -EINVAL;

	// 偏移已经超过flash总容量 → EOF，返回0，和普通文件read语义对齐
	if (*ppos >= priv->size)
		return 0;

	/*
	* 计算本次真正读取长度：
	* 取【用户想要读count字节】和【从当前偏移到flash末尾剩余字节】两者更小值
	* 防止越界读到flash外面
	*/
	n = min_t(size_t, count, priv->size - (u32)*ppos);
	if (!n)
		return 0;

	// 在内核堆分配一块临时缓冲区，SPI底层DMA传输需要内核合法地址，不能直接用用户态ubuf
	buf = kmalloc(n, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	// 互斥锁：防止多个进程/线程同时发起SPI读写，避免SPI事务错乱
	mutex_lock(&priv->lock);
	// 底层SPI读：从flash地址 *ppos 读出 n 字节到内核buf
	ret = w25q64_read_data(priv, (u32)*ppos, buf, n);
	mutex_unlock(&priv->lock);

	// 底层SPI读失败，释放内存，返回错误
	if (ret) {
		kfree(buf);
		return ret;
	}

	// 把内核缓冲区的数据拷贝到用户态地址空间；copy_to_user失败返回-EFAULT
	if (copy_to_user(ubuf, buf, n)) {
		kfree(buf);
		return -EFAULT;
	}

	// 释放内核临时缓冲区
	kfree(buf);
	// 更新文件偏移指针，下一次read会从这个新位置继续读
	*ppos += n;
	// 返回成功读到的字节数，给用户态read拿到
	return n;
}


/*
* write(fd, buf, n)：file_operations 对应的 write 回调
* @file：打开的设备文件句柄，private_data 指向我们的w25q64_dev私有结构体
* @ubuf：用户态传入的数据缓冲区（__user标记，内核不能直接访问）
* @count：用户希望写入的字节总数
* @ppos：文件偏移指针，入参是本次写入起始Flash地址；出参用于更新偏移
* 返回值：成功返回本次写入字节数；负数代表错误码
*
* 核心设计约定（非常关键）：
*   *ppos 对应Flash内部字节地址；写入会按照W25Q的256B页边界自动切分，逐段页编程
*   驱动【不自动擦除扇区】！！
*   NOR Flash硬件规则：bit只能 1 -> 0；如果对应扇区没有预先擦除（擦除后是0xFF），
*   直接写得到的结果 = 旧值 & 新值，不是预期的数据覆盖。
*   擦除动作交给用户态ioctl(W25Q64_IOC_ERASE_SECTOR / ERASE_CHIP)手动触发
*   驱动只封装原始硬件页编程命令，行为和芯片datasheet完全一致，不做RMW封装
*/
// 这个驱动的设计约定：擦除交给用户态手动控制，驱动底层 `write` / `page_program` 本身不会自动做擦除！
static ssize_t w25q64_write(struct file *file, const char __user *ubuf,
	size_t count, loff_t *ppos)
{
	struct w25q64_dev *priv = file->private_data;
	size_t n, off;
	u8 *buf;
	int ret;

	// 偏移不能是负数，非法地址
	if (*ppos < 0)
		return -EINVAL;

	// 写入起始地址已经超过Flash总容量，没有空间
	if (*ppos >= priv->size)
		return -ENOSPC;

	/*
	* 计算本次实际可写入字节数：
	* 取【用户想要写count字节】和【从当前偏移到Flash末尾剩余字节】的较小值
	* 防止越界写入Flash芯片外部
	*/
	n = min_t(size_t, count, priv->size - (u32)*ppos);
	if (!n)
		return 0;

	/*
	* memdup_user：内核封装函数，自动分配内核缓冲区 + 把用户ubuf拷贝到内核空间
	* 等价于 kmalloc + copy_from_user，代码更简洁
	* 原因：SPI底层DMA传输不能直接使用用户态虚拟地址，必须在内核缓冲区操作
	*/
	buf = memdup_user(ubuf, n);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	ret = 0;
	// 互斥锁：保护SPI总线，防止多进程并发读写打乱SPI时序
	mutex_lock(&priv->lock);

	/*
	* off：已经处理完的字节偏移，从0开始，循环直到写完n字节
	* W25Q页编程限制：一次页编程命令不能跨256B页边界，跨页会发生数据回绕（硬件特性）
	* 所以必须循环切分，保证每一次w25q64_page_program都不跨页
	*/
	for (off = 0; off < n; off += ret) {
		// 当前这一段的Flash物理地址
		u32 addr = (u32)*ppos + off;
		// 当前地址在本页内的偏移：addr % 256
		size_t page_off = addr % priv->page_size;
		// chunk：这一轮最多能写多少字节，保证不会冲出当前页
		size_t chunk = min_t(size_t, n - off, priv->page_size - page_off);

		/*
		* 调用底层页编程：只写chunk个字节，地址addr，数据来自buf+off
		* page_program内部逻辑：发写使能WEL、发送页编程SPI命令+地址+数据、等待BUSY位清零
		*/
		ret = w25q64_page_program(priv, addr, buf + off, chunk);
		if (ret)
			break; // 任意一段出错，立刻跳出循环，不再继续写
	}

	mutex_unlock(&priv->lock);
	// 释放memdup_user分配的内核缓冲区
	kfree(buf);

	// 循环里任意一次页编程失败，直接返回错误码
	if (ret)
		return ret;

	// 全部写完，更新文件偏移指针，下一次write从这个新地址继续
	*ppos += n;
	// 返回成功写入总字节数，给用户态write拿到
	return n;
}

/*
 * llseek：把文件位置钳到 [0, size]，越界时停在边界而不是报错，
 * 这样 `dd skip=... count=...` 之类的工具用起来更顺手。
 */
static loff_t w25q64_llseek(struct file *file, loff_t offset, int whence)
{
	struct w25q64_dev *priv = file->private_data;
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = offset;
		break;
	case SEEK_CUR:
		newpos = file->f_pos + offset;
		break;
	case SEEK_END:
		newpos = (loff_t)priv->size + offset;
		break;
	default:
		return -EINVAL;
	}

	if (newpos < 0)
		newpos = 0;

	if (newpos > priv->size)
		newpos = priv->size;

	file->f_pos = newpos;
	return newpos;
}

/* ------------------------------------------------------------------------- */
/* 字符设备接口：ioctl                                                         */
/* ------------------------------------------------------------------------- */

static long w25q64_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct w25q64_dev *priv = file->private_data;
	int ret = 0;

	/* 先校验魔数，避免把别人驱动的 ioctl 误当成自己的 */
	if (_IOC_TYPE(cmd) != W25Q64_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case W25Q64_IOC_GET_ID: {
		u32 id = priv->jedec_id;

		if (put_user(id, (u32 __user *)arg))
			ret = -EFAULT;
		break;
	}

	case W25Q64_IOC_GET_SIZE: {
		u32 size = priv->size;

		if (put_user(size, (u32 __user *)arg))
			ret = -EFAULT;
		break;
	}

	case W25Q64_IOC_GET_SECTOR_SIZE: {
		u32 size = priv->sector_size;

		if (put_user(size, (u32 __user *)arg))
			ret = -EFAULT;
		break;
	}

	case W25Q64_IOC_GET_PAGE_SIZE: {
		u32 size = priv->page_size;

		if (put_user(size, (u32 __user *)arg))
			ret = -EFAULT;
		break;
	}

	case W25Q64_IOC_GET_STATUS: {
		u8 status;

		/* 这里现读一次芯片，用户看到的是"此刻"的真实状态 */
		// W25Q64 状态寄存器 SR1（读命令 0x05，就是你 w25q64_read_status 读的这个 u8）
		// bit7  bit6  bit5  bit4  bit3  bit2  bit1  bit0
		// SRP0  SEC   TB   BP2   BP1   BP0  WEL  BUSY
		mutex_lock(&priv->lock);
		ret = w25q64_read_status(priv, &status);
		mutex_unlock(&priv->lock);

		if (!ret && put_user(status, (u8 __user *)arg))
			ret = -EFAULT;
		break;
	}

	case W25Q64_IOC_GET_INFO: {
		struct w25q64_info info = {
			.jedec_id = priv->jedec_id,
			.size = priv->size,
			.sector_size = priv->sector_size,
			.page_size = priv->page_size,
			.speed_hz = priv->spi->max_speed_hz,
			.status = 0,
		};
		u8 status;

		mutex_lock(&priv->lock);
		ret = w25q64_read_status(priv, &status);
		mutex_unlock(&priv->lock);

		if (!ret) {
			info.status = status;
			if (copy_to_user((void __user *)arg, &info, sizeof(info)))
				ret = -EFAULT;
		}
		break;
	}

	case W25Q64_IOC_ERASE_SECTOR: {
		u32 addr;

		// 从用户空间取出传入的地址参数
		if (get_user(addr, (u32 __user *)arg))
			return -EFAULT;

		/* 对齐到扇区边界：擦除是"整扇区"操作，不可能只擦一半 */
		// 扇区大小是 4096，sector_size - 1 = 0xFFF；~0xFFF就是掩码，把低 12bit 清 0
		// 例：用户传 0x1005 → 自动修正为 0x1000
		// 原因：W25Q 扇区擦除命令只能擦整个 4K 扇区，不支持擦除扇区内一部分
		addr &= ~(priv->sector_size - 1);
		if (addr >= priv->size)	// 合法性校验：地址不能超过Flash总容量
			return -EINVAL;

		mutex_lock(&priv->lock);
		ret = w25q64_sector_erase(priv, addr);	// 调用底层函数，执行扇区擦除
		mutex_unlock(&priv->lock);
		break;
	}

	case W25Q64_IOC_ERASE_CHIP:
		mutex_lock(&priv->lock);
		ret = w25q64_chip_erase(priv);
		mutex_unlock(&priv->lock);
		break;

	default:
		/* 不是本驱动认识的命令，交给上层返回 ENOTTY */
		return -ENOTTY;
	}

	return ret;
}

/* 把上面的回调打包成 fops，.owner 必须写上，否则模块卸载会出问题 */
static const struct file_operations w25q64_fops = {
	.owner		= THIS_MODULE,
	.open		= w25q64_open,
	.release	= w25q64_release,
	.read		= w25q64_read,
	.write		= w25q64_write,
	.llseek		= w25q64_llseek,
	.unlocked_ioctl	= w25q64_ioctl,
};

/* ------------------------------------------------------------------------- */
/* SPI 驱动：probe / remove                                                    */
/* ------------------------------------------------------------------------- */

/*
 * probe 的调用时机：
 *   - 设备树里的 flash@0 与 of_match_table 里的 "work,w25q64" 匹配
 *   - 或者 sysfs 里 new_device 时用名字 "w25q64" 匹配 id_table
 * 两种情况都会走到这里，参数是 SPI 核心创建好的 spi_device。
 */
static int w25q64_probe(struct spi_device *spi)
{
	struct w25q64_dev *priv;
	const struct w25q64_capacity *cap = NULL;
	size_t i;
	u32 dt_size = 0;
	int ret;

	/* devm 版本：设备卸载时自动释放，不用手写 kfree */
	priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spi = spi;
	priv->sector_size = W25Q64_SECTOR_SIZE;
	priv->page_size = W25Q64_PAGE_SIZE;
	priv->size = W25Q64_DEFAULT_SIZE;
	mutex_init(&priv->lock);

	/*
	 * 显式配置 SPI 模式：NOR flash 支持 mode 0 和 mode 3，
	 * 这里用最常见的 mode 0（CPOL=0, CPHA=0），8 bit。
	 * 频率取自设备树的 spi-max-frequency，spi_setup() 会做合法性检查。
	 */
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	// spi_setup()会把你设置好的 mode、bits_per_word、spi->max_speed_hz 下发给 SPI 控制器驱动
	// 成功返回后，这个 spi_device 就具备了收发 SPI 报文的能力
	// spi 需要设置总线参数所以需要setuo，i2c除了时钟需要设置其他的就是固定的了所以不需要setup
	ret = spi_setup(spi);
	if (ret) {
		dev_err(&spi->dev, "spi_setup 失败: %d\n", ret);
		goto err_mutex;
	}

	/* 读 JEDEC ID 识别芯片型号和容量 */
	ret = w25q64_read_jedec(priv);
	if (ret) {
		/*
		 * 没有芯片时不阻止 probe：节点照样建出来，
		 * 用户态至少能看到 /dev/w25q64 和默认容量，
		 * 也方便先用假 SPI 控制器把整条链路跑通。
		 */
		dev_warn(&spi->dev, "读 JEDEC ID 失败(%d)，按默认容量 %u 字节处理\n",
			 ret, priv->size);
	} else {
		for (i = 0; i < ARRAY_SIZE(w25q64_capacities); i++) {
			if (w25q64_capacities[i].jedec_id == priv->jedec_id) {
				cap = &w25q64_capacities[i];
				break;
			}
		}

		if (cap) {
			priv->size = cap->size;
			dev_info(&spi->dev, "检测到 %s（JEDEC ID 0x%06X，%u MB）\n",
				 cap->name, priv->jedec_id, cap->size >> 20);
		} else {
			dev_warn(&spi->dev,
				 "未知的 JEDEC ID 0x%06X，按默认容量处理\n",
				 priv->jedec_id);
		}
	}

	/*
	 * 设备树里的 size 属性可以覆盖自动识别的容量，
	 * 主要给"假 SPI 控制器"或者没插芯片时用。
	 */
	if (!of_property_read_u32(spi->dev.of_node, "size", &dt_size) &&
	    dt_size >= priv->sector_size) {
		priv->size = dt_size & ~(priv->sector_size - 1);
		dev_info(&spi->dev, "设备树覆盖容量: %u 字节\n", priv->size);
	}

	/* ---- 以下是和 at24c02 一模一样的字符设备注册流程 ---- */
	ret = alloc_chrdev_region(&priv->devt, 0, 1, DEVICE_NAME);
	if (ret) {
		dev_err(&spi->dev, "alloc_chrdev_region 失败: %d\n", ret);
		goto err_mutex;
	}

	cdev_init(&priv->cdev, &w25q64_fops);
	priv->cdev.owner = THIS_MODULE;

	ret = cdev_add(&priv->cdev, priv->devt, 1);
	if (ret) {
		dev_err(&spi->dev, "cdev_add 失败: %d\n", ret);
		goto err_region;
	}

	priv->dev = device_create(w25q64_class, &spi->dev, priv->devt, priv,
				  DEVICE_NAME);
	if (IS_ERR(priv->dev)) {
		ret = PTR_ERR(priv->dev);
		dev_err(&spi->dev, "device_create 失败: %d\n", ret);
		goto err_cdev;
	}

	spi_set_drvdata(spi, priv);

	dev_info(&spi->dev,
		 "就绪: /dev/%s，主设备号 %u，容量 %u 字节，SPI %u Hz\n",
		 DEVICE_NAME, MAJOR(priv->devt), priv->size, spi->max_speed_hz);

	return 0;

err_cdev:
	cdev_del(&priv->cdev);
err_region:
	unregister_chrdev_region(priv->devt, 1);
err_mutex:
	mutex_destroy(&priv->lock);
	return ret;
}

/*
 * remove 的返回类型是 void（6.1 里 spi_driver.remove 与 i2c_driver.remove
 * 都是 void，而 platform_driver.remove 还是 int，这是跟 led8 工程的区别）。
 * 拆的顺序和 probe 相反：device -> cdev -> 设备号 -> 锁。
 */
static void w25q64_remove(struct spi_device *spi)
{
	struct w25q64_dev *priv = spi_get_drvdata(spi);

	if (!priv)
		return;

	device_destroy(w25q64_class, priv->devt);
	cdev_del(&priv->cdev);
	unregister_chrdev_region(priv->devt, 1);
	mutex_destroy(&priv->lock);

	dev_info(&spi->dev, "已卸载\n");
}

/* ------------------------------------------------------------------------- */
/* 模块注册                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * 两张匹配表缺一不可：
 *   of_match_table：设备树里的 compatible = "work,w25q64" 走这张
 *   id_table      ：sysfs new_device 或者 SPI 板级信息用名字匹配走这张
 */
static const struct of_device_id w25q64_of_match[] = {
	{ .compatible = "work,w25q64" },
	{ }
};
MODULE_DEVICE_TABLE(of, w25q64_of_match);

static const struct spi_device_id w25q64_id[] = {
	{ "w25q64", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, w25q64_id);

static struct spi_driver w25q64_driver = {
	.driver = {
		.name		= "w25q64",
		.of_match_table	= w25q64_of_match,
	},
	.probe		= w25q64_probe,
	.remove		= w25q64_remove,
	.id_table	= w25q64_id,
};

/*
 * 模块初始化顺序也是照着 at24c02 来的：
 *   1. 先建 class（/sys/class/w25q64/）
 *   2. 再注册 SPI 驱动（此时如果设备树里已有 flash@0，probe 立刻被调用）
 * 不用 module_spi_driver() 宏，是为了把这两步的顺序显式写出来。
 */
static int __init w25q64_init(void)
{
	int ret;

	w25q64_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(w25q64_class))
		return PTR_ERR(w25q64_class);

	ret = spi_register_driver(&w25q64_driver);
	if (ret) {
		class_destroy(w25q64_class);
		w25q64_class = NULL;
		return ret;
	}

	pr_info("w25q64: 驱动已加载\n");
	return 0;
}

static void __exit w25q64_exit(void)
{
	spi_unregister_driver(&w25q64_driver);
	class_destroy(w25q64_class);
	pr_info("w25q64: 驱动已卸载\n");
}

module_init(w25q64_init);
module_exit(w25q64_exit);

MODULE_AUTHOR("fxy");
MODULE_DESCRIPTION("W25Q64 SPI NOR Flash character device driver for Taishan Pi RK3566");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:w25q64");
