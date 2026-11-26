// SPDX-License-Identifier: GPL-2.0-only
/*
 * spi_flash.cpp - w25q::Flash 的实现
 *
 * 这里只做三件事：
 *   1. 把 C 风格的 fd / ioctl / pread / pwrite 包装成会抛异常的成员函数；
 *   2. 处理"读短了""写了一半"这类系统调用返回部分长度的情况；
 *   3. 把"扇区擦除 + 写入"组合成写时保留（read-modify-write）。
 *
 * 驱动本身不做隐式擦除（NOR flash 只能 1->0，想写 0 之前必须擦成 0xFF），
 * 所以"先擦再写"这层策略放在用户态，驱动保持"你让我写我就编程"的语义。
 */
#include "spi_flash.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>

#include "w25q64_uapi.h"

namespace w25q {

namespace {

/* 把 errno 拼成人类可读的字符串 */
std::string sys_error(const std::string &what)
{
	return what + ": " + std::strerror(errno);
}

} // namespace

Flash::Flash(const std::string &path)
{
	/* O_CLOEXEC：万一程序后面 exec 别的程序，不要把设备 fd 漏出去 */
	fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
	if (fd_ < 0)
		throw Error(sys_error("打开 " + path + " 失败"), errno);
}

Flash::~Flash()
{
	if (fd_ >= 0)
		::close(fd_);
}

/* ------------------------------------------------------------------------- */
/* 查询                                                                      */
/* ------------------------------------------------------------------------- */

Info Flash::info()
{
	struct w25q64_info raw {};

	if (::ioctl(fd_, W25Q64_IOC_GET_INFO, &raw) < 0)
		throw Error(sys_error("ioctl(GET_INFO) 失败"), errno);

	Info out;
	out.jedec_id = raw.jedec_id;
	out.size = raw.size;
	out.sector_size = raw.sector_size;
	out.page_size = raw.page_size;
	out.speed_hz = raw.speed_hz;
	out.status = raw.status;
	return out;
}

std::uint8_t Flash::status()
{
	std::uint8_t st = 0;

	if (::ioctl(fd_, W25Q64_IOC_GET_STATUS, &st) < 0)
		throw Error(sys_error("ioctl(GET_STATUS) 失败"), errno);
	return st;
}

std::uint32_t Flash::capacity()
{
	return info().size;
}

std::string Flash::chip_name()
{
	const Info inf = info();
	const unsigned mbit = inf.size / 1024 / 1024 * 8;
	char buf[32];

	/* W25Q64 = 64Mbit = 8192KB，JEDEC 0xEF4017 */
	std::snprintf(buf, sizeof(buf), "W25Q%u", mbit);
	return buf;
}

/* ------------------------------------------------------------------------- */
/* 读                                                                        */
/* ------------------------------------------------------------------------- */

std::vector<std::byte> Flash::read(std::uint32_t addr, std::size_t len)
{
	std::vector<std::byte> out(len);
	std::size_t done = 0;

	while (done < len) {
		/*
		 * pread 的第四个参数就是驱动的 *ppos，也就是 EEPROM/flash 内部地址，
		 * 这正是我们把"地址"塞进 ppos 的意义：不需要额外的 seek。
		 */
		ssize_t n = ::pread(fd_, out.data() + done, len - done,
				    static_cast<off_t>(addr + done));
		if (n < 0)
			throw Error(sys_error("读 flash 失败"), errno);
		if (n == 0)	/* 读到容量末尾，驱动返回 0（EOF） */
			break;
		done += static_cast<std::size_t>(n);
	}

	out.resize(done);
	return out;
}

/* ------------------------------------------------------------------------- */
/* 写（裸编程，不含擦除）                                                     */
/* ------------------------------------------------------------------------- */

void Flash::write(std::uint32_t addr, const std::vector<std::byte> &data)
{
	std::size_t done = 0;

	while (done < data.size()) {
		ssize_t n = ::pwrite(fd_, data.data() + done, data.size() - done,
				     static_cast<off_t>(addr + done));
		if (n < 0)
			throw Error(sys_error("写 flash 失败"), errno);
		if (n == 0)
			throw Error("写 flash 返回 0 字节", EIO);
		done += static_cast<std::size_t>(n);
	}
}

/* ------------------------------------------------------------------------- */
/* 写（先擦后写，保留扇区里的其它数据）                                       */
/* ------------------------------------------------------------------------- */

void Flash::write_preserving(std::uint32_t addr, const std::vector<std::byte> &data)
{
	if (data.empty())
		return;

	const Info inf = info();
	const std::uint32_t sec = inf.sector_size;
	if (sec == 0)
		throw Error("驱动的 sector_size 为 0", EINVAL);

	const std::uint64_t begin = static_cast<std::uint64_t>(addr) - (addr % sec);
	const std::uint64_t end = static_cast<std::uint64_t>(addr) + data.size();

	/*
	 * 逐个扇区处理：
	 *   - 把整个扇区读出来（4KB）；
	 *   - 把用户要写的那一段覆盖进去；
	 *   - 擦除扇区；
	 *   - 把整扇区写回去。
	 *
	 * 这样做的好处是逻辑最简单、绝不会破坏相邻数据；代价是每次多读多写
	 * 一个扇区（4KB）。对 CLI 工具来说完全可以接受。
	 */
	for (std::uint64_t s = begin; s < end; s += sec) {
		const std::size_t d0 = static_cast<std::size_t>(std::max<std::uint64_t>(s, addr) - addr);
		const std::size_t d1 = static_cast<std::size_t>(std::min<std::uint64_t>(s + sec, end) - addr);
		const std::size_t off = static_cast<std::size_t>(std::max<std::uint64_t>(s, addr) - s);

		std::vector<std::byte> buf = read(static_cast<std::uint32_t>(s), sec);
		if (buf.size() < sec)	/* 芯片末尾不足一个扇区时补 0xFF */
			buf.resize(sec, std::byte{0xFF});

		std::copy(data.begin() + d0, data.begin() + d1, buf.begin() + off);

		erase_sector(static_cast<std::uint32_t>(s));
		write(static_cast<std::uint32_t>(s), buf);
	}
}

/* ------------------------------------------------------------------------- */
/* 擦除                                                                      */
/* ------------------------------------------------------------------------- */

void Flash::erase_sector(std::uint32_t addr)
{
	std::uint32_t a = addr;	/* 驱动内部会对齐到扇区边界 */

	if (::ioctl(fd_, W25Q64_IOC_ERASE_SECTOR, &a) < 0)
		throw Error(sys_error("ioctl(ERASE_SECTOR) 失败"), errno);
}

void Flash::erase_chip()
{
	if (::ioctl(fd_, W25Q64_IOC_ERASE_CHIP) < 0)
		throw Error(sys_error("ioctl(ERASE_CHIP) 失败"), errno);
}

} // namespace w25q
