
target("co_echo")
    set_kind("binary")
    add_deps("co_wq")

    add_files("echo.cpp")
    add_files("syswork.cpp")

target_end()

target("co_http")
    set_kind("binary")
    add_deps("co_wq")

    add_files("http_server.cpp")
    add_files("syswork.cpp")

target_end()

target("co_ws")
    set_kind("binary")
    add_deps("co_wq")

    add_files("websocket_server.cpp")
    add_files("syswork.cpp")

target_end()

target("co_cp")
    set_kind("binary")
    add_deps("co_wq")
    add_files("syswork.cpp")
    add_files("cp.cpp")
target_end()

if has_config("USING_USB") then
    target("co_usb")
        set_kind("binary")
        add_deps("co_wq")
        add_files("usb.cpp")
        add_files("syswork.cpp")
    target_end()
end

if is_plat("linux") then
    target("co_uds")
        set_kind("binary")
        add_deps("co_wq")
        add_files("uds.cpp")
        add_files("syswork.cpp")
    target_end()
end


