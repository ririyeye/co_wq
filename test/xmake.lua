
target("co_echo")
    set_kind("binary")
    add_deps("co_wq")

    add_files("echo.cpp")
    add_files("syswork.cpp")

target_end()

target("co_cp")
    set_kind("binary")
    add_deps("co_wq")

    add_files("syswork.cpp")
    add_files("cp.cpp")
target_end()


