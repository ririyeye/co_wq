// empty.cpp - force archive emission for co_wq on platforms requiring at least one object file
namespace co_wq {
// 一个空符号，避免静态库为空导致的安装/打包阶段出错
void __co_wq_archive_anchor__() { }
}

#if defined(_MSC_VER) && !defined(__clang__)
#include <cstdint>
extern "C" void wq_debug_check_func_addr(std::uintptr_t);
extern "C" void wq_debug_check_func_addr_default(std::uintptr_t) { }
#pragma comment(linker, "/alternatename:wq_debug_check_func_addr=wq_debug_check_func_addr_default")
#endif
