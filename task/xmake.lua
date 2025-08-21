target("task")

    add_linkgroups("task", { whole = true, public = true })

    set_kind("static")

    add_includedirs(
        "./", {public=true}
    )

    add_files(
        "workqueue.cpp"
    )

target_end()

