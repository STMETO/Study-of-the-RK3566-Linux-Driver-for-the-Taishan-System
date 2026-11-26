// SPDX-License-Identifier: GPL-2.0
/*
 * at24c02.c - AT24C02（I2C 接口的 256 字节 EEPROM）字符设备驱动
 *
 * ============================ 这个驱动想教你什么 ============================
 *
 * 前面 led8 工程学的是"纯 platform + 裸 cdev"：设备是自己编出来的，
 * 寄存器是内存模拟的，所以 probe 什么时候被调用、参数从哪来，全靠 platform
 * 框架。这一版把"设备从哪来"换成了真实的 I2C 总线：
 *
 *   设备树里的 eeprom@50 子节点
 *        │  （i2c 核心在注册适配器/设备时解析）
 *        ▼
 *   i2c_client（内核里代表"总线上的一颗从设备"）
 *        │  （i2c 核心拿 compatible / id 去和驱动匹配）
 *        ▼
 *   at24c02_probe(client, id)  ← 本文件
 *        │
 *        ▼
 *   注册字符设备 /dev/at24c02
 *
 * 也就是说：platform 驱动里 probe 的参数是 pdev（平台设备），这里换成了
 * client（I2C 从设备）。除此之外，字符设备的注册、fops、ioctl 那一套
 * 和 led8 完全一样——这正是本工程想让你对照着看的地方。
 *
 * ============================ 硬件怎么访问 ============================
 *
 * AT24C02 是 24Cxx 系列里最小的一颗：2Kbit = 256 字节，I2C 地址 0x50~0x57
 * （由 A0/A1/A2 引脚决定），页大小 8 字节，写周期最大 5ms。
 *
 * 它的访问协议非常简单：
 *   写一个字节：START | 从机地址+W | 字地址(1字节) | 数据 | STOP
 *   读一个字节：START | 从机地址+W | 字地址 | RESTART | 从机地址+R | 数据 | STOP
 *
 * 注意"字地址"只有 1 个字节，所以这颗芯片的寻址空间天然就是 0~255。
 * 更大的 24C32/64 要发 2 字节地址，24C04/08/16 则把高位地址塞进从机地址里，
 * 写法都不一样——本驱动只覆盖 1 字节地址的 24C01/24C02。
 *
 * 内核 I2C 子系统把上面这套时序抽象成了 i2c_smbus_* 一族函数：
 *   i2c_smbus_read_byte_data(client, addr)            读 1 字节
 *   i2c_smbus_write_byte_data(client, addr, val)      写 1 字节
 *   i2c_smbus_read_i2c_block_data(client, addr, n, buf)   连续读 n 字节
 *   i2c_smbus_write_i2c_block_data(client, addr, n, buf)  连续写 n 字节（≤32）
 *
 * 为什么不自己拼 i2c_msg 发原始时序？因为 i2c-stub 这个"假总线"只实现了
 * SMBus 层，用 SMBus API 写的驱动可以在完全没有硬件的情况下跑通全流程。
 * 而且 SMBus API 也是绝大多数真实 EEPROM/传感器驱动的写法。
 *
 * ============================ 两个必须记住的硬件约束 ============================
 *
 * 1. 页写不能跨页。
 *    AT24C02 内部有 32 个 8 字节的页。一次页写如果从地址 6 开始写 4 个字节，
 *    硬件会把第 3、4 个字节"绕回"到本页开头的 0、1 地址，而不是写到 8、9。
 *    所以驱动必须自己把用户的写请求按页边界切开，一页一次页写。
 *
 * 2. 每次写之后要等 tWR。
 *    芯片内部在擦写期间不响应 I2C（NAK），必须等它写完，最长 5ms。
 *    常见两种做法：固定 msleep(5)，或者"ACK 轮询"（反复发地址直到应答）。
 *    本驱动用固定延时，简单可靠。
 *
 * ============================ 字符设备这一侧 ============================
 *
 * read/write 的语义设计成"把芯片当成一个 256 字节的文件"：
 *   - 文件偏移（*ppos）就是 EEPROM 内部字节地址
 *   - pread(fd, buf, n, 0x10) 相当于从地址 0x10 开始读
 *   - pwrite(fd, buf, n, 0x20) 相当于从地址 0x20 开始写
 *   - lseek 到超出末尾会被钳到 256，read 到末尾返回 0
 * 这样用户态用最普通的 read/write/lseek 就能操作 EEPROM，不用记寄存器。
 *
 * ioctl 则提供"元信息 + 整片操作"：容量、页大小、从机地址、擦除。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/errno.h>

#include "at24c02_uapi.h"

/* 设备名：决定 /dev/at24c02 和 /sys/class/at24c02/at24c02 的名字 */
#define DEVICE_NAME		"at24c02"

/*
 * 容量上限。
 * 支持范围是 1~256 字节：只覆盖 1 字节字地址的小容量 24C01/24C02。
 * 如果设备树里写成 512 或更大，probe 会直接报错——因为本驱动不会发
 * 2 字节地址，硬跑只会读到错误的数据，不如早点失败。
 */
#define AT24C02_MAX_SIZE	256

/* 设备树没写属性时的默认值（AT24C02 的真实参数） */
#define AT24C02_DEF_SIZE	256
#define AT24C02_DEF_PAGE	8

/* SMBus I2C block 单次最多 32 字节（I2C_SMBUS_BLOCK_MAX） */
#define AT24C02_BLOCK_MAX	32

/* 页大小上限：擦除时用栈上缓冲拼一页，所以给个明确上限 */
#define AT24C02_MAX_PAGE	32

/* 写周期：AT24C02 数据手册 tWR 最大 5ms */
#define AT24C02_TWR_MS		5

/*
 * 驱动私有数据。
 *
 * 一个 at24c02_dev 对应总线上的一颗 EEPROM。它把"字符设备那一半"
 * （cdev/devt/dev）和"I2C 那一半"（client）绑在一起，这样 fops 里
 * 从 inode 找回 cdev、再 container_of 回 priv，就能同时拿到两边。
 */
struct at24c02_dev {
	struct i2c_client *client;	/* 对应的 I2C 从设备（probe 参数） */
	struct cdev cdev;		/* 字符设备对象 */
	struct device *dev;		/* /sys/class/at24c02/at24c02 */
	dev_t devt;			/* 设备号 major:minor */
	struct mutex lock;		/* 串行化对芯片的访问 */
	u32 size;			/* EEPROM 容量（字节） */
	u32 page_size;			/* 页大小（字节，2 的幂） */
	bool use_block;			/* 适配器是否支持 I2C block 传输 */
};

/*
 * 字符设备类。全局只有一个，module_init 里创建、module_exit 里销毁。
 * 它在 sysfs 里体现为 /sys/class/at24c02/，下面的每个设备是它的子目录。
 */
static struct class *at24c02_class;

/* ========================================================================== */
/*                          一、硬件访问层（真正碰 I2C 的地方）                 */
/* ========================================================================== */
/*
 * 这一层故意和字符设备逻辑分开：
 *   - 上层只管"用户想读/写哪段地址、多长"
 *   - 这一层管"怎么把这批数据变成 I2C 事务"
 * 以后换一颗同协议但容量更大的 EEPROM，只需要改这里的寻址方式，
 * fops 和 ioctl 一行都不用动。
 */

/*
 * 从 EEPROM 的 addr 开始连续读 len 字节。
 *
 * 优先用块读（一次 I2C 事务搬最多 32 字节），适配器不支持时退化成
 * 逐字节读。EEPROM 的连续读不受页限制，所以这里只需要按 32 字节切片。
 *
 * 调用者必须已经持有 priv->lock。
 * 返回 0 成功，负数是 I2C 错误码（-EREMOTEIO 表示从机没应答）。
 */
static int at24c02_read_bytes(struct at24c02_dev *priv, u32 addr,
			      u8 *buf, u32 len)
{
	struct i2c_client *client = priv->client;
	u32 done = 0;

	while (done < len) {
		u32 chunk = min_t(u32, len - done, AT24C02_BLOCK_MAX);
		int ret;

		if (priv->use_block) {
			/*
			 * 块读：command 是字地址，返回实际读到的字节数。
			 * 正常情况返回值 == chunk；偏小说明从机提前停止。
			 */
			ret = i2c_smbus_read_i2c_block_data(client,
							    addr + done,
							    chunk, buf + done);
			if (ret < 0)
				return ret;
			if (ret != chunk) {
				dev_err(&client->dev,
					"short block read at 0x%02x: %d/%u\n",
					addr + done, ret, chunk);
				return -EIO;
			}
		} else {
			/*
			 * 退化路径：一次只读 1 字节。
			 * 每字节都是一次完整的"写地址 + 读数据"事务，比较慢，
			 * 但几乎所有 I2C 适配器都支持 BYTE_DATA。
			 */
			u32 i;

			for (i = 0; i < chunk; i++) {
				ret = i2c_smbus_read_byte_data(client,
							       addr + done + i);
				if (ret < 0)
					return ret;
				buf[done + i] = (u8)ret;
			}
		}

		done += chunk;
	}

	return 0;
}

/*
 * 写"一页"（调用者保证 addr 对齐到页首、len ≤ page_size）。
 *
 * 写完必须等 tWR，否则紧接着的下一次访问会被芯片 NAK。
 * 调用者必须已经持有 priv->lock。
 */
static int at24c02_write_page(struct at24c02_dev *priv, u32 addr,
			      const u8 *buf, u32 len)
{
	struct i2c_client *client = priv->client;
	int ret;

	if (priv->use_block) {
		ret = i2c_smbus_write_i2c_block_data(client, addr, len, buf);
	} else {
		u32 i;

		ret = 0;
		for (i = 0; i < len; i++) {
			ret = i2c_smbus_write_byte_data(client, addr + i,
							buf[i]);
			if (ret < 0)
				break;
		}
	}

	if (ret < 0) {
		dev_err(&client->dev, "write failed at 0x%02x: %d\n", addr, ret);
		return ret;
	}

	/* 等芯片内部擦写完成 */
	msleep(AT24C02_TWR_MS);

	return 0;
}

/*
 * 把 [addr, addr+len) 这段数据写进芯片，自动按页切分。
 *
 * 这是"页写不能跨页"这条硬件约束的唯一处理点：
 *   room  = 当前页还剩多少字节可写
 *   chunk = 本次最多写多少（既不超过剩余长度，也不越过页边界）
 */
static int at24c02_write_range(struct at24c02_dev *priv, u32 addr,
			       const u8 *buf, u32 len)
{
	while (len > 0) {
		/* 当前地址到本页末尾还有多少空间 */
		u32 room = priv->page_size - (addr & (priv->page_size - 1));
		u32 chunk = min_t(u32, len, room);
		int ret;

		ret = at24c02_write_page(priv, addr, buf, chunk);
		if (ret < 0)
			return ret;

		addr += chunk;
		buf  += chunk;
		len  -= chunk;
	}

	return 0;
}

/*
 * 整片擦除：把每个字节写成 0xFF（EEPROM 擦除后的默认值）。
 * 按页写，一页一次，32 页 × 5ms ≈ 160ms。
 * 调用者必须已经持有 priv->lock。
 */
static int at24c02_erase(struct at24c02_dev *priv)
{
	u8 page[AT24C02_MAX_PAGE];
	u32 addr;

	memset(page, 0xFF, sizeof(page));

	for (addr = 0; addr < priv->size; addr += priv->page_size) {
		u32 chunk = min_t(u32, priv->page_size, priv->size - addr);
		int ret;

		ret = at24c02_write_page(priv, addr, page, chunk);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* ========================================================================== */
/*                          二、字符设备操作（和 led8 几乎一样）                */
/* ========================================================================== */

/*
 * open：把驱动的私有数据挂到 file->private_data 上。
 *
 * inode->i_cdev 是内核在 open 时根据设备号填好的 cdev 指针，
 * 用 container_of 反推出外层的 at24c02_dev——这是"裸 cdev"驱动的标准动作。
 * 后面的 read/write/ioctl 都从 private_data 直接取，不用再查表。
 */
static int at24c02_open(struct inode *inode, struct file *filp)
{
	struct at24c02_dev *priv;

	priv = container_of(inode->i_cdev, struct at24c02_dev, cdev);
	filp->private_data = priv;

	return 0;
}

static int at24c02_release(struct inode *inode, struct file *filp)
{
	filp->private_data = NULL;
	return 0;
}

/*
 * read：从 *ppos 指定的 EEPROM 地址开始读。
 *
 * 返回 0 表示读到芯片末尾（EOF），这是字符设备的常规语义，
 * 让 cat /dev/at24c02 能自然结束。
 */
static ssize_t at24c02_read(struct file *filp, char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct at24c02_dev *priv = filp->private_data;
	u8 *kbuf;
	size_t len;
	int ret;

	/* 偏移已经越过芯片容量：读到文件末尾 */
	if (*ppos < 0 || *ppos >= (loff_t)priv->size)
		return 0;

	/* 一次最多读到芯片末尾，不越界 */
	len = min_t(size_t, count, (size_t)(priv->size - *ppos));
	if (len == 0)
		return 0;

	kbuf = kmalloc(len, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	/*
	 * 拿锁 → 读芯片 → 放锁。
	 * 注意不要在持锁期间调 copy_to_user：用户页可能缺页、需要睡眠，
	 * 而且持锁做用户空间拷贝会拉长临界区。
	 */
	mutex_lock(&priv->lock);
	ret = at24c02_read_bytes(priv, (u32)*ppos, kbuf, len);
	mutex_unlock(&priv->lock);

	if (ret < 0) {
		kfree(kbuf);
		return ret;
	}

	if (copy_to_user(ubuf, kbuf, len)) {
		kfree(kbuf);
		return -EFAULT;
	}

	kfree(kbuf);

	/* 只有全部成功才推进偏移，失败时保持原位置方便重试 */
	*ppos += len;

	return len;
}

/*
 * write：往 *ppos 指定的 EEPROM 地址写。
 *
 * 越界返回 -ENOSPC（而不是静默截断），让用户态能明确知道"写超了"。
 * 真正的页切分在 at24c02_write_range() 里做。
 */
static ssize_t at24c02_write(struct file *filp, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct at24c02_dev *priv = filp->private_data;
	u8 *kbuf;
	size_t len;
	int ret;

	if (*ppos < 0)
		return -EINVAL;

	if (*ppos >= (loff_t)priv->size)
		return -ENOSPC;

	len = min_t(size_t, count, (size_t)(priv->size - *ppos));
	if (len == 0)
		return -ENOSPC;

	kbuf = kmalloc(len, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	/* 先把用户数据整批拷进内核，避免持锁时反复进出用户空间 */
	if (copy_from_user(kbuf, ubuf, len)) {
		kfree(kbuf);
		return -EFAULT;
	}

	mutex_lock(&priv->lock);
	ret = at24c02_write_range(priv, (u32)*ppos, kbuf, len);
	mutex_unlock(&priv->lock);

	kfree(kbuf);

	if (ret < 0)
		return ret;

	*ppos += len;

	return len;
}

/*
 * llseek：支持 SEEK_SET / SEEK_CUR / SEEK_END。
 *
 * 新位置会被钳在 [0, size]：
 *   - 负数 → 0
 *   - 超过容量 → size（此时 read 直接 EOF，write 返回 -ENOSPC）
 * 钳位而不是报错，是为了让 "lseek(fd, 0, SEEK_END)" 这种常见写法
 * 总能落到合法位置。
 */
static loff_t at24c02_llseek(struct file *filp, loff_t off, int whence)
{
	struct at24c02_dev *priv = filp->private_data;
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = off;
		break;
	case SEEK_CUR:
		newpos = filp->f_pos + off;
		break;
	case SEEK_END:
		newpos = (loff_t)priv->size + off;
		break;
	default:
		return -EINVAL;
	}

	if (newpos < 0)
		newpos = 0;
	if (newpos > (loff_t)priv->size)
		newpos = priv->size;

	filp->f_pos = newpos;

	return newpos;
}

/*
 * ioctl：元信息查询 + 整片擦除。
 *
 * 命令编号定义在 include/at24c02_uapi.h，驱动和 CLI 共用。
 * 先检查 magic，避免把别的设备的 ioctl 误当成自己的命令处理。
 */
static long at24c02_unlocked_ioctl(struct file *filp, unsigned int cmd,
				   unsigned long arg)
{
	struct at24c02_dev *priv = filp->private_data;
	struct at24c02_info info;
	int ret;

	if (_IOC_TYPE(cmd) != AT24C02_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	/*
	 * 这三个值在 probe 之后就不再变化，所以不需要持锁，
	 * 直接用 put_user 写回用户空间。
	 */
	case AT24C02_IOC_GET_SIZE:
		return put_user((int)priv->size, (int __user *)arg);

	case AT24C02_IOC_GET_PAGE_SIZE:
		return put_user((int)priv->page_size, (int __user *)arg);

	case AT24C02_IOC_GET_ADDR:
		return put_user((int)priv->client->addr, (int __user *)arg);

	/* 一次拿回全部参数，省两次系统调用 */
	case AT24C02_IOC_GET_INFO:
		info.addr      = priv->client->addr;
		info.size      = priv->size;
		info.page_size = priv->page_size;
		info.flags     = priv->use_block ? 1 : 0;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;

	/* 整片擦除：慢操作，持锁期间其它 read/write 会等待 */
	case AT24C02_IOC_ERASE:
		mutex_lock(&priv->lock);
		ret = at24c02_erase(priv);
		mutex_unlock(&priv->lock);
		return ret;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations at24c02_fops = {
	.owner		= THIS_MODULE,
	.open		= at24c02_open,
	.release	= at24c02_release,
	.read		= at24c02_read,
	.write		= at24c02_write,
	.llseek		= at24c02_llseek,
	.unlocked_ioctl	= at24c02_unlocked_ioctl,
};

/* ========================================================================== */
/*                          三、I2C 驱动框架这一侧                             */
/* ========================================================================== */

/*
 * probe：i2c 核心发现"设备树里的节点"和"驱动的匹配表"对上了，就会调用它。
 *
 * 这里的活儿分成两段：
 *   1. I2C 设备初始化：读设备树参数、检查适配器能力、探测芯片在不在
 *   2. 字符设备注册：alloc_chrdev_region → cdev_init → cdev_add →
 *      device_create（和 led8 的 probe 一模一样）
 *
 * 任何一步失败都 goto 到对应的回滚标签，严格按注册的逆序释放资源。
 */
static int at24c02_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct at24c02_dev *priv;
	int ret;

	dev_info(dev, "probing %s at 0x%02x on %s\n",
		 id ? id->name : DEVICE_NAME, client->addr,
		 client->adapter->name);

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;

	/*
	 * 从设备树读参数，读不到就用默认值。
	 * 设备树里写 size/pagesize 属性可以让同一份驱动适配 24C01（128B，页 8），
	 * 只要容量不超过 AT24C02_MAX_SIZE 就行。
	 */
	priv->size      = AT24C02_DEF_SIZE;
	priv->page_size = AT24C02_DEF_PAGE;

	if (dev->of_node) {
		u32 v;

		if (of_property_read_u32(dev->of_node, "size", &v) == 0)
			priv->size = v;
		if (of_property_read_u32(dev->of_node, "pagesize", &v) == 0)
			priv->page_size = v;
	}

	/* 参数合法性检查：宁可 probe 失败，也不要带着错参数跑 */
	if (priv->size == 0 || priv->size > AT24C02_MAX_SIZE) {
		dev_err(dev, "invalid size %u (must be 1..%u)\n",
			priv->size, AT24C02_MAX_SIZE);
		ret = -EINVAL;
		goto err_free;
	}

	if (priv->page_size == 0 || priv->page_size > AT24C02_MAX_PAGE ||
	    (priv->page_size & (priv->page_size - 1)) != 0 ||
	    priv->page_size > priv->size) {
		dev_err(dev, "invalid pagesize %u (power of 2, <= %u, <= size)\n",
			priv->page_size, AT24C02_MAX_PAGE);
		ret = -EINVAL;
		goto err_free;
	}

	/*
	 * 检查适配器能力：支持 I2C block 就用块传输，否则逐字节。
	 * 这一步在你自己的板子上尤其重要——不同 SoC 的 I2C 控制器
	 * 支持的 SMBus 子集不一样。
	 */
	priv->use_block = i2c_check_functionality(client->adapter,
						  I2C_FUNC_SMBUS_I2C_BLOCK);
	dev_info(dev, "adapter %s: %s transfers\n", client->adapter->name,
		 priv->use_block ? "block" : "byte-by-byte");

	mutex_init(&priv->lock);

	/*
	 * 探测芯片：读一下地址 0。
	 *
	 * 读失败只警告、不返回错误——这样在"还没有接真实 EEPROM"的板子上，
	 * 驱动依然能加载、能建出 /dev/at24c02，方便你先把字符设备那套流程
	 * 跑通。等真接上芯片，这里就会变成成功。
	 */
	ret = i2c_smbus_read_byte_data(client, 0);
	if (ret < 0)
		dev_warn(dev, "no ACK from 0x%02x (%d), node will still be created\n",
			 client->addr, ret);
	else
		dev_info(dev, "device replied, byte[0] = 0x%02x\n", ret);

	/* ---------------- 从这里开始是字符设备注册 ---------------- */

	/* 动态申请一个设备号（major 由内核分配） */
	ret = alloc_chrdev_region(&priv->devt, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		dev_err(dev, "alloc_chrdev_region failed: %d\n", ret);
		goto err_mutex;
	}

	cdev_init(&priv->cdev, &at24c02_fops);
	priv->cdev.owner = THIS_MODULE;

	ret = cdev_add(&priv->cdev, priv->devt, 1);
	if (ret < 0) {
		dev_err(dev, "cdev_add failed: %d\n", ret);
		goto err_region;
	}

	/* 在内核里创建 /dev/at24c02（实际由 udev/mdev 在用户态建节点） */
	priv->dev = device_create(at24c02_class, NULL, priv->devt, priv,
				  DEVICE_NAME);
	if (IS_ERR(priv->dev)) {
		ret = PTR_ERR(priv->dev);
		dev_err(dev, "device_create failed: %d\n", ret);
		goto err_cdev;
	}

	/*
	 * 把 priv 存进 client 的私有字段：remove 时用
	 * i2c_get_clientdata(client) 取回来。
	 */
	i2c_set_clientdata(client, priv);

	dev_info(dev, "ready: /dev/%s major=%d minor=%d size=%u page=%u\n",
		 DEVICE_NAME, MAJOR(priv->devt), MINOR(priv->devt),
		 priv->size, priv->page_size);

	return 0;

	/* 失败回滚：严格按注册的逆序 */
err_cdev:
	cdev_del(&priv->cdev);
err_region:
	unregister_chrdev_region(priv->devt, 1);
err_mutex:
	mutex_destroy(&priv->lock);
err_free:
	kfree(priv);
	return ret;
}

/*
 * remove：设备被移除（或驱动卸载、或 sysfs delete_device）时调用。
 *
 * 注意 6.1 内核里 i2c_driver 的 remove 回调返回 void，
 * 而 platform_driver 的 remove 返回 int —— 这是两个框架的差异，
 * 从 led8 迁移过来时最容易踩。
 */
static void at24c02_remove(struct i2c_client *client)
{
	struct at24c02_dev *priv = i2c_get_clientdata(client);

	if (!priv)
		return;

	/* 和 probe 完全镜像：先摘字符设备，再释放私有数据 */
	device_destroy(at24c02_class, priv->devt);
	cdev_del(&priv->cdev);
	unregister_chrdev_region(priv->devt, 1);
	mutex_destroy(&priv->lock);

	dev_info(&client->dev, "removed\n");

	kfree(priv);
}

/*
 * 匹配表一：设备树 compatible。
 * 设备树里写 compatible = "work,at24c02" 就走这条。
 */
static const struct of_device_id at24c02_of_match[] = {
	{ .compatible = "work,at24c02" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, at24c02_of_match);

/*
 * 匹配表二：I2C 设备 ID 表。
 * 这张表让"非设备树"的实例化方式也能工作，例如在板子上手写：
 *     echo at24c02 0x50 > /sys/bus/i2c/devices/i2c-2/new_device
 * 内核会创建 i2c_client，再用名字 "at24c02" 匹配到这个驱动。
 * 本工程的 fake_i2c.sh 就是靠这条路径配合 i2c-stub 做无硬件测试的。
 */
static const struct i2c_device_id at24c02_id[] = {
	{ "at24c02", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, at24c02_id);

static struct i2c_driver at24c02_driver = {
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table	= at24c02_of_match,
	},
	.probe		= at24c02_probe,
	.remove		= at24c02_remove,
	.id_table	= at24c02_id,
};

/* ========================================================================== */
/*                          四、模块入口与出口                                 */
/* ========================================================================== */
/*
 * 这里不用 module_i2c_driver() 宏，而是显式写 init/exit：
 *   1. 让你看清楚 class_create 和 i2c_add_driver 的先后顺序
 *   2. class 必须在注册设备之前存在，所以放在前面
 *   3. 卸载顺序严格反过来
 */

static int __init at24c02_init(void)
{
	int ret;

	/*
	 * 创建 /sys/class/at24c02。
	 * 第一个参数是 THIS_MODULE，表示这个 class 归本模块所有——
	 * 模块卸载时如果 class 里还有设备，内核会报错提醒你先清理。
	 */
	at24c02_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(at24c02_class)) {
		ret = PTR_ERR(at24c02_class);
		pr_err("at24c02: class_create failed: %d\n", ret);
		return ret;
	}

	/*
	 * 注册 i2c_driver。
	 * i2c_add_driver 内部会遍历所有已注册的 i2c_client，把匹配的
	 * 设备逐个 probe 掉；如果设备树节点已经存在，probe 会在这里被调用。
	 */
	ret = i2c_add_driver(&at24c02_driver);
	if (ret < 0) {
		pr_err("at24c02: i2c_add_driver failed: %d\n", ret);
		class_destroy(at24c02_class);
		at24c02_class = NULL;
		return ret;
	}

	pr_info("at24c02: driver loaded\n");

	return 0;
}

static void __exit at24c02_exit(void)
{
	/* 先摘驱动（会触发所有设备 remove），再销毁 class */
	i2c_del_driver(&at24c02_driver);

	if (at24c02_class) {
		class_destroy(at24c02_class);
		at24c02_class = NULL;
	}

	pr_info("at24c02: driver unloaded\n");
}

module_init(at24c02_init);
module_exit(at24c02_exit);

MODULE_AUTHOR("fxy <3332823540@qq.com>");
MODULE_DESCRIPTION("AT24C02 I2C EEPROM char device driver (platform-style teaching sample)");
MODULE_LICENSE("GPL");
MODULE_ALIAS("i2c:at24c02");
