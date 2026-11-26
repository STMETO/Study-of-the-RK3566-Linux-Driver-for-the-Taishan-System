// SPDX-License-Identifier: GPL-2.0-only
/*
 * main.cpp - w25q64 的命令行工具（C++17）
 *
 * 为什么用 C++ 写：
 *   - 一块 flash 的数据天然适合用 std::vector<std::byte> 表示，不用手写 malloc/free；
 *   - 打开/关闭设备可以交给 RAII（w25q::Flash 的构造与析构），不会漏 close；
 *   - 出错统一抛异常，main 里一个 catch 就能统一打印，不用每步都判断返回值；
 *   - std::chrono 做吞吐计时、std::mt19937 做随机地址测试都很顺手。
 *
 * 命令一览见 usage()。
 */
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <random>
#include <string>
#include <vector>

#include "spi_flash.hpp"

namespace {

using bytes = std::vector<std::byte>;

constexpr const char *kDefaultDev = "/dev/w25q64";

/* ------------------------------------------------------------------------- */
/* 帮助与错误                                                                */
/* ------------------------------------------------------------------------- */

void usage(FILE *out)
{
	std::fprintf(out,
"w25q64_cli - W25Q64 SPI NOR Flash 测试工具\n"
"\n"
"用法:\n"
"  w25q64_cli [--dev PATH] <命令> [参数...]\n"
"\n"
"查询:\n"
"  info                            显示芯片信息（ID/容量/扇区/页/频率/状态）\n"
"  id                              只打印 JEDEC ID\n"
"  status                          打印状态寄存器 1（BUSY/WEL 等位）\n"
"\n"
"读:\n"
"  read  <addr> <len>              读出 len 字节写到 stdout（可重定向成文件）\n"
"  dump  <addr> <len>              hexdump 形式打印\n"
"\n"
"写:\n"
"  write <addr> <hex...>           例: write 0x10 41 42 43\n"
"  write <addr> --text \"hello\"    以文本写入\n"
"  write <addr> --file data.bin    从文件写入\n"
"  说明: 默认先擦除所在扇区再写（写时保留该扇区其它字节）；\n"
"        加 --no-erase 则只做页编程（目标区域必须已经是 0xFF）。\n"
"\n"
"擦除:\n"
"  erase <addr>                    擦除 addr 所在的扇区（4KB）\n"
"  erase --chip                    整片擦除（8MB 约需数秒到数十秒）\n"
"\n"
"测试:\n"
"  selftest [--start A] [--size N] 逐扇区擦-写-读-比对（默认 64KB）\n"
"  bench    [--start A] [--size N] 擦除/写/读吞吐测试（默认 256KB）\n"
"  stress   [--count N]            随机扇区擦写校验（默认 20 轮）\n"
"\n"
"全局选项:\n"
"  --dev PATH    设备节点，默认 %s\n"
"  --help        显示本帮助\n"
"\n"
"示例:\n"
"  w25q64_cli info\n"
"  w25q64_cli write 0x0 --text \"hello w25q64\"\n"
"  w25q64_cli dump 0x0 64\n"
"  w25q64_cli selftest --size 0x10000\n"
"  w25q64_cli read 0 1024 > beifen.bin\n"
	, kDefaultDev);
}

[[noreturn]] void die(const std::string &msg)
{
	std::fprintf(stderr, "错误: %s\n", msg.c_str());
	std::exit(1);
}

/* ------------------------------------------------------------------------- */
/* 参数解析小工具                                                            */
/* ------------------------------------------------------------------------- */

/* 支持十进制和 0x 十六进制（base = 0 让 strtoul 自己判断） */
std::uint32_t parse_u32(const char *s, const char *what)
{
	char *end = nullptr;
	errno = 0;
	const unsigned long v = std::strtoul(s, &end, 0);

	if (errno != 0 || end == s || *end != '\0')
		die(std::string("无法解析") + what + ": " + s);
	if (v > 0xFFFFFFFFul)
		die(std::string(what) + " 超出 32 位范围: " + s);
	return static_cast<std::uint32_t>(v);
}

std::byte parse_hex_byte(const char *s)
{
	char *end = nullptr;
	errno = 0;
	const unsigned long v = std::strtoul(s, &end, 16);

	if (errno != 0 || end == s || *end != '\0' || v > 0xFF)
		die(std::string("不是合法的十六进制字节: ") + s);
	return static_cast<std::byte>(v);
}

bytes read_whole_file(const std::string &path)
{
	std::FILE *fp = std::fopen(path.c_str(), "rb");
	if (!fp)
		die("打开文件失败: " + path);

	bytes out;
	std::byte buf[4096];
	std::size_t n;

	while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0)
		out.insert(out.end(), buf, buf + n);

	const bool bad = std::ferror(fp) != 0;
	std::fclose(fp);
	if (bad)
		die("读取文件失败: " + path);
	return out;
}

/* ------------------------------------------------------------------------- */
/* 输出工具                                                                  */
/* ------------------------------------------------------------------------- */

void hexdump(const bytes &d, std::uint32_t base)
{
	for (std::size_t off = 0; off < d.size(); off += 16) {
		std::printf("%08x  ", base + static_cast<std::uint32_t>(off));

		for (std::size_t j = 0; j < 16; j++) {
			if (off + j < d.size())
				std::printf("%02x ", static_cast<unsigned>(d[off + j]));
			else
				std::printf("   ");
			if (j == 7)
				std::printf(" ");
		}

		std::printf(" |");
		for (std::size_t j = 0; j < 16 && off + j < d.size(); j++) {
			const auto c = static_cast<unsigned char>(d[off + j]);
			std::printf("%c", (c >= 32 && c < 127) ? c : '.');
		}
		std::printf("|\n");
	}
}

double mib_per_sec(std::size_t bytes_n, double seconds)
{
	if (seconds <= 0.0)
		return 0.0;
	return static_cast<double>(bytes_n) / seconds / (1024.0 * 1024.0);
}

/* 测试图案：跟地址相关，这样地址算错、数据错位都能被发现 */
std::byte pattern_at(std::uint32_t addr)
{
	return static_cast<std::byte>(((addr * 31u) ^ (addr >> 8)) & 0xFFu);
}

bytes make_pattern(std::uint32_t start, std::size_t len)
{
	bytes out(len);
	for (std::size_t i = 0; i < len; i++)
		out[i] = pattern_at(start + static_cast<std::uint32_t>(i));
	return out;
}

std::string human_size(std::uint32_t bytes_n)
{
	char buf[64];

	if (bytes_n >= 1024 * 1024)
		std::snprintf(buf, sizeof(buf), "%u 字节 (%u KB, %u MB)", bytes_n,
			      bytes_n / 1024, bytes_n / 1024 / 1024);
	else
		std::snprintf(buf, sizeof(buf), "%u 字节 (%u KB)", bytes_n, bytes_n / 1024);
	return buf;
}

/* ------------------------------------------------------------------------- */
/* 各子命令                                                                  */
/* ------------------------------------------------------------------------- */

void cmd_info(w25q::Flash &flash)
{
	const w25q::Info inf = flash.info();
	const std::uint8_t st = flash.status();

	std::printf("JEDEC ID     : 0x%06X\n", inf.jedec_id);
	std::printf("芯片型号     : %s\n", flash.chip_name().c_str());
	std::printf("容量         : %s\n", human_size(inf.size).c_str());
	std::printf("扇区大小     : %u 字节\n", inf.sector_size);
	std::printf("页大小       : %u 字节\n", inf.page_size);
	std::printf("SPI 频率     : %u Hz\n", inf.speed_hz);
	std::printf("状态寄存器1  : 0x%02X%s%s\n", st,
		    (st & w25q::status_busy) ? " BUSY" : "",
		    (st & w25q::status_wel) ? " WEL" : "");
}

void cmd_id(w25q::Flash &flash)
{
	std::printf("0x%06X\n", flash.info().jedec_id);
}

void cmd_status(w25q::Flash &flash)
{
	const std::uint8_t st = flash.status();

	std::printf("状态寄存器1 = 0x%02X\n", st);
	std::printf("  BUSY = %u\n", (st & w25q::status_busy) ? 1u : 0u);
	std::printf("  WEL  = %u\n", (st & w25q::status_wel) ? 1u : 0u);
}

void cmd_read(w25q::Flash &flash, std::uint32_t addr, std::uint32_t len)
{
	const bytes data = flash.read(addr, len);

	/* 原始字节写 stdout，方便 `read 0 4096 > dump.bin` */
	if (!data.empty() &&
	    std::fwrite(data.data(), 1, data.size(), stdout) != data.size())
		die("写 stdout 失败");
}

void cmd_dump(w25q::Flash &flash, std::uint32_t addr, std::uint32_t len)
{
	const bytes data = flash.read(addr, len);

	hexdump(data, addr);
	std::printf("共 %zu 字节\n", data.size());
}

void cmd_write(w25q::Flash &flash, int argc, char **argv, int from)
{
	if (from >= argc)
		die("write 至少需要一个地址参数");

	const std::uint32_t addr = parse_u32(argv[from++], "地址");
	bool erase = true;
	bytes payload;

	while (from < argc) {
		const std::string a = argv[from];

		if (a == "--no-erase") {
			erase = false;
			from++;
		} else if (a == "--text") {
			if (from + 1 >= argc)
				die("--text 后面缺少字符串");
			const char *s = argv[from + 1];
			for (const char *p = s; *p; p++)
				payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(*p)));
			from += 2;
		} else if (a == "--file") {
			if (from + 1 >= argc)
				die("--file 后面缺少文件名");
			const bytes file_data = read_whole_file(argv[from + 1]);
			payload.insert(payload.end(), file_data.begin(), file_data.end());
			from += 2;
		} else if (!a.empty() && a[0] == '-') {
			die("不认识的选项: " + a);
		} else {
			payload.push_back(parse_hex_byte(argv[from]));
			from++;
		}
	}

	if (payload.empty())
		die("没有要写入的数据");

	if (erase)
		flash.write_preserving(addr, payload);
	else
		flash.write(addr, payload);

	std::printf("已写入 %zu 字节 @ 0x%X（%s）\n", payload.size(), addr,
		    erase ? "先擦后写" : "仅页编程");
}

void cmd_erase(w25q::Flash &flash, int argc, char **argv, int from)
{
	if (from >= argc)
		die("erase 需要地址或 --chip");

	if (std::strcmp(argv[from], "--chip") == 0) {
		std::printf("整片擦除中，请稍候...\n");
		const auto t0 = std::chrono::steady_clock::now();
		flash.erase_chip();
		const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::printf("整片擦除完成，用时 %.2f 秒\n", sec);
		return;
	}

	const auto inf = flash.info();
	const std::uint32_t addr = parse_u32(argv[from], "地址");
	const std::uint32_t aligned = addr - (addr % inf.sector_size);

	const auto t0 = std::chrono::steady_clock::now();
	flash.erase_sector(addr);
	const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("已擦除扇区 0x%X，用时 %.3f 秒\n", aligned, sec);
}

/* 逐扇区：擦 -> 写 -> 读 -> 比对 */
void cmd_selftest(w25q::Flash &flash, std::uint32_t start, std::uint32_t size)
{
	const auto inf = flash.info();

	if (start % inf.sector_size != 0)
		die("--start 必须按扇区对齐");
	size = (size + inf.sector_size - 1) / inf.sector_size * inf.sector_size;
	if (start + size > inf.size)
		die("测试范围超出芯片容量");

	std::printf("自检范围: 0x%X ~ 0x%X（%u 字节，%u 个扇区）\n",
		    start, start + size, size, size / inf.sector_size);

	const auto t0 = std::chrono::steady_clock::now();
	std::uint32_t sectors = 0;

	for (std::uint32_t s = start; s < start + size; s += inf.sector_size) {
		const bytes expect = make_pattern(s, inf.sector_size);

		flash.erase_sector(s);
		flash.write(s, expect);

		const bytes got = flash.read(s, inf.sector_size);
		if (got.size() != expect.size() || std::memcmp(got.data(), expect.data(), expect.size()) != 0)
			die("扇区 0x" + std::to_string(s) + " 数据校验失败");

		sectors++;
		if (sectors % 16 == 0)
			std::printf("  ...已校验 %u 个扇区\n", sectors);
	}

	const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("自检通过: %u 个扇区，用时 %.2f 秒（%.2f MiB/s）\n",
		    sectors, sec, mib_per_sec(size, sec));
}

void cmd_bench(w25q::Flash &flash, std::uint32_t start, std::uint32_t size)
{
	const auto inf = flash.info();

	if (start % inf.sector_size != 0)
		die("--start 必须按扇区对齐");
	size = (size + inf.sector_size - 1) / inf.sector_size * inf.sector_size;
	if (start + size > inf.size)
		die("测试范围超出芯片容量");

	std::printf("吞吐测试范围: 0x%X ~ 0x%X（%u 字节）\n", start, start + size, size);

	/* 1. 擦除 */
	auto t0 = std::chrono::steady_clock::now();
	for (std::uint32_t s = start; s < start + size; s += inf.sector_size)
		flash.erase_sector(s);
	double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("  擦除: %8.2f 秒（%.2f MiB/s）\n", sec, mib_per_sec(size, sec));

	const bytes payload = make_pattern(start, size);

	/* 2. 写（这里是裸页编程，数据区域刚擦过，全是 0xFF） */
	t0 = std::chrono::steady_clock::now();
	flash.write(start, payload);
	sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("  写入: %8.2f 秒（%.2f MiB/s）\n", sec, mib_per_sec(size, sec));

	/* 3. 读 */
	t0 = std::chrono::steady_clock::now();
	const bytes got = flash.read(start, size);
	sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("  读取: %8.2f 秒（%.2f MiB/s）\n", sec, mib_per_sec(size, sec));

	if (got.size() != payload.size() || std::memcmp(got.data(), payload.data(), payload.size()) != 0)
		die("吞吐测试回读校验失败");
	std::printf("  回读校验通过\n");
}

void cmd_stress(w25q::Flash &flash, int count)
{
	const auto inf = flash.info();
	const std::uint32_t sectors = inf.size / inf.sector_size;

	std::random_device rd;
	std::mt19937 rng(rd());

	std::printf("随机擦写测试: %d 轮，芯片共 %u 个扇区\n", count, sectors);

	for (int i = 0; i < count; i++) {
		const std::uint32_t sector = rng() % sectors;
		const std::uint32_t addr = sector * inf.sector_size + (rng() % (inf.sector_size - 256));

		bytes payload(256);
		for (auto &b : payload)
			b = static_cast<std::byte>(rng() & 0xFF);

		flash.write_preserving(addr, payload);

		const bytes got = flash.read(addr, payload.size());
		if (got.size() != payload.size() || std::memcmp(got.data(), payload.data(), payload.size()) != 0)
			die("第 " + std::to_string(i + 1) + " 轮校验失败 @ 0x" + std::to_string(addr));

		std::printf("  #%02d 扇区 0x%06X 地址 0x%06X 校验通过\n",
			    i + 1, sector, addr);
	}

	std::printf("随机擦写测试全部通过\n");
}

} // namespace

/* ------------------------------------------------------------------------- */
/* 入口                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	std::string dev = kDefaultDev;
	int i = 1;

	/* 先摘掉全局选项（必须放在子命令前面） */
	while (i < argc) {
		const std::string a = argv[i];

		if (a == "--help" || a == "-h") {
			usage(stdout);
			return 0;
		} else if (a == "--dev") {
			if (i + 1 >= argc)
				die("--dev 后面缺少路径");
			dev = argv[i + 1];
			i += 2;
		} else {
			break;
		}
	}

	if (i >= argc) {
		usage(stderr);
		return 1;
	}

	const std::string cmd = argv[i++];

	/*
	 * 先检查命令名再打开设备：否则打错命令时，报的却是
	 * "打开 /dev/w25q64 失败"，会让人误以为是驱动没加载。
	 */
	static const char *const known_cmds[] = {
		"info", "id", "status", "read", "dump",
		"write", "erase", "selftest", "bench", "stress",
	};
	bool cmd_ok = false;
	for (const char *k : known_cmds) {
		if (cmd == k) {
			cmd_ok = true;
			break;
		}
	}
	if (!cmd_ok) {
		std::fprintf(stderr, "不认识的命令: %s\n\n", cmd.c_str());
		usage(stderr);
		return 1;
	}

	/* 子命令参数里的公共选项先解析，剩下的交给各命令自己 */
	std::uint32_t opt_start = 0;
	std::uint32_t opt_size = 0;
	int opt_count = 20;
	std::vector<std::string> rest;
	bool have_start = false, have_size = false;

	for (; i < argc; i++) {
		const std::string a = argv[i];

		if (a == "--start" && i + 1 < argc) {
			opt_start = parse_u32(argv[++i], "--start");
			have_start = true;
		} else if (a == "--size" && i + 1 < argc) {
			opt_size = parse_u32(argv[++i], "--size");
			have_size = true;
		} else if (a == "--count" && i + 1 < argc) {
			opt_count = static_cast<int>(parse_u32(argv[++i], "--count"));
		} else {
			rest.push_back(argv[i]);
		}
	}

	try {
		w25q::Flash flash(dev);

		if (cmd == "info") {
			cmd_info(flash);
		} else if (cmd == "id") {
			cmd_id(flash);
		} else if (cmd == "status") {
			cmd_status(flash);
		} else if (cmd == "read") {
			if (rest.size() < 2)
				die("用法: read <addr> <len>");
			cmd_read(flash, parse_u32(rest[0].c_str(), "地址"),
				 parse_u32(rest[1].c_str(), "长度"));
		} else if (cmd == "dump") {
			if (rest.size() < 2)
				die("用法: dump <addr> <len>");
			cmd_dump(flash, parse_u32(rest[0].c_str(), "地址"),
				 parse_u32(rest[1].c_str(), "长度"));
		} else if (cmd == "write") {
			/* 把 rest 重新拼成 argv 形式交给 cmd_write */
			std::vector<char *> args;
			args.reserve(rest.size());
			for (auto &s : rest)
				args.push_back(const_cast<char *>(s.c_str()));
			cmd_write(flash, static_cast<int>(args.size()), args.data(), 0);
		} else if (cmd == "erase") {
			std::vector<char *> args;
			args.reserve(rest.size());
			for (auto &s : rest)
				args.push_back(const_cast<char *>(s.c_str()));
			cmd_erase(flash, static_cast<int>(args.size()), args.data(), 0);
		} else if (cmd == "selftest") {
			cmd_selftest(flash, have_start ? opt_start : 0,
				     have_size ? opt_size : 64 * 1024);
		} else if (cmd == "bench") {
			cmd_bench(flash, have_start ? opt_start : 0,
				  have_size ? opt_size : 256 * 1024);
		} else if (cmd == "stress") {
			cmd_stress(flash, opt_count > 0 ? opt_count : 1);
		}
	} catch (const w25q::Error &e) {
		std::fprintf(stderr, "错误: %s (errno=%d)\n", e.what(), e.code());
		return 1;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "错误: %s\n", e.what());
		return 1;
	}

	return 0;
}
