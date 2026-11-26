// SPDX-License-Identifier: GPL-2.0
/*
 * at24c02_cli.c - AT24C02 驱动的用户态测试工具
 *
 * 这个工具演示了"字符设备驱动"的三种典型用法：
 *   1. 用 ioctl 拿设备元信息（容量、页大小、从机地址）
 *   2. 用 pread/pwrite 按"文件偏移 = EEPROM 地址"的语义读写
 *   3. 用普通 read/write 配合 lseek 做顺序读写
 *
 * 编译：由顶层 Makefile 用交叉编译器静态链接，直接拷到板子上就能跑。
 *
 * 用法：
 *   at24c02_cli info
 *   at24c02_cli read  <addr> <len>
 *   at24c02_cli write <addr> <hex> [<hex> ...]
 *   at24c02_cli dump  [addr] [len]
 *   at24c02_cli erase
 *   at24c02_cli bench
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/ioctl.h>

#include "at24c02_uapi.h"

/* 驱动的设备节点，由 probe 里的 device_create(DEVICE_NAME) 产生 */
#define DEV_PATH "/dev/at24c02"

/*
 * 把字符串解析成无符号长整数。
 * 支持 "0x50" 和 "80" 两种写法（strtoul 传 0 会自动识别进制）。
 * 解析失败或有多余字符就返回 -1。
 */
static int parse_ulong(const char *s, unsigned long *out)
{
	char *end = NULL;

	errno = 0;
	*out = strtoul(s, &end, 0);
	if (errno != 0 || end == s || *end != '\0')
		return -1;

	return 0;
}

/*
 * 把 "0x41" / "41" / "FF" 这种两字符十六进制串解析成一个字节。
 */
static int parse_byte(const char *s, unsigned char *out)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 16);
	if (errno != 0 || end == s || *end != '\0' || v > 0xFF)
		return -1;

	*out = (unsigned char)v;
	return 0;
}

/*
 * 打印一段内存的十六进制 + ASCII 视图，格式和 hexdump -C 类似：
 *   00000000  41 42 43 44 ...  |ABCD....|
 *
 * base 是这段数据在 EEPROM 里的起始地址，用来打印左侧的偏移列。
 */
static void hexdump(unsigned long base, const unsigned char *buf, size_t len)
{
	size_t i, j;

	for (i = 0; i < len; i += 16) {
		/* 左侧偏移 */
		printf("%08lx  ", base + i);

		/* 中间 16 个字节，不足 16 个用空格补齐 */
		for (j = 0; j < 16; j++) {
			if (i + j < len)
				printf("%02x ", buf[i + j]);
			else
				printf("   ");
			if (j == 7)
				printf(" ");
		}

		/* 右侧 ASCII，不可打印字符显示成 '.' */
		printf(" |");
		for (j = 0; j < 16 && i + j < len; j++) {
			unsigned char c = buf[i + j];

			putchar(isprint(c) ? c : '.');
		}
		printf("|\n");
	}
}

/*
 * 打开设备并取回元信息。
 * 后面所有命令都基于 info.size 做边界检查。
 */
static int open_dev(int *fdp, struct at24c02_info *info)
{
	int fd = open(DEV_PATH, O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", DEV_PATH, strerror(errno));
		fprintf(stderr, "提示：先 insmod at24c02.ko，并确认设备树/节点已生效\n");
		return -1;
	}

	if (ioctl(fd, AT24C02_IOC_GET_INFO, info) < 0) {
		fprintf(stderr, "ioctl(GET_INFO): %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	*fdp = fd;
	return 0;
}

/* info：打印驱动的元信息 */
static int cmd_info(void)
{
	struct at24c02_info info;
	int fd;

	if (open_dev(&fd, &info) < 0)
		return 1;

	printf("device      : %s\n", DEV_PATH);
	printf("i2c address : 0x%02x\n", info.addr);
	printf("size        : %u bytes\n", info.size);
	printf("page size   : %u bytes (%u pages)\n",
	       info.page_size,
	       info.page_size ? info.size / info.page_size : 0);
	printf("transfer    : %s\n", (info.flags & 1) ? "block (I2C_BLOCK)" :
						       "byte-by-byte");

	close(fd);
	return 0;
}

/* read <addr> <len>：用 pread 读一段并打印 */
static int cmd_read(unsigned long addr, unsigned long len)
{
	struct at24c02_info info;
	unsigned char *buf;
	ssize_t n;
	int fd;

	if (open_dev(&fd, &info) < 0)
		return 1;

	/* 边界检查放在用户态做，给更友好的提示；驱动里也会再检查一遍 */
	if (addr >= info.size) {
		fprintf(stderr, "addr 0x%lx 超出容量 %u\n", addr, info.size);
		close(fd);
		return 1;
	}
	if (addr + len > info.size)
		len = info.size - addr;

	buf = malloc(len);
	if (!buf) {
		fprintf(stderr, "malloc failed\n");
		close(fd);
		return 1;
	}

	/*
	 * pread = read + lseek，一次调用把"从哪个 EEPROM 地址读"说清楚，
	 * 不会改变文件偏移，适合随机访问。
	 */
	n = pread(fd, buf, len, (off_t)addr);
	if (n < 0) {
		fprintf(stderr, "pread: %s\n", strerror(errno));
		free(buf);
		close(fd);
		return 1;
	}

	printf("read %zd bytes from 0x%lx\n", n, addr);
	hexdump(addr, buf, (size_t)n);

	free(buf);
	close(fd);
	return 0;
}

/* write <addr> <hex> [<hex> ...]：用 pwrite 写一串字节 */
static int cmd_write(unsigned long addr, int argc, char **argv)
{
	struct at24c02_info info;
	unsigned char *buf;
	ssize_t n;
	int fd, i;

	if (argc <= 0) {
		fprintf(stderr, "write 需要至少一个十六进制字节，例如：write 0x10 41 42\n");
		return 1;
	}

	if (open_dev(&fd, &info) < 0)
		return 1;

	buf = malloc((size_t)argc);
	if (!buf) {
		fprintf(stderr, "malloc failed\n");
		close(fd);
		return 1;
	}

	for (i = 0; i < argc; i++) {
		if (parse_byte(argv[i], &buf[i]) < 0) {
			fprintf(stderr, "无效的十六进制字节: %s\n", argv[i]);
			free(buf);
			close(fd);
			return 1;
		}
	}

	if (addr >= info.size) {
		fprintf(stderr, "addr 0x%lx 超出容量 %u\n", addr, info.size);
		free(buf);
		close(fd);
		return 1;
	}

	if (addr + (unsigned long)argc > info.size) {
		fprintf(stderr, "写入会越过芯片末尾（0x%lx + %d > %u）\n",
			addr, argc, info.size);
		free(buf);
		close(fd);
		return 1;
	}

	/*
	 * pwrite 把数据交给驱动的 write()，驱动内部按页切分、
	 * 每页等 5ms 擦写时间，所以这里不需要自己管页边界。
	 */
	n = pwrite(fd, buf, (size_t)argc, (off_t)addr);
	if (n < 0) {
		fprintf(stderr, "pwrite: %s\n", strerror(errno));
		free(buf);
		close(fd);
		return 1;
	}

	printf("wrote %zd bytes to 0x%lx\n", n, addr);

	free(buf);
	close(fd);
	return 0;
}

/* dump [addr] [len]：读一段并按 16 字节一行打印 */
static int cmd_dump(unsigned long addr, unsigned long len)
{
	return cmd_read(addr, len) == 0 ? 0 : 1;
}

/* erase：整片擦成 0xFF */
static int cmd_erase(void)
{
	struct at24c02_info info;
	int fd;

	if (open_dev(&fd, &info) < 0)
		return 1;

	if (ioctl(fd, AT24C02_IOC_ERASE) < 0) {
		fprintf(stderr, "ioctl(ERASE): %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("erased %u bytes (0xFF)\n", info.size);

	close(fd);
	return 0;
}

/*
 * bench：整片写入 + 回读校验，并测耗时。
 *
 * 这是最实用的"驱动到底能不能用"的一键测试：
 * 写完立刻读回来比对，能同时验证写时序（tWR 够不够）、
 * 页切分（跨页写会不会错位）和读通路。
 */
static int cmd_bench(void)
{
	struct at24c02_info info;
	unsigned char *wbuf, *rbuf;
	struct timespec t0, t1;
	double ms;
	int fd, i, ret = 0;

	if (open_dev(&fd, &info) < 0)
		return 1;

	wbuf = malloc(info.size);
	rbuf = malloc(info.size);
	if (!wbuf || !rbuf) {
		fprintf(stderr, "malloc failed\n");
		free(wbuf);
		free(rbuf);
		close(fd);
		return 1;
	}

	/* 造一个每字节都不同的图案，任何错位都会被发现 */
	for (i = 0; i < (int)info.size; i++)
		wbuf[i] = (unsigned char)(i ^ 0x5A);

	clock_gettime(CLOCK_MONOTONIC, &t0);

	if (pwrite(fd, wbuf, info.size, 0) != (ssize_t)info.size) {
		fprintf(stderr, "bench 写入失败: %s\n", strerror(errno));
		ret = 1;
		goto out;
	}

	if (pread(fd, rbuf, info.size, 0) != (ssize_t)info.size) {
		fprintf(stderr, "bench 读取失败: %s\n", strerror(errno));
		ret = 1;
		goto out;
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
	     (t1.tv_nsec - t0.tv_nsec) / 1000000.0;

	if (memcmp(wbuf, rbuf, info.size) != 0) {
		fprintf(stderr, "bench 校验失败：写进去的数据读回来不一致\n");
		for (i = 0; i < (int)info.size; i++) {
			if (wbuf[i] != rbuf[i])
				fprintf(stderr,
					"  0x%02x: wrote 0x%02x, read 0x%02x\n",
					i, wbuf[i], rbuf[i]);
		}
		ret = 1;
		goto out;
	}

	printf("bench ok: %u bytes write+read in %.1f ms\n", info.size, ms);

out:
	free(wbuf);
	free(rbuf);
	close(fd);
	return ret;
}

static void usage(const char *prog)
{
	printf("用法: %s <command> [args]\n", prog);
	printf("\n");
	printf("命令:\n");
	printf("  info                     显示设备信息（容量/页大小/从机地址）\n");
	printf("  read  <addr> <len>       从 EEPROM 地址 addr 读 len 字节\n");
	printf("  write <addr> <hex> ...   从 addr 开始写若干个十六进制字节\n");
	printf("  dump  [addr] [len]       十六进制 dump（默认 0 0x100）\n");
	printf("  erase                    整片擦除（全部写成 0xFF）\n");
	printf("  bench                    整片写入+回读校验并计时\n");
	printf("\n");
	printf("示例:\n");
	printf("  %s info\n", prog);
	printf("  %s write 0x10 41 42 43 44\n", prog);
	printf("  %s read 0x10 8\n", prog);
	printf("  %s dump 0 64\n", prog);
}

int main(int argc, char **argv)
{
	const char *cmd;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	cmd = argv[1];

	if (strcmp(cmd, "info") == 0)
		return cmd_info();

	if (strcmp(cmd, "read") == 0) {
		unsigned long addr, len;

		if (argc != 4 || parse_ulong(argv[2], &addr) < 0 ||
		    parse_ulong(argv[3], &len) < 0) {
			fprintf(stderr, "用法: %s read <addr> <len>\n", argv[0]);
			return 1;
		}
		return cmd_read(addr, len);
	}

	if (strcmp(cmd, "write") == 0) {
		unsigned long addr;

		if (argc < 4 || parse_ulong(argv[2], &addr) < 0) {
			fprintf(stderr,
				"用法: %s write <addr> <hex> [<hex> ...]\n",
				argv[0]);
			return 1;
		}
		return cmd_write(addr, argc - 3, argv + 3);
	}

	if (strcmp(cmd, "dump") == 0) {
		unsigned long addr = 0;
		unsigned long len = 0x100;

		if (argc >= 3 && parse_ulong(argv[2], &addr) < 0) {
			fprintf(stderr, "无效地址: %s\n", argv[2]);
			return 1;
		}
		if (argc >= 4 && parse_ulong(argv[3], &len) < 0) {
			fprintf(stderr, "无效长度: %s\n", argv[3]);
			return 1;
		}
		return cmd_dump(addr, len);
	}

	if (strcmp(cmd, "erase") == 0)
		return cmd_erase();

	if (strcmp(cmd, "bench") == 0)
		return cmd_bench();

	if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 ||
	    strcmp(cmd, "--help") == 0) {
		usage(argv[0]);
		return 0;
	}

	fprintf(stderr, "未知命令: %s\n\n", cmd);
	usage(argv[0]);
	return 1;
}
