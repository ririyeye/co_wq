// empty.cpp - force archive emission for co_wq on platforms requiring at least one object file
namespace co_wq {
// 一个空符号，避免静态库为空导致的安装/打包阶段出错
void __co_wq_archive_anchor__() { }
}
