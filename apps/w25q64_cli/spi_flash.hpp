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
// 头文件保护：防止这个头文件被同一个编译单元重复 include，避免重复定义报错

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace w25q {
// 命名空间 w25q，隔离所有和W25Q Flash相关的类型、接口，避免和全局其他名字冲突

/*
* 【和内核驱动里的 struct w25q64_info 一一对应】
* 用户态通过 ioctl(W25Q64_IOC_GET_INFO) 从内核驱动拿到原始信息，
* 驱动的结构体原样拷贝到这个Info，再封装给上层C++调用
*/
struct Info {
	std::uint32_t jedec_id = 0;      // JEDEC 3字节ID，用来识别Flash型号（0xEF4017 对应W25Q64）
	std::uint32_t size = 0;          // Flash总容量，单位字节
	std::uint32_t sector_size = 0;   // 扇区大小，W25Q固定 4096(4KB)，擦除最小单位
	std::uint32_t page_size = 0;     // 页大小，W25Q固定 256字节，编程（写）最小单位
	std::uint32_t speed_hz = 0;      // SPI通信时钟频率，Hz
	std::uint32_t status = 0;        // 状态寄存器原始值
};

/*
* 状态寄存器1的bit定义，和W25Q数据手册保持一致
* inline constexpr：C++17，编译期常量，头文件里直接定义，不会多重定义链接错误
*/
// 接代表这一个 bit 掩码，后续你不用自己手写移位计算偏移了
// 怎么用？（最常用就是按位与 & 判断该位是否置 1）
inline constexpr std::uint8_t status_busy = 1u << 0;  // BIT0 BUSY位：1=芯片正在忙（擦除/编程中），0=空闲
inline constexpr std::uint8_t status_wel = 1u << 1;   // BIT1 WEL位：Write Enable Latch，1=写使能打开，可以擦除/编程

/*
* 自定义异常类 Error：专门用来封装Flash操作错误
* 继承 std::runtime_error，属于运行时异常
* 好处：
* 1. 携带原始errno，上层可以区分是 ENOENT / EIO / EBUSY 等不同错误
* 2. 既可以单独 catch(w25q::Error) 精细处理，也可以 catch(std::exception) 统一兜底
*/
class Error : public std::runtime_error {
public:
	// 构造函数：what是错误描述字符串，err是系统调用返回的errno
	Error(const std::string &what, int err)
		: std::runtime_error(what), errno_(err) {}

	// 获取错误码，noexcept 保证这个函数不会抛出异常
	int code() const noexcept { return errno_; }

private:
	int errno_; // 保存原始系统errno
};

/*
* Flash类：W25Q设备的RAII包装类
* 职责：
* 1. 管理 /dev/w25q64 的文件描述符 fd
* 2. 封装所有和内核字符设备驱动交互的接口：ioctl / pread / pwrite
* 3. 提供两种写模式：裸写、RMW读改写(write_preserving)
*/
class Flash {
public:
	/*
	* 构造函数：打开Flash字符设备
	* explicit：禁止隐式类型转换，比如 Flash f = "/dev/xxx"; 这种写法会报错
	* 带默认参数，默认打开 /dev/w25q64
	* 打开失败直接抛 w25q::Error 异常（驱动没加载、节点不存在、权限不足都会抛）
	*/
	explicit Flash(const std::string &path = "/dev/w25q64");

	/*
	* 析构函数：RAII核心
	* 对象生命周期结束自动 close(fd_)，不会忘记关闭设备文件
	*/
	~Flash();

	/*
	* ========== 拷贝、移动全部禁用 ==========
	* 设备fd是独占资源，一个fd不能被多个Flash对象共管
	* 如果允许拷贝：两个对象持有同一个fd，析构时会close两次，引发未定义行为
	* 移动也禁止：简化设计，本工具场景不需要转移fd所有权
	* ========================================
	*/
	Flash(const Flash &) = delete;
	Flash &operator=(const Flash &) = delete;
	Flash(Flash &&) = delete;
	Flash &operator=(Flash &&) = delete;

	// 对外暴露fd，只读，只允许读，不允许外部随便close
	int fd() const noexcept { return fd_; }

	/* ---------- 查询接口 ---------- */
	// 获取Flash完整信息结构体（JEDEC、容量、扇区页大小、SPI频率等）
	Info info();
	// 单独读取状态寄存器1，返回原始8bit值
	std::uint8_t status();
	// 快捷接口：返回总容量字节数
	std::uint32_t capacity();
	// 根据info里的容量自动生成芯片名字符串，例如 "W25Q64"
	std::string chip_name();

	/* ---------- 数据读写接口 ---------- */
	/**
	* 读Flash
	* @param addr  Flash物理起始地址
	* @param len   需要读取的字节长度
	* @return      读到的数据，放到vector<std::byte>返回
	* 内部会循环pread处理短读，直到读完或者读到Flash末尾EOF
	*/
	std::vector<std::byte> read(std::uint32_t addr, std::size_t len);

	/**
	* 裸页编程写接口（无自动擦除！重点）
	* @param addr  Flash物理起始地址
	* @param data  待写入数据
	* 约束：目标地址区域必须已经是0xFF（已经擦除过）
	* NOR Flash硬件限制：只能1->0，不能原地把0写成1
	* 内部循环pwrite，处理短写
	*/
	void write(std::uint32_t addr, const std::vector<std::byte> &data);

	/**
	* 【RMW 读-改-擦-写】
	* 写的时候保留扇区里其他无关数据，上层不用关心扇区边界
	* 逻辑：读整个扇区 → 内存里修改对应位置 → 擦扇区 → 把整扇区写回去
	* 注意：这个RMW逻辑在【用户态】实现，内核驱动不做
	*/
	void write_preserving(std::uint32_t addr, const std::vector<std::byte> &data);

	/* ---------- 擦除接口 ---------- */
	/**
	* 扇区擦除
	* @param addr 任意地址，内核驱动内部会自动对齐到4KB扇区起始地址
	* 擦除后这个扇区全部变成 0xFF
	*/
	void erase_sector(std::uint32_t addr);

	/**
	* 整片擦除（chip erase）
	* 速度很慢，W25Q64 8Mbit整片擦除几十秒
	*/
	void erase_chip();

private:
	int fd_ = -1; // 设备文件描述符，初始值-1代表未打开
};

} // namespace w25q
 
