// SPDX-License-Identifier: GPL-2.0-only
/*
 * spi_flash.hpp - W25Q64 用户态 C++ 封装
 *
 * 设计目标：把"文件描述符 + ioctl + pread/pwrite"这些 C 接口收进一个
 * RAII 类里，让上层命令行代码只用关心业务逻辑：
 *
 *     w25q::Flash flash;                      // 打开 /dev/w25q64，析构自动 close
 *     auto info = flash.info();               // ioctl 一把取回全部信息
 *     auto data = flash.read(0, 256);         // ppos 语义的读
 *     flash.erase_sector(0x1000);             // 擦除
 *     flash.write(0x1000, data);              // 裸页编程（不擦除）
 *     flash.write_preserving(0x1000, data);   // 先擦除再写，并保留扇区里其它数据
 *
 * 为什么参数用 const std::vector<std::byte> & 而不是 std::span：
 * 交叉工具链是 gcc 10.3，std::span 属于 C++20，用 -std=c++17 编不过；
 * vector 的引用在这里足够用，而且调用方本来就是 vector。
 *
 * 为什么不是 const 方法：ioctl 本身就是"对设备发命令"，语义上不是纯查询，
 * 所以统一用非 const 方法，避免给调用者错误的错觉。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace w25q {

/* 与驱动的 struct w25q64_info 一一对应 */
struct Info {
	std::uint32_t jedec_id = 0;
	std::uint32_t size = 0;
	std::uint32_t sector_size = 0;
	std::uint32_t page_size = 0;
	std::uint32_t speed_hz = 0;
	std::uint32_t status = 0;
};

/* 状态寄存器 1 的位（和数据手册一致） */
inline constexpr std::uint8_t status_busy = 1u << 0;
inline constexpr std::uint8_t status_wel = 1u << 1;

/*
 * 统一异常类型：把 errno 带上，方便上层决定是提示重试还是直接退出。
 * 继承 std::runtime_error，所以 catch (const std::exception &) 也能兜住。
 */
class Error : public std::runtime_error {
public:
	Error(const std::string &what, int err)
		: std::runtime_error(what), errno_(err) {}

	int code() const noexcept { return errno_; }

private:
	int errno_;
};

class Flash {
public:
	/* 打开设备；失败抛 Error（例如驱动没加载、/dev/w25q64 不存在） */
	explicit Flash(const std::string &path = "/dev/w25q64");
	~Flash();

	/* 设备是独占资源，禁止拷贝和移动，避免 fd 被关两次 */
	Flash(const Flash &) = delete;
	Flash &operator=(const Flash &) = delete;
	Flash(Flash &&) = delete;
	Flash &operator=(Flash &&) = delete;

	int fd() const noexcept { return fd_; }

	/* ---- 查询类 ---- */
	Info info();
	std::uint8_t status();
	std::uint32_t capacity();
	std::string chip_name();	/* 由 JEDEC ID 猜型号，如 "W25Q64" */

	/* ---- 数据传输 ---- */
	std::vector<std::byte> read(std::uint32_t addr, std::size_t len);
	void write(std::uint32_t addr, const std::vector<std::byte> &data);

	/* 读-改-擦-写：只覆盖 data 对应的那段，扇区里其它字节保持不变 */
	void write_preserving(std::uint32_t addr, const std::vector<std::byte> &data);

	/* ---- 擦除 ---- */
	void erase_sector(std::uint32_t addr);	/* 地址会对齐到 4KB 边界 */
	void erase_chip();

private:
	int fd_ = -1;
};

} // namespace w25q
