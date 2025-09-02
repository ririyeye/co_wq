set_project(co_wq)
add_rules("plugin.compile_commands.autoupdate")
add_rules("mode.release", "mode.debug", "mode.releasedbg", "mode.minsizerel")
set_languages("c++20")
set_warnings("all", "extra", "pedantic")

if is_plat("windows") then
    add_cxflags("/utf-8")
    add_cxflags("/DWIN32")
    add_cxflags("/D_WINDOWS")
    add_defines("HAVE_STRUCT_TIMESPEC")
    add_defines("WIN32_LEAN_AND_MEAN")
    add_defines("_WINSOCK_DEPRECATED_NO_WARNINGS")
end

option("USING_NET")
    set_default(true)
option_end()

option("USING_EXAMPLE")
    set_default(true)
option_end()


target("co_wq")
    set_kind("static")

    add_includedirs(
        "task", {public=true}
    )
    add_includedirs(
        "sync", {public=true}
    )

if get_config("USING_NET") then
    add_includedirs("net", {public = true})
    add_includedirs("io", {public = true})
    add_defines("USING_NET", {public = true})
end

target_end()

if get_config("USING_EXAMPLE") then
    includes("test")
end
