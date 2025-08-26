set_project(co_wq)

target("co_wq")
    set_kind("static")

    add_includedirs(
        "task", {public=true}
    )
    add_includedirs(
        "sync", {public=true}
    )

    add_files(
        "task/workqueue.cpp"
    )
target_end()


add_rules("plugin.compile_commands.autoupdate")
set_languages("c++20")
set_warnings("all", "extra", "pedantic")
