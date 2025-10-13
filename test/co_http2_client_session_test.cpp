#include "http/http2_client_session.hpp"
#include "http/http2_session.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace co_wq::net::http;

namespace {

class TestServerSession : public Http2Session {
public:
    struct RequestInfo {
        std::string method;
        std::string path;
    };

    TestServerSession() : Http2Session(Mode::Server) { }

    const std::unordered_map<int32_t, RequestInfo>& received_requests() const { return requests_; }

protected:
    int on_begin_headers(const nghttp2_frame* frame) override
    {
        if (!frame)
            return 0;
        if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST)
            requests_[frame->hd.stream_id] = RequestInfo {};
        return 0;
    }

    int on_header(const nghttp2_frame* frame, std::string_view name, std::string_view value) override
    {
        if (!frame)
            return 0;
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
            return 0;
        auto it = requests_.find(frame->hd.stream_id);
        if (it == requests_.end())
            return 0;
        if (name == ":method")
            it->second.method.assign(value.begin(), value.end());
        else if (name == ":path")
            it->second.path.assign(value.begin(), value.end());
        return 0;
    }

    int on_frame_recv(const nghttp2_frame* frame) override
    {
        if (!frame)
            return 0;
        if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST
            && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
            respond_with_payload(frame->hd.stream_id);
        }
        return 0;
    }

    int on_stream_close(int32_t stream_id, uint32_t error_code) override
    {
        auto headers_it = response_headers_.find(stream_id);
        if (headers_it != response_headers_.end())
            response_headers_.erase(headers_it);
        auto provider_it = response_data_providers_.find(stream_id);
        if (provider_it != response_data_providers_.end())
            response_data_providers_.erase(provider_it);
        auto body_it = response_bodies_.find(stream_id);
        if (body_it != response_bodies_.end())
            response_bodies_.erase(body_it);
        auto request_it = requests_.find(stream_id);
        if (request_it != requests_.end())
            requests_.erase(request_it);
        (void)error_code;
        return 0;
    }

private:
    void respond_with_payload(int32_t stream_id)
    {
        auto request_it = requests_.find(stream_id);
        if (request_it == requests_.end())
            return;

        std::string body            = std::string("payload:") + request_it->second.path;
        response_bodies_[stream_id] = std::move(body);

        Http2HeaderBlock& headers = response_headers_[stream_id];
        headers.clear();
        headers.reserve(3);
        headers.add(":status", "200");
        headers.add("content-length", std::to_string(response_bodies_[stream_id].size()));
        headers.add("content-type", "text/plain");

        Http2StringDataProvider& provider = response_data_providers_[stream_id];
        provider.reset();
        provider.bind(&response_bodies_[stream_id]);

        auto  span         = headers.as_span();
        auto* provider_ptr = provider.provider();
        submit_response(stream_id, span, provider_ptr);
    }

    std::unordered_map<int32_t, RequestInfo>             requests_;
    std::unordered_map<int32_t, Http2HeaderBlock>        response_headers_;
    std::unordered_map<int32_t, Http2StringDataProvider> response_data_providers_;
    std::unordered_map<int32_t, std::string>             response_bodies_;
};

struct StreamCapture {
    std::vector<std::string> headers;
    std::string              body;
    bool                     headers_complete { false };
    bool                     closed { false };
    uint32_t                 close_code { 0 };
};

bool pump_sessions(Http2ClientSession& client, TestServerSession& server)
{
    bool progress = false;
    for (int loop = 0; loop < 8; ++loop) {
        if (!client.drain_send_queue(nullptr) || !server.drain_send_queue(nullptr))
            return false;

        bool moved_any      = false;
        auto pump_direction = [&](Http2Session& sender, Http2Session& receiver) -> bool {
            bool        moved = false;
            std::string error;
            while (true) {
                auto pending = sender.pending_send_data();
                if (pending.empty())
                    break;
                std::string_view chunk(reinterpret_cast<const char*>(pending.data()), pending.size());
                if (!receiver.feed(chunk, &error)) {
                    std::cerr << "feed failed: " << error << '\n';
                    return false;
                }
                sender.consume_send_data(pending.size());
                moved    = true;
                progress = true;
            }
            moved_any = moved_any || moved;
            return true;
        };

        if (!pump_direction(client, server))
            return false;
        if (!pump_direction(server, client))
            return false;

        if (!moved_any)
            break;
    }
    return progress || (!client.pending_send_data().empty() || !server.pending_send_data().empty());
}

bool run_multi_stream_exchange()
{
    Http2ClientSession client;
    TestServerSession  server;

    if (!client.init()) {
        std::cerr << "client init failed: " << client.last_error() << '\n';
        return false;
    }
    if (!server.init()) {
        std::cerr << "server init failed: " << server.last_error() << '\n';
        return false;
    }

    std::array<nghttp2_settings_entry, 1> settings { { { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 } } };
    if (client.submit_settings(settings) != 0) {
        std::cerr << "client submit settings failed: " << client.last_error() << '\n';
        return false;
    }
    if (server.submit_settings(settings) != 0) {
        std::cerr << "server submit settings failed: " << server.last_error() << '\n';
        return false;
    }

    if (!pump_sessions(client, server)) {
        std::cerr << "pump after settings failed\n";
        return false;
    }

    StreamCapture capture1;
    StreamCapture capture2;

    Http2ClientSession::StreamHandlers handlers1;
    handlers1.on_header = [&](std::string_view name, std::string_view value) {
        capture1.headers.emplace_back(std::string(name) + ":" + std::string(value));
    };
    handlers1.on_headers_complete = [&](bool) { capture1.headers_complete = true; };
    handlers1.on_data             = [&](std::string_view data) { capture1.body.append(data.data(), data.size()); };
    handlers1.on_close            = [&](uint32_t code) {
        capture1.closed     = true;
        capture1.close_code = code;
    };

    Http2ClientSession::StreamHandlers handlers2;
    handlers2.on_header = [&](std::string_view name, std::string_view value) {
        capture2.headers.emplace_back(std::string(name) + ":" + std::string(value));
    };
    handlers2.on_headers_complete = [&](bool) { capture2.headers_complete = true; };
    handlers2.on_data             = [&](std::string_view data) { capture2.body.append(data.data(), data.size()); };
    handlers2.on_close            = [&](uint32_t code) {
        capture2.closed     = true;
        capture2.close_code = code;
    };

    Http2ClientSession::Request req1;
    req1.method    = "GET";
    req1.scheme    = "https";
    req1.authority = "example.com";
    req1.path      = "/alpha";

    Http2ClientSession::Request req2;
    req2.method    = "GET";
    req2.scheme    = "https";
    req2.authority = "example.com";
    req2.path      = "/beta";

    int32_t stream1 = client.start_request(req1, handlers1);
    if (stream1 < 0) {
        std::cerr << "stream1 start failed: " << client.last_error() << '\n';
        return false;
    }
    int32_t stream2 = client.start_request(req2, handlers2);
    if (stream2 < 0) {
        std::cerr << "stream2 start failed: " << client.last_error() << '\n';
        return false;
    }

    for (int i = 0; i < 32; ++i) {
        if (!pump_sessions(client, server))
            break;
        if (client.message_complete(stream1) && client.message_complete(stream2))
            break;
    }

    if (!client.message_complete(stream1) || !client.message_complete(stream2)) {
        std::cerr << "streams did not complete\n";
        return false;
    }

    const HttpResponse* resp1 = client.response_for(stream1);
    const HttpResponse* resp2 = client.response_for(stream2);
    if (!resp1 || !resp2) {
        std::cerr << "missing responses\n";
        return false;
    }

    if (resp1->status_code != 200 || resp2->status_code != 200) {
        std::cerr << "unexpected status codes\n";
        return false;
    }

    if (resp1->body != "payload:/alpha" || resp2->body != "payload:/beta") {
        std::cerr << "unexpected response body\n";
        return false;
    }

    if (!capture1.headers_complete || !capture2.headers_complete) {
        std::cerr << "headers callback did not fire\n";
        return false;
    }

    if (!capture1.closed || !capture2.closed) {
        std::cerr << "stream close callback missing\n";
        return false;
    }

    if (capture1.close_code != NGHTTP2_NO_ERROR || capture2.close_code != NGHTTP2_NO_ERROR) {
        std::cerr << "unexpected close code\n";
        return false;
    }

    auto active = client.active_streams();
    if (active.size() < 2) {
        std::cerr << "expected at least two recorded streams\n";
        return false;
    }

    client.clear_stream(stream1);
    client.clear_stream(stream2);
    if (!client.active_streams().empty()) {
        std::cerr << "stream cleanup failed\n";
        return false;
    }

    return true;
}

} // namespace

int main()
{
    if (!run_multi_stream_exchange())
        return 1;
    std::cout << "Http2ClientSession multi-stream test passed" << std::endl;
    return 0;
}
