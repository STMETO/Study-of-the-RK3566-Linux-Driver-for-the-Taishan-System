/* SPDX-License-Identifier: GPL-2.0 */
/*
 * at24c02_uapi.h - AT24C02 驱动的用户态接口定义
 *
 * 这个头文件同时被两个地方包含：
 *   1. 内核驱动  drivers/at24c02/at24c02.c
 *   2. 用户态工具 apps/at24c02_cli/at24c02_cli.c
 *
 * 两边必须对 struct 的内存布局、ioctl 编号完全一致，否则 ioctl 会
 * 返回 -ENOTTY（编号对不上）或者拷贝出错误的数据（结构体大小对不上）。
 * 所以把它单独放在 include/ 下，让驱动和 CLI 共享同一份定义。
 *
 * 注意：用户态头文件里只能用 __u32/__u8 这种固定宽度类型，
 * 不能用 u32/u8——后者是内核内部类型，用户态没有。
 */
#ifndef _AT24C02_UAPI_H
#define _AT24C02_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * ioctl 的"幻数"（magic）。
 * 每个驱动挑一个字母，避免不同设备的 ioctl 编号撞车。
 * 'E' 在这里取 EEPROM 的首字母。
 */
#define AT24C02_IOC_MAGIC	'E'

/*
 * 设备信息结构体。
 *
 * 用 ioctl 一次拿回全部参数，比连续调 3 次 GET_xxx 少两次系统调用，
 * 也是驱动里最常见的"打包返回"写法。
 *
 * addr  : I2C 从机地址（AT24C02 默认 0x50）
 * size  : EEPROM 总容量（字节），AT24C02 是 256
 * page_size : 页大小，AT24C02 是 8。写操作不能跨页，驱动内部会按页切分
 * flags : bit0 = 1 表示适配器支持 I2C block 传输（走块读/块写），
 *         0 表示退化成逐字节读写
 */
struct at24c02_info {
	__u32 addr;
	__u32 size;
	__u32 page_size;
	__u32 flags;
};

/* 读：返回 EEPROM 容量（int） */
#define AT24C02_IOC_GET_SIZE		_IOR(AT24C02_IOC_MAGIC, 0x01, int)

/* 读：返回页大小（int） */
#define AT24C02_IOC_GET_PAGE_SIZE	_IOR(AT24C02_IOC_MAGIC, 0x02, int)

/* 读：返回 I2C 从机地址（int） */
#define AT24C02_IOC_GET_ADDR		_IOR(AT24C02_IOC_MAGIC, 0x03, int)

/* 读：一次返回 struct at24c02_info */
#define AT24C02_IOC_GET_INFO		_IOR(AT24C02_IOC_MAGIC, 0x04, \
					     struct at24c02_info)

/*
 * 写：整片擦除（全部写成 0xFF）。
 * 没有参数，所以用 _IO 而不是 _IOW。
 * 注意这是个"慢操作"：32 页 × 5ms ≈ 160ms，期间驱动持锁。
 */
#define AT24C02_IOC_ERASE		_IO(AT24C02_IOC_MAGIC, 0x05)

#endif /* _AT24C02_UAPI_H */
