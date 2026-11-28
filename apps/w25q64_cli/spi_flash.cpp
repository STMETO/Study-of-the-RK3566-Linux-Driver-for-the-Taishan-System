// SPDX-License-Identifier: GPL-2.0-only
/*
* spi_flash.cpp - w25q::Flash 的实现文件
*
* 本层是【用户态C++封装层】，跑在应用层，调用内核字符设备驱动提供的接口
* 这里只做三件事：
*   1. 把 C 风格的 fd / ioctl / pread / pwrite 包装成会抛异常的成员函数；
*   2. 处理"读短了""写了一半"这类系统调用返回部分长度的情况（循环读写直到完成）；
*   3. 把"扇区擦除 + 写入"组合成写时保留（read-modify-write，RMW）逻辑。
*
* 重要边界约定：
* 驱动本身不做隐式擦除（NOR flash 硬件特性：bit只能1→0，想要写入0之前必须擦成全0xFF），
* 所以"先擦再写"这层业务策略放在用户态实现，内核驱动保持极简语义：你让我写我就执行页编程。
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
/**
* @brief 辅助函数：拼接错误信息字符串，带上errno对应的文字描述
* @param what 自定义错误前缀（比如"打开xxx失败"）
* @return 拼接好的错误字符串，用于构造异常
*/
std::string sys_error(const std::string &what)
{
	// errno是全局线程变量，系统调用失败之后会自动赋值
	return what + ": " + std::strerror(errno);
}

} // namespace 匿名命名空间：里面的函数只在本cpp文件可见，对外不可见

/* ------------------------------------------------------------------------- */
/* 构造函数：打开Flash字符设备文件 */
/* ------------------------------------------------------------------------- */
Flash::Flash(const std::string &path)
{
	/*
	* O_RDWR：可读可写，Flash需要读写、擦除
	* O_CLOEXEC：close-on-exec标志
	* 作用：如果这个进程后续调用exec拉起新程序，内核会自动关闭这个fd，
	* 防止文件描述符泄露到子进程，是Linux安全最佳实践
	*/
	// ::open是强制指定去全局命名空间找
	fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
	// open返回-1代表打开失败（驱动没加载、/dev节点不存在、权限不足）
	if (fd_ < 0)
		throw Error(sys_error("打开 " + path + " 失败"), errno);
}

/* ------------------------------------------------------------------------- */
/* 析构函数：RAII自动关闭fd */
/* ------------------------------------------------------------------------- */
Flash::~Flash()
{
	// 只有fd有效时才close，避免close(-1)产生未定义行为
	if (fd_ >= 0)
		::close(fd_);
}

/* ------------------------------------------------------------------------- */
/* 查询接口组 */
/* ------------------------------------------------------------------------- */
/**
* @brief 获取Flash全部硬件信息（从内核驱动通过ioctl读取）
* @return 填充好的Info结构体
*/
Info Flash::info()
{
	// raw 和内核驱动的 struct w25q64_info 内存布局完全一致
	struct w25q64_info raw {};

	// 调用ioctl，下发GET_INFO命令，内核把硬件信息拷贝到raw缓冲区
	if (::ioctl(fd_, W25Q64_IOC_GET_INFO, &raw) < 0)
		throw Error(sys_error("ioctl(GET_INFO) 失败"), errno);

	// 把内核raw结构体，转成用户态C++的Info结构体，做一层隔离
	Info out;
	out.jedec_id = raw.jedec_id;
	out.size = raw.size;
	out.sector_size = raw.sector_size;
	out.page_size = raw.page_size;
	out.speed_hz = raw.speed_hz;
	out.status = raw.status;
	return out;
}

/**
* @brief 单独读取Flash状态寄存器1
* @return 8bit原始状态值，配合status_busy/status_wel掩码判断位
*/
std::uint8_t Flash::status()
{
	std::uint8_t st = 0;

	if (::ioctl(fd_, W25Q64_IOC_GET_STATUS, &st) < 0)
		throw Error(sys_error("ioctl(GET_STATUS) 失败"), errno);
	return st;
}

/**
* @brief 快捷接口：直接返回Flash总容量（字节）
*/
std::uint32_t Flash::capacity()
{
	return info().size;
}

/**
* @brief 根据容量自动推断芯片名字，例如 8MB → 64Mbit → "W25Q64"
* @note W25Q命名规则：W25Qxx，xx是Mbit数
*/
std::string Flash::chip_name()
{
	const Info inf = info();
	// 字节 → Mbit：(字节 / 1024 /1024) * 8
	const unsigned mbit = inf.size / 1024 / 1024 * 8;
	char buf[32];

	/* W25Q64 = 64Mbit = 8192KB，JEDEC 0xEF4017 */
	// snprintf把数字 mbit按照格式串 W25Q%u 拼成字符串，写到buf字符数组里
	std::snprintf(buf, sizeof(buf), "W25Q%u", mbit);
	return buf;
}

/* ------------------------------------------------------------------------- */
/* 读接口：pread 循环读，处理短读 */
/* ------------------------------------------------------------------------- */
/**
* @brief 从Flash指定地址读取数据
* @param addr Flash内部物理地址
* @param len 期望读取总字节数
* @return vector<std::byte> 读到的数据（长度可能小于len，读到Flash末尾会截断）
*
* pread特点：第四个参数是文件偏移，不会修改文件的当前读写位置，不需要lseek
* 这里我们把文件偏移直接映射成Flash的物理地址，这是驱动层的约定
*/
std::vector<std::byte> Flash::read(std::uint32_t addr, std::size_t len)
{
	std::vector<std::byte> out(len); // 预先分配足够的缓冲区
	std::size_t done = 0;            // 已经成功读完的字节计数

	while (done < len) {
		/*
		* pread(fd, buf, count, offset)
		* offset：Flash物理地址 addr+done，每次读的起始位置
		* 返回值 n：本次实际读到的字节（可能小于请求长度，叫短读）
		*/
		// out.data()：返回指向 vector 底层连续内存首地址的指针,等价于 &out[0]
		// 第3个参数count：本次希望读取的最大字节数,len - done：还剩下多少字节没读完，本次最多读这么多
		// addr + done：flash 芯片上的起始地址（对应本次读的位置）
		ssize_t n = ::pread(fd_, out.data() + done, len - done,
							static_cast<off_t>(addr + done));
		if (n < 0)
			throw Error(sys_error("读 flash 失败"), errno);
		if (n == 0) /* 读到Flash容量末尾，驱动返回EOF(0)，停止读取 */
			break;
		done += static_cast<std::size_t>(n);
	}

	// 裁剪缓冲区，实际读到多少就保留多少
	out.resize(done);
	return out;
}

/* ------------------------------------------------------------------------- */
/* 裸写接口：pwrite循环写，只做页编程，【不会自动擦除】 */
/* ------------------------------------------------------------------------- */
/**
* @brief 裸写Flash，直接下发页编程命令，不自动擦除
* @param addr Flash物理地址
* @param data 待写入数据
* 约束：目标区域必须已经擦除为0xFF，否则写失败/数据错乱
*/
void Flash::write(std::uint32_t addr, const std::vector<std::byte> &data)
{
	std::size_t done = 0; // 已经写完的字节

	while (done < data.size()) {
		ssize_t n = ::pwrite(fd_, data.data() + done, data.size() - done,
							static_cast<off_t>(addr + done));
		if (n < 0)
			throw Error(sys_error("写 flash 失败"), errno);
		if (n == 0)
			// 写返回0是异常情况，直接抛出IO错误
			throw Error("写 flash 返回 0 字节", EIO);
		done += static_cast<std::size_t>(n);
	}
}

/* ------------------------------------------------------------------------- */
/* RMW 写：write_preserving，写一段数据，保留扇区内其他原有数据（重点） */
/* ------------------------------------------------------------------------- */
/**
* @brief 读-改-擦-写，用户只需要写一小段，自动保护同一个扇区内别的数据
* @param addr 起始写入地址
* @param data 待写入数据
*
* 原理（NOR Flash硬件限制导致必须这样）：
* 擦除最小单位是4KB扇区，不能只擦几个字节；擦除后整个扇区全部变成0xFF
* 流程：
* 1. 找到本次写入覆盖到的所有扇区
* 2. 读出整个扇区原始数据
* 3. 在内存里，把用户新数据覆盖到对应偏移位置
* 4. 调用擦除接口，擦除整个扇区
* 5. 把修改后的4KB完整扇区写回Flash
*
* 缺点：会多读多写整个扇区，性能一般；适合配置类小数据，不适合高速大流量
*/
void Flash::write_preserving(std::uint32_t addr, const std::vector<std::byte> &data)
{
	if (data.empty())
		return; // 空数据直接返回，不做任何操作

	const Info inf = info();
	const std::uint32_t sec = inf.sector_size;
	if (sec == 0)
		throw Error("驱动的 sector_size 为 0", EINVAL);

	// 算出本次写入覆盖范围对应的【第一个扇区起始地址】，对齐到扇区边界
	// begin：本次写入覆盖到的第一个扇区的起始地址（对齐到扇区边界）
	// 举例：sec=4096，addr=5000
	// 5000 % 4096 = 904 → begin = 5000 - 904 = 4096，也就是这个扇区从 4096 开始
	const std::uint64_t begin = static_cast<std::uint64_t>(addr) - (addr % sec);
	// 算出本次写入的末尾地址
	const std::uint64_t end = static_cast<std::uint64_t>(addr) + data.size();

	/*
	* 循环遍历每一个被本次写入覆盖的扇区
	* s = 当前扇区起始地址，每次 += sec 跳到下一个扇区
	*/
	for (std::uint64_t s = begin; s < end; s += sec) {
		/*
		举个例子帮你看懂：
		addr=5000，用户数据长度 2000，end=7000
		sec=4096，当前 s=4096（扇区起始：4096~8191）
		std::max(s, addr) → max(4096,5000) = 5000 d0 = 5000 - 5000 = 0：在用户 data 数组里，从下标 0 开始拿数据
		std::min(s+sec, end) → min(8192,7000)=7000 d1 =7000 -5000=2000：用户 data 数组读到下标 2000 为止
		off = 5000 - 4096 = 904：在扇区缓冲区（4K）里面，偏移 904 的位置开始写入
		*/
		// d0,d1：用户 data 数组里【这一个扇区需要用到的一段区间】[d0, d1)
		// off：扇区 buffer 里面，把上面这段数据粘贴到哪个位置
		// d0：当前扇区内，用户数据的起始偏移（相对于data的开头）
		const std::size_t d0 = static_cast<std::size_t>(std::max<std::uint64_t>(s, addr) - addr);
		// d1：当前扇区内，用户数据的结束偏移（相对于data的开头）
		const std::size_t d1 = static_cast<std::size_t>(std::min<std::uint64_t>(s + sec, end) - addr);
		// off：在扇区buffer里，需要写入的偏移（相对于扇区起始地址s）
		const std::size_t off = static_cast<std::size_t>(std::max<std::uint64_t>(s, addr) - s);

		// 读出整个扇区原始数据
		std::vector<std::byte> buf = read(static_cast<std::uint32_t>(s), sec);
		// 如果读到Flash末尾，读到的数据不足一个扇区，不足部分填充0xFF（擦除后的默认值）
		if (buf.size() < sec)
			buf.resize(sec, std::byte{0xFF});

		// 内存中拷贝：把用户新数据，覆盖到扇区buffer对应的位置
		std::copy(data.begin() + d0, data.begin() + d1, buf.begin() + off);

		// 擦除这个完整扇区
		erase_sector(static_cast<std::uint32_t>(s));
		// 把修改好的完整扇区写回Flash
		write(static_cast<std::uint32_t>(s), buf);
	}
}

/* ------------------------------------------------------------------------- */
/* 擦除接口组（ioctl下发擦除命令给内核驱动） */
/* ------------------------------------------------------------------------- */
/**
* @brief 扇区擦除，擦除一个4K扇区
* @param addr 任意地址，内核驱动内部自动对齐到扇区起始边界
*/
void Flash::erase_sector(std::uint32_t addr)
{
	std::uint32_t a = addr; /* 驱动内部会对齐到扇区边界 */

	if (::ioctl(fd_, W25Q64_IOC_ERASE_SECTOR, &a) < 0)
		throw Error(sys_error("ioctl(ERASE_SECTOR) 失败"), errno);
}

/**
* @brief 整片擦除（Chip Erase），很慢，W25Q64要几十秒
*/
void Flash::erase_chip()
{
	if (::ioctl(fd_, W25Q64_IOC_ERASE_CHIP) < 0)
		throw Error(sys_error("ioctl(ERASE_CHIP) 失败"), errno);
}

} // namespace w25q
