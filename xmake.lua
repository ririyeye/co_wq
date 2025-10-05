-- 项目名
set_project(co_wq)
add_rules("plugin.compile_commands.autoupdate")
add_rules("mode.release", "mode.debug", "mode.releasedbg", "mode.minsizerel")
set_languages("c++20")
set_warnings("all", "extra", "pedantic")
set_license("LGPL-2.1")
-- Windows 平台编译选项
if is_plat("windows") then
    add_cxflags("/utf-8")
    add_cxflags("/DWIN32")
    add_cxflags("/D_WINDOWS")
    add_defines("HAVE_STRUCT_TIMESPEC")
    add_defines("WIN32_LEAN_AND_MEAN")
    add_defines("_WINSOCK_DEPRECATED_NO_WARNINGS")
end

option("USING_NET")
    set_default(false)
option_end()

option("USE_BUNDLED_LLHTTP")
    set_default(true)
option_end()

option("USING_SSL")
    set_default(false)
option_end()

option("USING_USB")
    set_default(false)
option_end()

-- 是否构建 examples（test 目录）
option("USING_EXAMPLE")
    set_default(false)
option_end()

if get_config("USING_NET") then
    if get_config("USE_BUNDLED_LLHTTP") then
        add_requires("llhttp")
    end
    if get_config("USING_SSL") then
        add_requires("openssl3")
    end
end

if get_config("USING_USB") then
    add_requires("libusb")
end

if get_config("USING_EXAMPLE") then
    add_requires("nlohmann_json")
end


-- 主静态库
target("co_wq")
    set_kind("static")
    -- 至少一个源文件以生成静态库产物（便于 xmake install）
    add_files("task/empty.cpp")

    add_includedirs(
        "task", {public=true}
    )
    add_includedirs(
        "sync", {public=true}
    )

    if get_config("USING_NET") then
        add_includedirs("io", {public = true})
        add_includedirs("net", {public = true})
        add_defines("USING_NET", {public = true})
        if is_plat("windows") then
            add_links("Ws2_32")
            add_includedirs("net/win", {public = true})
            -- add_defines("_ITERATOR_DEBUG_LEVEL=2")
            add_defines("_DISABLE_VECTOR_ALGORITHMS")
        else
            add_includedirs("net/linux", {public = true})
        end
        if get_config("USE_BUNDLED_LLHTTP") then
            add_packages("llhttp", {public = true})
        end
        if get_config("USING_SSL") then
            add_defines("USING_SSL", {public = true})
            add_packages("openssl3", {public = true})
        end
    end

    if get_config("USING_USB") then
        add_defines("USING_USB", {public = true})
        add_packages("libusb", {public = true})
    end

target_end()

-- 可选 example 程序
if get_config("USING_EXAMPLE") then
    includes("test")
end
