/* SPDX-License-Identifier: GPL-2.0 */
/*
 * led8_uapi.h - /dev/led8 的用户态接口定义（驱动和 CLI 共用这一份）
 *
 * 为什么单独放一个头文件？
 *   内核驱动里 ioctl 的 cmd 编号、结构体布局，必须和用户态程序完全一致。
 *   如果两边各写一遍，改一个数字就可能对不上（而且很难查）。
 *   所以把「双方约定」抽出来，驱动和 CLI 都 include 它：
 *     - 驱动：drivers/led8/led8.c  ->  #include "led8_uapi.h"
 *     - 应用：apps/led8_cli/led8_cli.c -> #include "led8_uapi.h"（Makefile 加 -Iinclude）
 *
 * 注意：这个头文件要能同时被内核态和用户态编译，所以只能用
 * <linux/types.h>（__u8/__u32 这类定长类型）和 <linux/ioctl.h>，
 * 不能用内核私有的 struct、也不能用 C 库类型。
 */
#ifndef _LED8_UAPI_H
#define _LED8_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * 通道数上限。
 * 数组用固定长度，是为了让 ioctl 的 struct 在用户态/内核态布局一致
 * （变长数组没法直接放在 ioctl 结构体里）。
 */
#define LED8_MAX_CHANNELS	16

/*
 * struct led8_state - 一次性读写全部通道亮度的结构体
 * @ch: 每个元素是一个通道的亮度，取值 0~255
 *
 * 只有前「通道数」个字节有效，其余字节由驱动清零后返回。
 * 传结构体而不是数组指针，能顺便把长度也固定下来，避免越界。
 */
struct led8_state {
	__u8 ch[LED8_MAX_CHANNELS];
};

/*
 * ioctl 命令编号。
 * _IOR/_IOW 宏会把这些信息编码进 32 位 cmd：
 *   方向（读/写）、幻数（'L'，本设备专用，避免和其它驱动撞车）、序号、数据大小。
 * 内核的 unlocked_ioctl 里就是按这些宏生成的值来 switch 的。
 */
#define LED8_IOC_MAGIC		'L'

/* 读：返回通道数（int） */
#define LED8_IOC_GET_CHANNELS	_IOR(LED8_IOC_MAGIC, 0x01, int)
/* 读：返回所有通道亮度（struct led8_state） */
#define LED8_IOC_GET_ALL	_IOR(LED8_IOC_MAGIC, 0x02, struct led8_state)
/* 写：一次性设置所有通道亮度（struct led8_state） */
#define LED8_IOC_SET_ALL	_IOW(LED8_IOC_MAGIC, 0x03, struct led8_state)
/* 写：全部通道置 0（全灭），无参数 */
#define LED8_IOC_OFF		_IO(LED8_IOC_MAGIC, 0x04)
/* 写：全部通道置 255（全亮），无参数 */
#define LED8_IOC_FULL		_IO(LED8_IOC_MAGIC, 0x05)

#endif /* _LED8_UAPI_H */
