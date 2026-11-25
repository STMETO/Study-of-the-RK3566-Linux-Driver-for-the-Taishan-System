// SPDX-License-Identifier: GPL-2.0
/*
 * led8_cli.c - /dev/led8 的命令行测试工具
 *
 * ============================================================================
 * 这个程序的作用
 * ============================================================================
 * 驱动 /dev/led8 是虚拟的 8 路 LED 调光板：每个通道 1 字节亮度（0~255）。
 * 这个 CLI 把驱动的几种访问方式都走一遍，方便你在板子上验证：
 *
 *   pread/pwrite  -> 走驱动的 read/write + llseek（偏移 = 通道号）
 *   ioctl         -> 走驱动的 unlocked_ioctl（整片读写、全灭、全亮）
 *
 * 命令：
 *   led8_cli info              打印设备路径和通道数
 *   led8_cli get <ch>          读某个通道（pread）
 *   led8_cli set <ch> <0-255>  写某个通道（pwrite）
 *   led8_cli dump              读全部通道并画成柱状图
 *   led8_cli all <0-255>       所有通道设成同一亮度（ioctl SET_ALL）
 *   led8_cli off               全灭（ioctl OFF）
 *   led8_cli full              全亮（ioctl FULL）
 *   led8_cli demo              "流水灯"演示（连续 pwrite/ioctl，带间隔）
 *
 * 注意：设备是虚拟的，所以"灯"不会真的亮，但 /dev/led8 的读写、
 * 偏移、并发、ioctl 全都是真实的内核路径，验证框架足够了。
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* 驱动和 CLI 共用同一份 ioctl 定义，避免两边编号对不上 */
#include "led8_uapi.h"

/* 驱动注册的设备节点。默认就是这个路径，可用环境变量 DEV_PATH 覆盖 */
#define DEV_PATH	"/dev/led8"

/* 全局打开的设备 fd 和通道数（打开成功后查一次，后面命令都用它做范围检查） */
static int		dev_fd = -1;
static unsigned int	channels;

/**
 * usage - 打印帮助
 */
static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s <command> [args]\n"
		"\n"
		"commands:\n"
		"  info              show device path and channel count\n"
		"  get <ch>          read brightness of one channel (pread)\n"
		"  set <ch> <0-255>  set brightness of one channel (pwrite)\n"
		"  dump              read all channels and draw bars\n"
		"  all <0-255>       set every channel to the same value (ioctl)\n"
		"  off               all channels off (ioctl)\n"
		"  full              all channels full brightness (ioctl)\n"
		"  demo              running-light demo\n",
		prog);
}

/**
 * die - 打印错误并退出
 * @what: 出错的操作名，perror 会附上 errno 对应的文字
 */
static void die(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

/**
 * parse_u32 - 把字符串解析成 [min, max] 范围内的整数
 * @s:    输入字符串
 * @min:  允许的最小值
 * @max:  允许的最大值
 * @what: 用于报错的参数名
 *
 * 返回解析后的值；非法输入直接退出程序（CLI 工具，没必要容错到很细）。
 */
static unsigned int parse_u32(const char *s, unsigned int min,
			      unsigned int max, const char *what)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 0);	/* 0 进制：支持 10/0x 前缀 */
	if (errno || end == s || *end != '\0' || v < (long)min || v > (long)max) {
		fprintf(stderr, "invalid %s: '%s' (expect %u..%u)\n",
			what, s, min, max);
		exit(EXIT_FAILURE);
	}
	return (unsigned int)v;
}

/**
 * query_channels - 通过 ioctl 问驱动有多少个通道
 *
 * 这是 LED8_IOC_GET_CHANNELS 的最小用法：用户态传一个 int 的地址，
 * 驱动里用 put_user() 把值写回来。
 */
static unsigned int query_channels(void)
{
	int n = 0;

	if (ioctl(dev_fd, LED8_IOC_GET_CHANNELS, &n) < 0)
		die("ioctl(GET_CHANNELS)");
	if (n <= 0) {
		fprintf(stderr, "driver reported bad channel count %d\n", n);
		exit(EXIT_FAILURE);
	}
	return (unsigned int)n;
}

/**
 * check_ch - 检查通道号是否在范围内
 */
static void check_ch(unsigned int ch)
{
	if (ch >= channels) {
		fprintf(stderr, "channel %u out of range (0..%u)\n",
			ch, channels - 1);
		exit(EXIT_FAILURE);
	}
}

/**
 * bar - 把 0~255 的亮度画成 8 格柱状图
 * @v: 亮度值
 *
 * 纯 ASCII，方便在任何串口终端里看：
 *   0   -> [........]
 *   255 -> [########]
 */
static void bar(unsigned int v)
{
	int i;
	int filled = (int)(v / 32);	/* 0..7 */

	putchar('[');
	for (i = 0; i < 8; i++)
		putchar(i < filled ? '#' : '.');
	putchar(']');
}

/**
 * print_all - 打印所有通道的状态
 * @ch: 长度至少为 channels 的数组
 */
static void print_all(const unsigned char *ch)
{
	unsigned int i;

	for (i = 0; i < channels; i++) {
		printf("ch%-2u = %3u  ", i, ch[i]);
		bar(ch[i]);
		putchar('\n');
	}
}

/**
 * cmd_info - 打印设备和通道信息
 */
static void cmd_info(void)
{
	printf("device  : %s\n", DEV_PATH);
	printf("channels: %u\n", channels);
	printf("node    : /sys/class/led8/led8/\n");
}

/**
 * cmd_get - 读一个通道（走驱动的 read + llseek：pread 的 offset 就是通道号）
 */
static void cmd_get(unsigned int ch)
{
	unsigned char v = 0;
	ssize_t n;

	check_ch(ch);
	n = pread(dev_fd, &v, 1, (off_t)ch);
	if (n < 0)
		die("pread");
	if (n != 1) {
		fprintf(stderr, "pread returned %zd bytes, expect 1\n", n);
		exit(EXIT_FAILURE);
	}
	printf("ch%-2u = %3u  ", ch, v);
	bar(v);
	putchar('\n');
}

/**
 * cmd_set - 写一个通道（pwrite 的 offset 就是通道号）
 */
static void cmd_set(unsigned int ch, unsigned int val)
{
	unsigned char v = (unsigned char)val;
	ssize_t n;

	check_ch(ch);
	n = pwrite(dev_fd, &v, 1, (off_t)ch);
	if (n < 0)
		die("pwrite");
	if (n != 1) {
		fprintf(stderr, "pwrite returned %zd bytes, expect 1\n", n);
		exit(EXIT_FAILURE);
	}
	printf("ch%-2u <- %u\n", ch, val);
}

/**
 * cmd_dump - 一次性读全部通道（一次 pread 读完，验证驱动的多字节读）
 */
static void cmd_dump(void)
{
	unsigned char buf[LED8_MAX_CHANNELS] = { 0 };
	ssize_t n;

	if (channels > LED8_MAX_CHANNELS) {
		fprintf(stderr, "driver channel count %u exceeds CLI limit %u\n",
			channels, LED8_MAX_CHANNELS);
		exit(EXIT_FAILURE);
	}

	n = pread(dev_fd, buf, channels, 0);
	if (n < 0)
		die("pread");
	if ((unsigned int)n != channels) {
		fprintf(stderr, "short read: %zd/%u\n", n, channels);
		exit(EXIT_FAILURE);
	}

	print_all(buf);
}

/**
 * cmd_all - 所有通道设成同一亮度（走 ioctl LED8_IOC_SET_ALL）
 *
 * 演示 ioctl 传结构体：用户态组好 struct led8_state，
 * 驱动 copy_from_user 后一次性更新所有通道。
 */
static void cmd_all(unsigned int val)
{
	struct led8_state st;
	unsigned int i;

	memset(&st, 0, sizeof(st));
	for (i = 0; i < channels; i++)
		st.ch[i] = (unsigned char)val;

	if (ioctl(dev_fd, LED8_IOC_SET_ALL, &st) < 0)
		die("ioctl(SET_ALL)");

	printf("all channels <- %u\n", val);
	print_all(st.ch);
}

/**
 * cmd_off / cmd_full - 全灭 / 全亮（走 ioctl 的 OFF/FULL）
 */
static void cmd_off(void)
{
	if (ioctl(dev_fd, LED8_IOC_OFF) < 0)
		die("ioctl(OFF)");
	printf("all channels off\n");
}

static void cmd_full(void)
{
	if (ioctl(dev_fd, LED8_IOC_FULL) < 0)
		die("ioctl(FULL)");
	printf("all channels full\n");
}

/**
 * cmd_demo - 流水灯演示
 *
 * 每一帧让一个通道亮、其余灭，通过 ioctl SET_ALL 下发，中间 sleep 120ms。
 * 因为没有真实 LED，只是把每帧状态打印出来；
 * 等以后换成真实的 LED 驱动板，这个 demo 就能看到灯真的在跑。
 */
static void cmd_demo(void)
{
	struct led8_state st;
	unsigned int i, round;

	for (round = 0; round < 3; round++) {
		for (i = 0; i < channels; i++) {
			memset(&st, 0, sizeof(st));
			st.ch[i] = 0xff;

			if (ioctl(dev_fd, LED8_IOC_SET_ALL, &st) < 0)
				die("ioctl(SET_ALL)");

			printf("round %u frame %u: ", round, i);
			/* 一行打印所有通道的柱状图 */
			{
				unsigned int k;
				for (k = 0; k < channels; k++) {
					bar(st.ch[k]);
					putchar(' ');
				}
			}
			putchar('\n');
			usleep(120 * 1000);
		}
	}

	if (ioctl(dev_fd, LED8_IOC_OFF) < 0)
		die("ioctl(OFF)");
	printf("demo done, all channels off\n");
}

// pread / pwrite = read/write + lseek原子化版本，自带偏移参数，不会修改文件描述符内部的 f_pos（文件偏移指针）
// 普通 read / write 会顺着操作、自动更新 f_pos
int main(int argc, char **argv)
{
	const char *path = getenv("DEV_PATH") ? getenv("DEV_PATH") : DEV_PATH;

	if (argc < 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	/* O_RDWR：get/set/ioctl 都要用；驱动没有区分只读打开 */
	dev_fd = open(path, O_RDWR);
	if (dev_fd < 0)
		die("open " DEV_PATH);

	/* 先问通道数，后面所有命令都基于它做范围检查 */
	channels = query_channels();

	if (!strcmp(argv[1], "info")) {
		cmd_info();
	} else if (!strcmp(argv[1], "get")) {
		if (argc < 3) {
			usage(argv[0]);
			goto out_fail;
		}
		cmd_get(parse_u32(argv[2], 0, LED8_MAX_CHANNELS - 1, "channel"));
	} else if (!strcmp(argv[1], "set")) {
		if (argc < 4) {
			usage(argv[0]);
			goto out_fail;
		}
		cmd_set(parse_u32(argv[2], 0, LED8_MAX_CHANNELS - 1, "channel"),
			parse_u32(argv[3], 0, 255, "brightness"));
	} else if (!strcmp(argv[1], "dump")) {
		cmd_dump();
	} else if (!strcmp(argv[1], "all")) {
		if (argc < 3) {
			usage(argv[0]);
			goto out_fail;
		}
		cmd_all(parse_u32(argv[2], 0, 255, "brightness"));
	} else if (!strcmp(argv[1], "off")) {
		cmd_off();
	} else if (!strcmp(argv[1], "full")) {
		cmd_full();
	} else if (!strcmp(argv[1], "demo")) {
		cmd_demo();
	} else {
		usage(argv[0]);
		goto out_fail;
	}

	close(dev_fd);
	return EXIT_SUCCESS;

out_fail:
	close(dev_fd);
	return EXIT_FAILURE;
}
