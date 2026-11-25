// SPDX-License-Identifier: GPL-2.0
/*
 * led8.c ：虚拟 8 路 LED 调光板（泰山派 RK3566）
 *
 * ============================================================================
 * 一、这个驱动想教你什么
 * ============================================================================
 * 上一个工程 gpio3a1 用的是 miscdevice，好处是"一行 misc_register 就有 /dev 节点"，
 * 但代价是注册细节全被框架包住了。本工程故意不用 misc，把 Linux 字符设备
 * 最主流、最原始的注册流程完完整整摊开：
 *
 *   模块加载（led8_init）
 *     class_create()            -> 在 /sys/class 下建 led8 这个类
 *     platform_driver_register()-> 把驱动挂到 platform 总线，等设备来匹配
 *
 *   设备匹配成功（led8_probe）
 *     alloc_chrdev_region()     -> 第 1 步：向内核申请设备号（主设备号动态分配）
 *     cdev_init() / cdev_add()  -> 第 2、3 步：初始化 cdev 并加入内核 cdev 表
 *     device_create()           -> 第 4 步：创建 /dev/led8 节点和 /sys/class/led8/led8/
 *
 *   设备移除（led8_remove）
 *     device_destroy()          -> 严格逆序拆
 *     cdev_del()
 *     unregister_chrdev_region()
 *
 *   模块卸载（led8_exit）
 *     platform_driver_unregister()
 *     class_destroy()
 *
 * ============================================================================
 * 二、硬件模型（为什么不需要真实外设）
 * ============================================================================
 * 这块"LED 调光板"上电后表现为一个寄存器文件：
 *   - 8 个通道（设备树 channels 属性可配，最多 16）
 *   - 每个通道 1 字节亮度，0 = 灭，255 = 最亮
 *
 * 现在这些"寄存器"就是 priv->regs[] 这块内核内存，led8_hw_commit() 是空函数。
 * 以后接 PCA9685 这类真实 I2C PWM 芯片时，只需要改 led8_hw_commit()/led8_hw_read()
 * 两个函数（把内存读写换成 i2c_smbus_* 调用），本文件里 cdev、fops、ioctl
 * 这些"框架代码"一行都不用动。这就是"先学框架，再上总线"的增量路径。
 *
 * ============================================================================
 * 三、用户态接口（/dev/led8）
 * ============================================================================
 *   read(fd, buf, n)     从当前文件偏移读 n 个通道的亮度，一字节一个通道
 *   write(fd, buf, n)    从当前文件偏移写 n 个通道的亮度
 *   lseek(fd, off, ...)  把文件偏移当"通道号"用，支持 SEEK_SET/CUR/END
 *   ioctl(fd, cmd, arg)  见 include/led8_uapi.h：
 *                          LED8_IOC_GET_CHANNELS / GET_ALL / SET_ALL / OFF / FULL
 *
 * 把偏移当通道号，是为了让这个"假设备"也能完整演示 ppos 语义：
 *   用户态 pread(fd, &v, 1, 3) 就等于"读 3 号通道"。
 *
 * ============================================================================
 * 四、和 misc 版本（gpio3a1）的关键差异
 * ============================================================================
 *   | 项目            | miscdevice                | 裸 cdev（本文件）              |
 *   |-----------------|---------------------------|--------------------------------|
 *   | 主设备号        | 固定 10                   | alloc_chrdev_region 动态分配    |
 *   | 注册步骤        | misc_register 一行         | chrdev_region + cdev + device   |
 *   | /dev 节点       | 框架创建                   | device_create() 创建            |
 *   | file->private_data | misc_open 自动设成 &miscdev | 本驱动 open() 里自己设       |
 *   | 定位             | 简单小设备                 | /dev 下所有真正的主力字符设备    |
 *
 * 下面每个函数都写了详细注释，可以按顺序从上往下读。
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "led8_uapi.h"

/* platform_driver.driver.name，用于和 platform 设备（或 of 匹配）绑定 */
#define DRIVER_NAME		"led8"
/* 设备名：/dev/led8、/sys/class/led8/led8，也是 /proc/devices 里注册的名字 */
#define DEVICE_NAME		"led8"
/* 设备树里没写 channels 属性时的默认通道数 */
#define LED8_DEFAULT_CHANNELS	8

/**
 * struct led8_dev - 每个 led8 设备实例的私有上下文
 * @cdev:     字符设备对象，cdev_add 之后内核就能通过设备号找到我们的 fops
 * @dev:      device_create 返回的 struct device，对应 /sys/class/led8/led8/
 * @devt:     本实例的设备号（主设备号 + 次设备号），由 alloc_chrdev_region 分配
 * @lock:     保护 regs[] 的互斥锁。read/write/ioctl 都会访问寄存器，
 *            并发时靠它保证"读到的是一组完整的通道值"
 * @channels: 实际通道数（1~LED8_MAX_CHANNELS），来自设备树 channels 属性
 * @regs:     模拟的硬件寄存器文件。下标 = 通道号，值 = 亮度 0~255
 *
 * 为什么要把这些字段打包成一个结构体，而不是用全局变量？
 *   1. 一个驱动可以对应多个设备实例（多个 platform 设备 / 多个 minor），
 *      每个实例有自己的 regs 和锁，全局变量做不到；
 *   2. open() 里通过 container_of 从 inode->i_cdev 反推出本结构体，
 *      后面 read/write/ioctl 全都从 file->private_data 拿到它。
 */
struct led8_dev {
	struct cdev		cdev;		// 给 VFS 用的
	struct device		*dev;	// 给内核设备模型、sysfs、规范设备日志、扩展 sysfs 属性用的。
	dev_t			devt;
	struct mutex		lock;
	u32			channels;
	u8			regs[LED8_MAX_CHANNELS];
};

/*
 * class_create() 创建出来的设备类，全局只有一份。
 * 设备类的作用：让 /sys/class/led8/ 有统一归属，device_create 需要它；
 * 同时 udv/mdev 收到内核 uevent 后，会按 "class + dev_t" 去创建 /dev 节点。
 */
 // struct class代表一个设备大类（比如 /sys/class/leds/、/sys/class/tty/、你这里 /sys/class/led8/）
 // 一个 class 是一类设备的容器，不是一个设备实例的东西
 // class 描述一类设备，所有同类型的 led8 设备实例（多个 platform 设备）共用同一个 class。
 //如果放到led8_dev里面，那每一个设备实例都去 class_create，就会在 /sys/class/下面创建一堆同名 class，直接报错、冲突，这是错误写法。
static struct class *led8_class;

/* ==========================================================================
 * 硬件抽象层：把"怎么操作真实硬件"关在这两个函数里
 * ========================================================================== */

/**
 * led8_hw_commit - 把 regs[] 的当前内容"下发"到硬件
 * @priv: 设备上下文
 *
 * 现在是虚拟设备，什么也不用做（寄存器就是内存本身）。
 * 以后接 PCA9685 之类的 I2C PWM 芯片，这里换成类似：
 *     i2c_smbus_write_i2c_block_data(priv->client, REG_PWM0, priv->channels, priv->regs);
 * 上层代码不需要任何改动。
 *
 * 注意：调用本函数时必须已经持有 priv->lock（保证下发的是同一批数据）。
 */
static void led8_hw_commit(struct led8_dev *priv)
{
	/* 虚拟设备：无实际操作。真实硬件在这里做总线写。 */
	(void)priv;
}

/**
 * led8_hw_read - 读取某个通道当前的亮度
 * @priv: 设备上下文
 * @ch:   通道号（调用者保证 0 <= ch < channels）
 *
 * 返回：该通道亮度 0~255。
 * 真实硬件版本这里应该是 i2c_smbus_read_byte_data() 之类的读操作；
 * 现在直接返回内存里的寄存器值。
 *
 * 注意：调用本函数时必须已经持有 priv->lock。
 */
static u8 led8_hw_read(struct led8_dev *priv, u32 ch)
{
	return priv->regs[ch];
}

/* ==========================================================================
 * 文件操作（fops）实现
 * ========================================================================== */

/**
 * led8_from_file - 从 struct file 取出本设备的私有上下文
 * @file: open() 之后拿到的文件对象
 *
 * open() 里我们做了 file->private_data = priv，
 * 所以这里直接取出来即可。
 *
 * 对比 misc 版本：misc_open() 会把 private_data 设成 &miscdev，
 * 驱动还得 container_of 一次才能拿到自己的结构体。
 * 裸 cdev 没有这层包装，private_data 放什么完全由驱动自己决定。
 */
static struct led8_dev *led8_from_file(struct file *file)
{
	return file->private_data;
}

/**
 * led8_open - .open 回调：应用 open("/dev/led8") 时进来
 * @inode: 内核里的 inode，i_cdev 指向 cdev_add 时注册的那个 cdev
 * @file:  新建的 file 对象
 *
 * 这里做两件事：
 *   1. 用 container_of 从 inode->i_cdev 反推出 led8_dev 的首地址
 *      （cdev 是 led8_dev 的第一个成员，但 container_of 不依赖这个前提）；
 *   2. 把 priv 存进 file->private_data，供 read/write/ioctl 使用。
 *
 * 为什么不用 global 变量？因为 cdev 可能被多个实例共用同一套 fops，
 * 只有 inode->i_cdev 才能告诉你"这次打开的是哪个实例"。
 */
static int led8_open(struct inode *inode, struct file *file)
{
	struct led8_dev *priv;

	priv = container_of(inode->i_cdev, struct led8_dev, cdev);
	file->private_data = priv;

	return 0;
}

/**
 * led8_release - .release 回调：最后一个 close() 时进来
 * @inode: inode
 * @file:  文件对象
 *
 * 把 private_data 清掉，和 open 对称。
 * 本驱动没有额外资源要释放（regs 是设备级的，不是每次打开一份）。
 */
static int led8_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

/**
 * led8_read - 从"寄存器文件"读数据
 * @file:  文件对象
 * @buf:   用户缓冲区（__user 标记表示这是用户空间地址，不能直接解引用）
 * @count: 用户想读多少字节
 * @ppos:  文件当前偏移。对本设备来说，偏移 = 起始通道号
 *
 * 语义：
 *   - 从通道 *ppos 开始，最多读 (channels - *ppos) 个字节；
 *   - 一次拷贝不超过 LED8_MAX_CHANNELS 字节（栈上缓冲区固定大小）；
 *   - 读成功后 *ppos 前进实际读到的字节数，和普通文件行为一致；
 *   - 偏移已经到结尾时返回 0（EOF）。
 *
 * 返回值：实际读到的字节数；出错返回负 errno。
 */
static ssize_t led8_read(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct led8_dev *priv = led8_from_file(file);
	u8 kbuf[LED8_MAX_CHANNELS];
	size_t n;
	u32 off;
	u32 i;

	if (!count)
		return 0;

	/* 偏移非法或已经读到底：按文件语义返回 0（EOF），不要报错 */
	if (*ppos < 0 || *ppos >= (loff_t)priv->channels)
		return 0;

	off = (u32)*ppos;
	// 可以读的数量和想要读的数量取最小值
	n = min_t(size_t, count, (size_t)(priv->channels - off));

	/*
	 * 先在锁内把数据拷到内核栈缓冲区，再解锁去 copy_to_user。
	 * 不要反过来（先 copy_to_user 再读寄存器）：
	 *   - 持锁做 copy_to_user 可能在缺页时睡眠，锁被长时间占住；
	 *   - 中途解锁再逐字节读，可能读到"一半新一半旧"的通道值。
	 */
	mutex_lock(&priv->lock);
	for (i = 0; i < n; i++)
		kbuf[i] = led8_hw_read(priv, off + i);
	mutex_unlock(&priv->lock);

	/* 把内核缓冲区拷到用户空间；失败一般是用户传了非法地址 */
	if (copy_to_user(buf, kbuf, n))
		return -EFAULT;

	// 用户传给read的是 “希望从哪里读” 的意图；内核读完之后，要更新ppos，记下新的文件位置，给下一次 read用
	*ppos += n;	// loff_t *ppos 既是输入参数，也是输出参数
	return n;
}

/**
 * led8_write - 往"寄存器文件"写数据
 * @file:  文件对象
 * @buf:   用户缓冲区
 * @count: 用户想写多少字节
 * @ppos:  文件当前偏移 = 起始通道号
 *
 * 语义：
 *   - 从通道 *ppos 开始，最多写 (channels - *ppos) 个字节；
 *   - 每个字节就是对应通道的亮度（0~255，写多少就是多少）；
 *   - 偏移超出通道数时返回 -ENOSPC（写到设备末尾之外）；
 *   - 写成功后 *ppos 前进实际写入的字节数。
 *
 * 返回值：实际写入的字节数；出错返回负 errno。
 */
static ssize_t led8_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct led8_dev *priv = led8_from_file(file);
	u8 kbuf[LED8_MAX_CHANNELS];
	size_t n;
	u32 off;
	u32 i;

	if (!count)
		return 0;

	/* 写越界：返回 ENOSPC，比静默丢弃更容易排查 */
	if (*ppos < 0 || *ppos >= (loff_t)priv->channels)
		return -ENOSPC;

	off = (u32)*ppos;
	n = min_t(size_t, count, (size_t)(priv->channels - off));

	/* 先从用户空间取数据，成功后再动寄存器，避免写一半失败 */
	if (copy_from_user(kbuf, buf, n))
		return -EFAULT;

	mutex_lock(&priv->lock);
	for (i = 0; i < n; i++)
		priv->regs[off + i] = kbuf[i];
	/* 数据改完了，通知"硬件"（现在是空函数，以后是 I2C 下发） */
	led8_hw_commit(priv);
	mutex_unlock(&priv->lock);

	*ppos += n;
	return n;
}

/**
 * led8_llseek - .llseek 回调：改变文件偏移
 * @file:   文件对象
 * @offset: 偏移量
 * @whence: SEEK_SET（从开头）/ SEEK_CUR（从当前位置）/ SEEK_END（从结尾）
 *
 * 本设备的"文件"就是一块 channels 字节的寄存器文件，所以：
 *   - 合法范围是 [0, channels]；
 *   - 超出范围不报错，钳到边界（和块设备/普通文件部分行为一致，方便调试）；
 *   - 返回新的偏移值。
 *
 * 对比 gpio3a1：那里用 no_llseek，因为没有"位置"概念；
 * 这里有寄存器文件，偏移就是通道号，必须自己实现。
 */
static loff_t led8_llseek(struct file *file, loff_t offset, int whence)
{
	struct led8_dev *priv = led8_from_file(file);
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:	// 从开头
		newpos = offset;
		break;
	case SEEK_CUR:	// 从当前位置
		newpos = file->f_pos + offset;
		break;
	case SEEK_END:	// 从结尾  offset可以为负数
		newpos = (loff_t)priv->channels + offset;
		break;
	default:
		return -EINVAL;
	}

	if (newpos < 0)
		return -EINVAL;

	if (newpos > (loff_t)priv->channels)
		newpos = priv->channels;

	file->f_pos = newpos;
	return newpos;
}

/**
 * led8_ioctl - .unlocked_ioctl 回调：带参数的批量操作
 * @file: 文件对象
 * @cmd:  ioctl 命令号（来自 include/led8_uapi.h）
 * @arg:  用户传进来的参数，含义由 cmd 决定（整数或结构体指针）
 *
 * 为什么叫 unlocked：VFS 不再帮你拿大内核锁（BKL），
 * 并发保护要驱动自己做——本驱动用 priv->lock 保护 regs[]。
 *
 * 处理 5 个命令：
 *   GET_CHANNELS : 把通道数写回用户态 int
 *   GET_ALL      : 把整块 regs[] 拷给用户态 struct led8_state
 *   SET_ALL      : 用用户态的 struct led8_state 覆盖 regs[]
 *   OFF / FULL   : 全部通道置 0 / 255
 *
 * 返回值：0 成功；负 errno（-ENOTTY 表示不支持的命令）。
 */
static long led8_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct led8_dev *priv = led8_from_file(file);
	struct led8_state state;
	int channels;

	switch (cmd) {
	case LED8_IOC_GET_CHANNELS:
		channels = (int)priv->channels;
		/* put_user 是"内核值 -> 用户空间"的标量拷贝，失败返回 -EFAULT */
		// put_user / get_user：只传单个简单变
		if (put_user(channels, (int __user *)arg))
			return -EFAULT;
		return 0;

	case LED8_IOC_GET_ALL:
		/*
		 * 结构体里没用到的字节清零，避免把内核栈上的旧数据漏给用户态
		 * （内核信息泄露是常见的安全问题）。
		 */
		memset(&state, 0, sizeof(state));
		mutex_lock(&priv->lock);
		memcpy(state.ch, priv->regs, priv->channels);
		mutex_unlock(&priv->lock);

		if (copy_to_user((void __user *)arg, &state, sizeof(state)))
			return -EFAULT;
		return 0;

	case LED8_IOC_SET_ALL:
		if (copy_from_user(&state, (void __user *)arg, sizeof(state)))
			return -EFAULT;

		mutex_lock(&priv->lock);
		memcpy(priv->regs, state.ch, priv->channels);
		led8_hw_commit(priv);
		mutex_unlock(&priv->lock);
		return 0;

	case LED8_IOC_OFF:
		mutex_lock(&priv->lock);
		memset(priv->regs, 0, priv->channels);
		led8_hw_commit(priv);
		mutex_unlock(&priv->lock);
		return 0;

	case LED8_IOC_FULL:
		mutex_lock(&priv->lock);
		memset(priv->regs, 0xff, priv->channels);
		led8_hw_commit(priv);
		mutex_unlock(&priv->lock);
		return 0;

	default:
		/* 不认识命令时返回 ENOTTY，是字符设备驱动的惯例 */
		return -ENOTTY;
	}
}

/**
 * led8_fops - 文件操作函数集
 *
 * VFS 在 open/read/write/ioctl/close 时会按这里的函数指针回调驱动。
 * .owner = THIS_MODULE 很关键：只要有进程还开着 /dev/led8，
 * 内核就不会允许 rmmod 本模块（模块引用计数 +1）。
 */
static const struct file_operations led8_fops = {
	.owner		= THIS_MODULE,
	.open		= led8_open,
	.release	= led8_release,
	.read		= led8_read,
	.write		= led8_write,
	.llseek		= led8_llseek,
	.unlocked_ioctl	= led8_ioctl,
};

/* ==========================================================================
 * platform 驱动：设备树节点匹配 -> probe -> 注册字符设备
 * ========================================================================== */

/**
 * led8_probe - platform 设备与驱动匹配成功后调用
 * @pdev: platform 设备实例（来自设备树里的 led8 节点）
 *
 * 按顺序做四件事，任何一步失败都按相反顺序回滚：
 *   1. 申请设备号        alloc_chrdev_region()
 *   2. 初始化 cdev        cdev_init()
 *   3. 把 cdev 加进内核   cdev_add()
 *   4. 创建 /dev 节点     device_create()
 *
 * 返回值：0 成功；负 errno 失败。
 */
static int led8_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct led8_dev *priv;
	u32 channels = LED8_DEFAULT_CHANNELS;
	u32 brightness = 0;
	int ret;

	/* ---- 0. 分配私有上下文 ---- */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* ---- 1. 解析设备树属性 ---- */
	/*
	 * of_property_read_u32() 返回 0 表示属性存在且读取成功；
	 * 返回 -EINVAL 通常表示属性不存在（或类型不是 u32）。
	 * 属性缺失时我们用默认值，不报错。
	 */
	// np：struct device_node，设备树节点句柄；dev->of_node就是当前platform设备对应的dts节点
	// propname：字符串，dts 里属性的名字，这里就是channels
	// out_value：输出指针，读到的值会写到这个 u32 变量里
	if (!of_property_read_u32(dev->of_node, "channels", &channels)) {
		if (channels == 0 || channels > LED8_MAX_CHANNELS) {
			dev_err(dev, "invalid channels %u (1..%u)\n",
				channels, LED8_MAX_CHANNELS);
			ret = -EINVAL;
			goto err_free;
		}
	}
	priv->channels = channels;

	if (!of_property_read_u32(dev->of_node, "default-brightness",
				  &brightness)) {
		if (brightness > 255) {
			dev_err(dev, "invalid default-brightness %u (0..255)\n",
				brightness);
			ret = -EINVAL;
			goto err_free;
		}
		/* 上电初始亮度：所有通道先设成同一个值，默认是 0（全灭） */
		// void *memset(void *s, int c, size_t n);
		// s：priv->regs，目标内存起始地址，就是u8 regs[LED8_MAX_CHANNELS]数组首地址
		// c：brightness，要填充的字节值（虽然参数类型是 int，但只会取低 8bit）
		// n：priv->channels，要连续填充多少个字节
		// 从 regs 起始位置开始，连续把 priv->channels 个字节，全部写成 brightness 的低 8 位
		memset(priv->regs, brightness, priv->channels);
	}

	mutex_init(&priv->lock);

	/* ---- 2. 申请设备号 ---- */
	/*
	 * alloc_chrdev_region(&devt, first_minor, count, name)：
	 *   - 主设备号由内核动态分配（想看结果：cat /proc/devices | grep led8）；
	 *   - 次设备号从 0 开始，一次申请 1 个；
	 *   - name 会出现在 /proc/devices 里，方便排查。
	 * 如果像 misc 那样用固定主设备号，就要自己保证不和别人冲突，
	 * 还要先查 /proc/devices —— 动态分配是 out-of-tree 驱动的标准做法。
	 */
	 // 动态分配主设备号
	 // 第二个参数 baseminor=0只是：我们申请的这一串次设备号，从 0 开始
	 // alloc_chrdev_region(&devt, 5, 3, "led8");
	// 含义：内核自动选一个空闲 major，分配 minor=5、6、7 一共 3 个次设备号。
	ret = alloc_chrdev_region(&priv->devt, 0, 1, DEVICE_NAME);
	if (ret) {
		dev_err(dev, "alloc_chrdev_region failed: %d\n", ret);
		goto err_free;
	}

	/* ---- 3. 初始化并注册 cdev ---- */
	/*
	 * cdev_init() 只是把 fops 填进 cdev 结构体，还没有对外生效。
	 * cdev.owner 和 fops.owner 双重保险，防止模块在被打开时卸载。
	 */
	cdev_init(&priv->cdev, &led8_fops);
	priv->cdev.owner = THIS_MODULE;

	/*
	 * cdev_add(cdev, devt, count)：把 cdev 和设备号绑定，并加入内核的
	 * cdev 映射表。之后 VFS 才能按设备号找到 led8_fops。
	 * 注意：cdev_add 成功后，即使 /dev 节点还没建，内核也已经可以
	 * 通过设备号访问到这个设备了，所以失败回滚要 cdev_del。
	 */
	ret = cdev_add(&priv->cdev, priv->devt, 1);
	if (ret) {
		dev_err(dev, "cdev_add failed: %d\n", ret);
		goto err_unregister;
	}

	/* ---- 4. 创建设备节点 ---- */
	/*
	 * device_create(class, parent, devt, drvdata, fmt, ...)：
	 *   - 在 /sys/class/led8/ 下创建名为 "led8" 的设备；
	 *   - 向用户空间发 uevent，mdev/udev 收到后创建 /dev/led8；
	 *   - drvdata 会存进 struct device，本驱动用不到（用 platform drvdata 即可）。
	 * 返回的是 struct device 指针，失败时是 ERR_PTR。
	 */
	 // rvdata = priv：私有数据指针，非常常用。内核会把这个指针存到生成的struct device的driver_data成员
	priv->dev = device_create(led8_class, NULL, priv->devt, priv, DEVICE_NAME);
	if (IS_ERR(priv->dev)) {
		ret = PTR_ERR(priv->dev);
		dev_err(dev, "device_create failed: %d\n", ret);
		goto err_cdev_del;
	}

	/* 把 priv 挂到 platform 设备上，remove() 时用 platform_get_drvdata 取回 */
	// 为什么又要设置？
	// latform_set_drvdata：服务于 platform 驱动的 remove 回调，因为 remove 只有 pdev 入参；
	// device_create 的第四个 drvdata：服务于这个 class 设备的 sysfs 属性回调；
	// 它们是两个不同struct device上独立的私有数据字段，存放同一个 priv 指针，各司其职，不是两套独立系统。
	platform_set_drvdata(pdev, priv);

	/*
	 * 打印实际分配到的设备号。注意主设备号每次 insmod 都可能不同，
	 * 所以不要把它写死在脚本里，要看日志或用 /proc/devices 查。
	 */
	dev_info(dev, "ready: major=%u minor=%u, %u channels, /dev/%s\n",
		 MAJOR(priv->devt), MINOR(priv->devt), priv->channels,
		 DEVICE_NAME);

	return 0;

	/* ---- 失败回滚：严格按注册的反序拆 ---- */
err_cdev_del:
	cdev_del(&priv->cdev);
err_unregister:
	unregister_chrdev_region(priv->devt, 1);
err_free:
	kfree(priv);
	return ret;
}

/**
 * led8_remove - platform 设备移除 / 驱动卸载时调用
 * @pdev: platform 设备实例
 *
 * 逆序释放 probe 里申请的东西：
 *   device_destroy  -> 删 /dev 节点和 /sys 条目
 *   cdev_del        -> 从内核 cdev 表移除
 *   unregister_chrdev_region -> 归还设备号
 *   kfree           -> 释放私有上下文
 *
 * 注意：内核 6.1 里 platform_driver.remove 的返回值类型还是 int；
 * 更新的内核（6.11+）已经改成 void，移植时留意。
 */
static int led8_remove(struct platform_device *pdev)
{
	struct led8_dev *priv = platform_get_drvdata(pdev);

	device_destroy(led8_class, priv->devt);
	cdev_del(&priv->cdev);
	unregister_chrdev_region(priv->devt, 1);
	mutex_destroy(&priv->lock);
	kfree(priv);

	dev_info(&pdev->dev, "removed\n");
	return 0;
}

/*
 * 设备树匹配表：compatible = "work,led8" 与 dts/led8-overlay.dts 里的节点对应。
 * MODULE_DEVICE_TABLE(of, ...) 会把这张表导出到模块的 modalias 里，
 * 方便 udev/mdev 在设备出现时自动 modprobe 对应驱动。
 */
static const struct of_device_id led8_of_match[] = {
	{ .compatible = "work,led8" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, led8_of_match);

/*
 * platform_driver：platform 总线是一条"虚拟总线"，专门给那些不挂在
 * PCI/USB/I2C/SPI 等真实总线上的设备用（本设备就是纯虚拟的）。
 * 它把设备树节点和驱动代码关联起来。
 */
static struct platform_driver led8_driver = {
	.probe	= led8_probe,
	.remove	= led8_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= led8_of_match,
	},
};

/* ==========================================================================
 * 模块加载 / 卸载
 * ========================================================================== */

/**
 * led8_init - insmod 时执行
 *
 * 顺序很重要：
 *   1. 先 class_create：probe 里 device_create 需要这个 class；
 *   2. 再注册 platform 驱动：注册后内核立刻拿现有设备去匹配，
 *      匹配成功会同步调用 led8_probe。
 * 如果顺序反过来，probe 里 led8_class 还是 NULL，device_create 会崩。
 */
static int __init led8_init(void)
{
	int ret;

	/*
	 * class_create(owner, name)：6.1 内核还是两个参数。
	 * 新内核（6.5+）改成了 class_create(name)，移植时注意。
	 */
	 // lass_create()：创建class大类本身（/sys/class/led8/这个顶层目录）→ 这个不能在probe里
	 // probe可能多次执行：系统里如果有2个compatible="work,led8"的设备树节点，probe会跑两次
	led8_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(led8_class))
		return PTR_ERR(led8_class);

	ret = platform_driver_register(&led8_driver);
	if (ret) {
		class_destroy(led8_class);
		return ret;
	}

	pr_info("led8: module loaded\n");
	return 0;
}

/**
 * led8_exit - rmmod 时执行
 *
 * 先注销 platform 驱动：内核会先调用所有实例的 remove()，
 * 把 /dev 节点、cdev、设备号都释放掉；确认没设备了再销毁 class。
 */
static void __exit led8_exit(void)
{
	platform_driver_unregister(&led8_driver);
	class_destroy(led8_class);

	pr_info("led8: module unloaded\n");
}

/*
 * 这里没有用 module_platform_driver() 宏，而是显式写 module_init/exit，
 * 因为我们要在注册驱动前后额外做 class_create/class_destroy ——
 * 显式写法能把"初始化顺序"完整展示出来，学习阶段更直观。
 */
module_init(led8_init);
module_exit(led8_exit);

MODULE_AUTHOR("fxy");
MODULE_DESCRIPTION("Pure cdev + platform demo: virtual 8-channel LED dimmer (TaishanPi RK3566)");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
