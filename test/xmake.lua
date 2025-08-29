
target("test")
    set_kind("binary")
    add_deps("co_wq")

    add_files("main.cpp")
    add_files("syswork.cpp")

target_end()


