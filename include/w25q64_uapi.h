/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * w25q64_uapi.h - 驱动与用户态程序共用的 ioctl 定义
 *
 * 这个头文件会被两边同时包含：
 *   1. 内核驱动 drivers/w25q64/w25q64.c
 *   2. 用户态 CLI（apps/w25q64_cli 下的 .cpp 文件）
 * 所以里面只能用 __u32/__u8 这类两边都认识的内核固定宽度类型，
 * 不能出现 <linux/kernel.h> 里的东西。
 *
 * 数值约定（与 W25Q 系列数据手册一致）：
 *   - 页大小 256 字节：页编程不能跨页，跨页会回绕到本页开头
 *   - 扇区 4KB：擦除的最小单位
 *   - JEDEC ID 24 位：厂商 0xEF(Winbond) + 类型 0x40 + 容量码
 *     0x14=1MB(W25Q80) / 0x15=2MB / 0x16=4MB / 0x17=8MB(W25Q64) / 0x18=16MB
 */
#ifndef _W25Q64_UAPI_H
#define _W25Q64_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* ioctl 的"魔数"：任意选一个没被别的驱动占用的可打印字符即可。
 * 用 'W'（Winbond / Write）方便记忆，内核不会校验它的含义。 */
#define W25Q64_IOC_MAGIC 'W'

/*
 * 一次取回全部信息，省得用户态连续发 5 个 ioctl。
 * 结构体布局固定，驱动和 CLI 共用同一份定义，天然不会对不上。
 */
struct w25q64_info {
	__u32 jedec_id;		/* 24 位 JEDEC ID，例如 W25Q64 是 0xEF4017 */
	__u32 size;			/* 芯片总容量，单位字节 */
	__u32 sector_size;	/* 擦除扇区大小，固定 4096 */
	__u32 page_size;	/* 编程页大小，固定 256 */
	__u32 speed_hz;		/* 当前 SPI 时钟频率 */
	__u32 status;		/* 状态寄存器 1 的原始值（bit0=BUSY, bit1=WEL） */
};

/* 读：内核 -> 用户，第三个参数是用户态变量的类型（决定 _IOC_SIZE） */
#define W25Q64_IOC_GET_ID		_IOR(W25Q64_IOC_MAGIC, 0x01, __u32)
#define W25Q64_IOC_GET_SIZE		_IOR(W25Q64_IOC_MAGIC, 0x02, __u32)
#define W25Q64_IOC_GET_SECTOR_SIZE	_IOR(W25Q64_IOC_MAGIC, 0x03, __u32)
#define W25Q64_IOC_GET_PAGE_SIZE	_IOR(W25Q64_IOC_MAGIC, 0x04, __u32)
#define W25Q64_IOC_GET_STATUS		_IOR(W25Q64_IOC_MAGIC, 0x05, __u8)
#define W25Q64_IOC_GET_INFO		_IOR(W25Q64_IOC_MAGIC, 0x06, struct w25q64_info)

/* 写：用户 -> 内核，参数是 32 位 flash 地址（驱动内部会对齐到扇区边界） */
#define W25Q64_IOC_ERASE_SECTOR		_IOW(W25Q64_IOC_MAGIC, 0x07, __u32)

/* 无参数：整片擦除（耗时较长，8MB 芯片大约几十秒） */
#define W25Q64_IOC_ERASE_CHIP		_IO(W25Q64_IOC_MAGIC, 0x08)

#endif /* _W25Q64_UAPI_H */
