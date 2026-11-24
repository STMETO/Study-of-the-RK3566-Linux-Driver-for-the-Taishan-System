// SPDX-License-Identifier: GPL-2.0
/*
 * gpio3a1.c - GPIO3_A1 character device driver for TaishanPi RK3566
 *
 * Device tree node (see ../../dts/gpio3a1-overlay.dts):
 *
 *     gpio3a1: gpio3a1 {
 *         compatible = "work,gpio3a1";
 *         gpios = <&gpio3 RK_PA1 GPIO_ACTIVE_HIGH>;
 *     };
 *
 * Userspace interface /dev/gpio3a1:
 *   read  : returns "0\n" or "1\n"
 *   write : accepts "0", "1", "on", "off", "toggle" (optional trailing
 *           whitespace/newline is ignored)
 */

#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/uaccess.h>

// 驱动名称，用于platform_driver.driver.name
#define DRIVER_NAME "gpio3a1"
// /dev 下生成的设备文件名
#define DEVICE_NAME "gpio3a1"
// 接收用户态写入命令的内核缓冲区大小
#define CMD_BUF_SIZE    16

/**
 * struct gpio3a1_dev - 私有设备上下文结构体
 * @gpiod: GPIO 描述符，由 devm_gpiod_get 获取，代表GPIO3_A1
 * @miscdev: 杂项字符设备实例，绑定 fops，对应 /dev/gpio3a1
 */
 /* 
	为什么要单独包一个 `gpio3a1_dev`？
	1. 把属于同一个硬件实例的变量打包在一起
		这个驱动对应的硬件实例，现在就两样东西：
			gpiod：GPIO 资源句柄
			miscdev：杂项设备
	以后扩展的时候，你直接往里面加：中断、定时器、缓存、配置参数、状态 flag，不用到处散着放全局变量。
	如果不封装，你只能用全局变量存 gpiod、miscdev。
	全局变量最大坑：不支持多实例。
 */
struct gpio3a1_dev {
    struct gpio_desc *gpiod;
    struct miscdevice miscdev;
};

/**
 * gpio3a1_from_file - 从 file 指针反向取出私有设备结构体
 * @file: open 打开文件得到的 struct file
 *
 * file->private_data 存放 miscdev 地址；
 * container_of 通过 miscdev 成员地址反推出外层 gpio3a1_dev 首地址
 */
static struct gpio3a1_dev *gpio3a1_from_file(struct file *file)
{
	/*
		container_of(ptr, type, member)：已知结构体里面某个成员的地址，反推出整个外层结构体的起始地址
		1. ptr：已知的那个成员的指针。这里就是 file->private_data，它存的是miscdev的地址（&priv->miscdev）
		2. type：外层大结构体的类型，这里是 struct gpio3a1_dev
		3. member：这个成员在大结构体里的成员名字，miscdev
	*/
	/*
		为什么：这里就是 file->private_data，它存的是miscdev的地址而不是probe里面设置的priv？
		先把两条完全独立的 “存储链路” 分开，这是最容易混的地方：
			链路 A：platform_device(pdev) ↔ priv（给 probe /remove 用）
				一条是平台总线框架自己的私有数据槽，只属于 pdev，只给 probe、remove回调用。
			链路 B：struct file ↔ miscdev（给 open/read/write 这些文件操作回调用）
				open回调里 file->private_data = &priv->miscdev;
				read里面拿：通过file得到miscdev，再container_of找回priv
				file->private_data 是驱动开发者自己决定放啥，不是内核强制只能放 miscdev
	*/

    return container_of(file->private_data, struct gpio3a1_dev, miscdev);
}

/**
 * gpio3a1_read - /dev/gpio3a1 的 read 回调
 * @file: 文件句柄
 * @buf: 用户态接收缓冲区 __user 标记，代表用户空间地址
 * @count: 用户请求读取字节数
 * @ppos: 文件偏移指针（本驱动不使用偏移，no_llseek）
 *
 * 返回：成功返回读出字节数；失败返回负 errno
 * 功能：读取当前GPIO电平，返回 "0\n" 或 "1\n" 给用户态
 */
static ssize_t gpio3a1_read(struct file *file, char __user *buf,
                            size_t count, loff_t *ppos)
{
    struct gpio3a1_dev *priv = gpio3a1_from_file(file);
    char kbuf[2];
    int val;

    // cansleep 版本：GPIO可能挂在I2C扩展芯片等，允许睡眠，不能在原子上下文调用
	// gpiod_get_value() 普通版本，要求 GPIO 电平读取过程不能睡眠。
	// 带 cansleep 的版本，表示读取 GPIO 电平的过程中允许发生睡眠。
	// 典型场景是 GPIO 扩展芯片挂在 I2C 或 SPI 总线上。
	// 此时 GPIO 控制器并不直接通过 CPU 的 MMIO 寄存器访问。
	// 读取 GPIO 电平需要：
		// 1. CPU 发起 I2C 读操作。
		// 2. I2C 控制器执行总线传输。
		// 3. 等待 I2C 从设备返回数据。
		// 4. 获取对应 GPIO 引脚的电平。
		// I2C 传输可能涉及等待、中断、调度等过程，因此可能睡眠。
	// 为什么 GPIO descriptor 就能得到 GPIO 电平？
	// GPIO descriptor 是 GPIO 引脚在 Linux 内核中的软件抽象，内核通过它可以找到对应的 GPIO 控制器和引脚信息
    val = gpiod_get_value_cansleep(priv->gpiod);
    if (val < 0)
        return val;

    // 构造返回字符串：'0'/'1' + 换行
    kbuf[0] = val ? '1' : '0';
    kbuf[1] = '\n';

    // 内核辅助函数：把内核kbuf拷贝到用户buf，自动处理count、ppos
	// simple_read_from_buffer() 不仅封装了 copy_to_user()，还帮你处理了 读取长度、文件偏移量 ppos、重复读取和 EOF 等逻辑
    // 直接使用 copy_to_user() 有什么问题？
	// 没有处理 count : 用户可能只想读取 5 个字节
	// 没有处理 ppos : 用户可能需要再某个偏移出进行读取
	// 没有处理 EOF
	return simple_read_from_buffer(buf, count, ppos, kbuf, sizeof(kbuf));
}

/**
 * gpio3a1_write - /dev/gpio3a1 的 write 回调
 * @file: 文件句柄
 * @buf: 用户态写入缓冲区
 * @count: 用户写入长度
 * @ppos: 文件偏移
 *
 * 支持命令：
 *   "0" / "off"   -> GPIO输出低
 *   "1" / "on"    -> GPIO输出高
 *   "toggle"      -> 翻转当前电平
 * 自动忽略首尾空格、换行符（strim）
 * 返回：成功返回 count；非法命令/拷贝失败返回负 errno
 */
static ssize_t gpio3a1_write(struct file *file, const char __user *buf,
                             size_t count, loff_t *ppos)
{
    struct gpio3a1_dev *priv = gpio3a1_from_file(file);
    char kbuf[CMD_BUF_SIZE];
    // 最多拷贝缓冲区上限-1，预留末尾'\0'
    size_t len = min(count, sizeof(kbuf) - 1);

    // 从用户空间拷贝数据到内核缓冲区，失败返回-EFAULT
    if (copy_from_user(kbuf, buf, len))
        return -EFAULT;

    // 手动字符串结束符
    kbuf[len] = '\0';
    // strim：去掉字符串首尾空白字符（空格、\n、\r、tab）
    strim(kbuf);

	// 字符串相等 → 返回 0
    if (!strcmp(kbuf, "0") || !strcmp(kbuf, "off")) {
        gpiod_set_value_cansleep(priv->gpiod, 0);
    } else if (!strcmp(kbuf, "1") || !strcmp(kbuf, "on")) {
        gpiod_set_value_cansleep(priv->gpiod, 1);
    } else if (!strcmp(kbuf, "toggle")) {
        int val = gpiod_get_value_cansleep(priv->gpiod);
        if (val < 0)
            return val;
        // 取反电平输出
        gpiod_set_value_cansleep(priv->gpiod, !val);
    } else {
        // 不识别的命令，返回非法参数
        return -EINVAL;
    }

    // 按照字符设备常规做法：成功返回用户传入的count
    return count;
}

/**
 * gpio3a1_fops - 文件操作函数集
 * .owner = THIS_MODULE：防止驱动在文件打开时被rmmod卸载
 * .read / .write：读写回调
 * .llseek = no_llseek：不支持seek操作，偏移固定无效
 */
static const struct file_operations gpio3a1_fops = {
    .owner      = THIS_MODULE,
    .read       = gpio3a1_read,
    .write      = gpio3a1_write,
    .llseek     = no_llseek,
	// 没有 .open、没有 .release  
	// misc + VFS 走了一套默认逻辑，内核帮你做了 open release
	// 这个默认 misc_open 里面做一件关键事：file->private_data = &miscdev;
	// 就是这里！！自动把 miscdev 的地址塞到 file->private_data
};

/**
 * gpio3a1_probe - platform驱动匹配成功后的探测函数
 * @pdev: platform设备实例
 *
 * 流程：
 * 1. devm_kzalloc 分配私有结构（devm托管，失败自动释放）
 * 2. devm_gpiod_get 按DTS里gpios属性获取GPIO，初始方向输出低电平 GPIOD_OUT_LOW
 * 3. 设置GPIO消费者名字，方便在 /sys/kernel/debug/gpio 查看归属
 * 4. 初始化 miscdevice，动态分配minor号
 * 5. misc_register 注册杂项字符设备，生成 /dev/gpio3a1
 * 6. platform_set_drvdata：把私有指针挂到pdev上，remove时取回
 * pdev = struct platform_device，平台设备实例
 */
static int gpio3a1_probe(struct platform_device *pdev)
{
	// pdev（struct platform_device）内嵌了一个标准的struct device dev成员
	// struct device：Linux 内核设备模型的通用基类
    struct device *dev = &(pdev->dev);	
    struct gpio3a1_dev *priv;
    int ret;

    // devm 托管内存：设备detach时自动释放，不用手动kfree
	// devm_kzalloc(dev, ...) = 带设备托管的清零版内核内存分配
	// 分配一块内存给 priv；这块内存的生命周期绑定到dev这个设备。
	// 普通版本 priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	// 你必须在 remove 里面手动 kfree(priv); 否则泄漏
    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    // devm_gpiod_get：第二个参数为NULL，表示取DTS里第一条gpios属性
    // GPIOD_OUT_LOW：配置方向输出，初始电平低
	/*
		struct gpio_desc *devm_gpiod_get(struct device *dev, const char *con_id,
                                 enum gpiod_flags flags);
		con_id：GPIO 标签名，对应 DTS 里 gpios 配套的命名 gpio-names
		如果你写 con_id = "led"，DTS 要这样写：
			gpio-names = "led";
			gpios = <&gpio3 RK_PA1 GPIO_ACTIVE_HIGH>;
	*/
    priv->gpiod = devm_gpiod_get(dev, NULL, GPIOD_OUT_LOW);
    if (IS_ERR(priv->gpiod)) {	// IS_ERR用来判断指针是不是**错误指针**。
        ret = PTR_ERR(priv->gpiod);	// PTR_ERR(ptr)：取出错误码
        dev_err(dev, "failed to get GPIO3_A1: %d\n", ret);	// dev_err()内核打印函数，属于 dev_* 系列dev_info/dev_warn/dev_err
        return ret;
    }
    // 设置GPIO消费者名称，debug时区分是谁占用这个GPIO
	// 给这个 GPIO 设置一个消费者名字
	// 内核 /sys/kernel/debug/gpio 里面会显示这个名字，用来 debug：看这个 GPIO 被哪个驱动占用了。
    gpiod_set_consumer_name(priv->gpiod, "gpio3a1");

    // 杂项设备配置：动态次设备号、设备名、fops、父设备节点
	// 杂项设备共用一个固定主设备号 10，minor 是次设备号。
	// MISC_DYNAMIC_MINOR 意思：让内核自动分配一个空闲的次设备号，我们自己不指定
    priv->miscdev.minor = MISC_DYNAMIC_MINOR;
    priv->miscdev.name = DEVICE_NAME;
    priv->miscdev.fops = &gpio3a1_fops;
    priv->miscdev.parent = dev;	// 设置这个杂项设备的父设备是我们的 `struct device *dev`
	// 作用：
		// 在 sysfs 设备树里形成父子设备层级关系（/sys/devices/...）
		// 电源管理、设备生命周期、dmesg 日志归属会关联到这个父设备；
		// 属于规范写法，不写也大概率能跑，但不符合设备模型规范。

    // 注册杂项字符设备
    ret = misc_register(&priv->miscdev);
    if (ret) {
        dev_err(dev, "failed to register /dev/%s: %d\n",DEVICE_NAME, ret);
        return ret;
    }

    // 将私有上下文绑定到platform_device，remove函数可以通过platform_get_drvdata拿到
    platform_set_drvdata(pdev, priv);	// 把私有指针存到 pdev 里面
    dev_info(dev, "GPIO3_A1 ready: /dev/%s, current level %d\n",
         DEVICE_NAME, gpiod_get_value_cansleep(priv->gpiod));

    return 0;
}

/**
 * gpio3a1_remove - platform设备移除回调（卸载驱动/设备树overlay移除）
 * @pdev: platform设备
 *
 * misc_deregister：注销杂项设备，/dev/gpio3a1消失
 * 注意：gpiod、priv内存都是devm托管，不需要手动释放
 * pdev = struct platform_device，平台设备实例
 */
static int gpio3a1_remove(struct platform_device *pdev)
{	
	// platform_get_drvdata：从pdev里面把私有指针取出来
    struct gpio3a1_dev *priv = platform_get_drvdata(pdev);

    misc_deregister(&priv->miscdev);
	// 注意：gpiod、priv内存都是devm托管，不需要手动释放
    return 0;
}

/**
 * gpio3a1_of_match - 设备树匹配表
 * compatible = "work,gpio3a1" 和DTS节点compatible字符串匹配
 * MODULE_DEVICE_TABLE(of, xxx)：让udev/modalias知道该驱动匹配哪些DTS节点
 */
static const struct of_device_id gpio3a1_of_match[] = {
    { .compatible = "work,gpio3a1" },
    { }
};
// 导出匹配信息给 modalias，让系统（udev）在识别到这个 DTS 设备时，自动加载这个 ko 模块。
// of 表示 Open Firmware，也就是 Linux Device Tree 设备匹配体系
MODULE_DEVICE_TABLE(of, gpio3a1_of_match);	// 导出别名给 udev/modprobe，实现自动加载

/**
 * gpio3a1_driver - platform驱动结构体,platform总线是为了让没有实际物理总线的设备而创建的虚拟总线
 * probe：匹配成功执行；remove：设备摘除执行
 * driver.of_match_table：绑定DTS匹配表
 */
static struct platform_driver gpio3a1_driver = {
    .probe  = gpio3a1_probe,
    .remove = gpio3a1_remove,
    .driver = {
        .name       = DRIVER_NAME,
        .of_match_table = gpio3a1_of_match,
    },
};

// 平台驱动注册宏，自动封装 module_init / module_exit
module_platform_driver(gpio3a1_driver);

MODULE_AUTHOR("fxy");
MODULE_DESCRIPTION("GPIO3_A1 character device driver (TaishanPi RK3566)");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
