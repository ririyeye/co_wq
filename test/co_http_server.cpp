#include "co_syswork.hpp"
#include "co_test_sys_stats_logger.hpp"

#include "http/http_easy_server.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#endif

using namespace co_wq;
using net::http::HttpEasyServer;
using net::http::HttpRequest;
using net::http::HttpResponse;

namespace {

std::atomic<HttpEasyServer*> g_server_ptr { nullptr };

#if defined(_WIN32)
BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT) {
        if (auto* server = g_server_ptr.load(std::memory_order_acquire))
            server->request_stop();
        return TRUE;
    }
    return FALSE;
}
#else
void sigint_handler(int)
{
    if (auto* server = g_server_ptr.load(std::memory_order_acquire))
        server->request_stop();
}
#endif

} // namespace

int main(int argc, char* argv[])
{
    std::string host = "0.0.0.0";
    uint16_t    port = 8080;
    std::string cert_path;
    std::string key_path;
    bool        use_tls         = false;
    bool        enable_http2    = false;
    bool        verbose_logging = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--cert" && i + 1 < argc) {
            cert_path = argv[++i];
            use_tls   = true;
        } else if (arg == "--key" && i + 1 < argc) {
            key_path = argv[++i];
            use_tls  = true;
        } else if (arg == "--http2") {
            enable_http2 = true;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose_logging = true;
        }
    }

#if !defined(USING_SSL)
    if (use_tls) {
        CO_WQ_LOG_ERROR("[http] 当前构建未启用 TLS，无法使用 --cert/--key 参数");
        return 1;
    }
#endif

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, sigint_handler);
#endif

    co_wq::test::SysStatsLogger    stats_logger("http_server");
    auto&                          wq = get_sys_workqueue(0);
    net::http::HttpServerWorkqueue fdwq(wq);

    HttpEasyServer server(fdwq);
    auto&          router = server.router();

    auto handle_root = [](const HttpRequest& req) {
        HttpResponse resp;
        resp.set_status(200, "OK");
        resp.set_header("content-type", "text/plain; charset=utf-8");
        std::string path = req.target.empty() ? std::string("/") : req.target;
        resp.set_body("Hello from co_wq HTTP server!\nMethod: " + req.method + "\nPath: " + path + "\n");
        return resp;
    };

    router.get("/", handle_root);
    router.get("/index", handle_root);
    router.get("/index.html", handle_root);

    router.get("/health", [](const HttpRequest&) {
        HttpResponse resp;
        resp.set_status(200, "OK");
        resp.set_header("content-type", "text/plain; charset=utf-8");
        resp.set_body("OK\n");
        return resp;
    });

    router.post("/echo-json", [](const HttpRequest& req) {
        HttpResponse resp;
        try {
            nlohmann::json request_json = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
            nlohmann::json response_json {
                { "status",  "ok"         },
                { "method",  req.method   },
                { "path",    req.target   },
                { "request", request_json }
            };
            if (auto it = req.headers.find("content-type"); it != req.headers.end())
                response_json["request_content_type"] = it->second;
            resp.set_status(200, "OK");
            resp.set_header("content-type", "application/json; charset=utf-8");
            resp.set_body(response_json.dump());
        } catch (const nlohmann::json::exception& ex) {
            nlohmann::json error_json {
                { "status", "error"   },
                { "reason", ex.what() }
            };
            resp.set_status(400, "Bad Request");
            resp.set_header("content-type", "application/json; charset=utf-8");
            resp.set_body(error_json.dump());
        }
        return resp;
    });

    router.serve_static("/static", "test/static", "index.html");

    router.add_middleware([](const HttpRequest&, HttpResponse& resp) {
        if (!resp.has_header("content-type"))
            resp.set_header("content-type", "text/plain; charset=utf-8");
        resp.set_header("server", "co_wq-http");
    });

    HttpEasyServer::Config config;
    config.host            = host;
    config.port            = port;
    config.enable_http2    = enable_http2;
    config.verbose_logging = verbose_logging;

#if defined(USING_SSL)
    if (use_tls) {
        if (cert_path.empty() || key_path.empty()) {
            CO_WQ_LOG_ERROR("[http] 启用 TLS 时必须同时提供 --cert 与 --key");
            return 1;
        }
        config.tls = HttpEasyServer::TlsConfig { cert_path, key_path };
    }
#endif

    if (!server.start(config)) {
        CO_WQ_LOG_ERROR("[http] 无法启动 HTTP 服务器");
        return 1;
    }

    g_server_ptr.store(&server, std::memory_order_release);

    server.wait();

    g_server_ptr.store(nullptr, std::memory_order_release);
    return 0;
}
