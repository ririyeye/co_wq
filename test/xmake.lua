target("co_echo")
set_kind("binary")
add_deps("co_wq")

add_files("co_echo.cpp")
add_files("co_syswork.cpp")

target_end()

target("co_chat")
set_kind("binary")
add_deps("co_wq")
add_files("co_chat_server.cpp")
add_files("co_syswork.cpp")
add_packages("nlohmann_json")
target_end()

target("co_http")
set_kind("binary")
add_deps("co_wq")

add_files("co_http_server.cpp")
add_files("co_syswork.cpp")
add_packages("nlohmann_json")

target_end()

target("co_curl")
set_kind("binary")
add_deps("co_wq")

add_files("co_curl.cpp")
add_files("co_syswork.cpp")

target_end()

target("co_http2_client_session_test")
set_kind("binary")
add_deps("co_wq")
add_files("co_http2_client_session_test.cpp")
target_end()

target("co_http_headers_test")
set_kind("binary")
add_deps("co_wq")
add_files("co_http_headers_test.cpp")
target_end()

target("co_http_proxy")
set_kind("binary")
add_deps("co_wq")

add_files("co_http_proxy.cpp")
add_files("co_syswork.cpp")

target_end()

target("co_socks_proxy")
set_kind("binary")
add_deps("co_wq")

add_files("co_socks_proxy.cpp")
add_files("co_syswork.cpp")

target_end()

target("co_ws")
set_kind("binary")
add_deps("co_wq")

add_files("co_websocket_server.cpp")
add_files("co_syswork.cpp")

target_end()

target("co_cp")
set_kind("binary")
add_deps("co_wq")
add_files("co_syswork.cpp")
add_files("co_cp.cpp")
target_end()

if has_config("USING_USB") then
    target("co_usb")
    set_kind("binary")
    add_deps("co_wq")
    add_files("co_usb.cpp")
    add_files("co_syswork.cpp")
    target_end()
end

if is_plat("linux") then
    target("co_uds")
    set_kind("binary")
    add_deps("co_wq")
    add_files("co_uds.cpp")
    add_files("co_syswork.cpp")
    target_end()
end

if has_config("USING_SSL") then
    target("co_bili")
    set_kind("binary")
    add_deps("co_wq")
    add_rules("utils.bin2c", { linewidth = 16, extensions = { ".html" } })
    add_files("static/bili_index.html")
    add_files("co_bilibili_ranking.cpp")
    add_files("co_syswork.cpp")
    add_packages("nlohmann_json")
    target_end()
end
