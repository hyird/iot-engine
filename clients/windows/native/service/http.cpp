#include "service.h"
#include "../common/win32.h"
#include <winhttp.h>
#include <array>
#include <chrono>
#include <condition_variable>

namespace iotvpn::service {
namespace {
struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};
struct AsyncState {
    std::mutex mutex;
    std::condition_variable_any ready;
    DWORD wanted = 0, bytes = 0, error = 0;
    bool done = false, closed = false;
};
void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD event, LPVOID information, DWORD length) {
    if (!context) return;
    auto& state = *reinterpret_cast<AsyncState*>(context);
    std::lock_guard lock(state.mutex);
    if (event == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) state.closed = true;
    else if (event == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) state.error = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
    else if (event == state.wanted) {
        state.done = true;
        state.bytes = event == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE ? *static_cast<DWORD*>(information) : length;
    }
    state.ready.notify_all();
}
class AsyncRequest {
public:
    ~AsyncRequest() { close(); }
    void open(std::string_view method, std::string_view path, std::string_view token, bool stream) {
        session_.value = WinHttpOpen(L"iot-egine/0.7.3", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        if (!session_.value) win32Error("Cannot initialize HTTPS transport");
        if (!WinHttpSetTimeouts(session_.value, 10000, 15000, 15000, 45000)) win32Error("Cannot configure HTTPS timeouts");
        connection_.value = WinHttpConnect(session_.value, L"i.a-z.xin", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection_.value) win32Error("Cannot connect to platform");
        const auto verb = utf16(method), resource = utf16(path);
        request_ = WinHttpOpenRequest(connection_.value, verb.c_str(), resource.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request_) win32Error("Cannot create HTTPS request");
        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(request_, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable))) win32Error("Cannot disable HTTPS redirects");
        DWORD_PTR pointer = reinterpret_cast<DWORD_PTR>(&state_);
        if (!WinHttpSetOption(request_, WINHTTP_OPTION_CONTEXT_VALUE, &pointer, sizeof(pointer))) win32Error("Cannot set HTTPS callback context");
        if (WinHttpSetStatusCallback(request_, callback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
            win32Error("Cannot create asynchronous HTTPS callback");
        registered_ = true;
        headers_ = stream ? L"Accept: text/event-stream\r\n" : L"Accept: application/json\r\n";
        if (!token.empty()) {
            if (token.size() > 16384 || token.find_first_of("\r\n \t") != std::string_view::npos) throw std::runtime_error("Invalid authorization token");
            headers_ += L"Authorization: Bearer " + utf16(token) + L"\r\n";
        }
    }
    void send(const Json* body, std::stop_token stop) {
        struct Payload {
            std::string text;
            ~Payload() { if (!text.empty()) SecureZeroMemory(text.data(), text.size()); }
        } payload{body ? body->dump() : std::string{}};
        auto headers = headers_;
        if (body) headers += L"Content-Type: application/json\r\n";
        perform(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, [&] {
            return WinHttpSendRequest(request_, headers.c_str(), static_cast<DWORD>(headers.size()),
                body ? payload.text.data() : WINHTTP_NO_REQUEST_DATA, static_cast<DWORD>(payload.text.size()), static_cast<DWORD>(payload.text.size()), reinterpret_cast<DWORD_PTR>(&state_));
        }, stop);
        perform(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, [&] { return WinHttpReceiveResponse(request_, nullptr); }, stop);
        DWORD bytes = sizeof(status_);
        if (!WinHttpQueryHeaders(request_, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status_, &bytes, WINHTTP_NO_HEADER_INDEX))
            win32Error("Cannot read platform status");
    }
    DWORD status() const { return status_; }
    std::wstring contentType() const {
        DWORD bytes = 0;
        WinHttpQueryHeaders(request_, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
        if (!bytes || bytes > 4096) return {};
        std::wstring result(bytes / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(request_, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, result.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return {};
        result.resize(bytes / sizeof(wchar_t)); while (!result.empty() && result.back() == 0) result.pop_back(); return result;
    }
    DWORD read(std::span<char> buffer, std::stop_token stop) {
        // SSE responses stay open. Read only available bytes rather than waiting
        // for WinHTTP to fill an 8 KiB buffer with future snapshots/heartbeats.
        const auto available=perform(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, [&] { return WinHttpQueryDataAvailable(request_,nullptr); },stop);
        if (!available) return 0;
        const auto size=std::min(available,static_cast<DWORD>(buffer.size()));
        return perform(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, [&] { return WinHttpReadData(request_, buffer.data(), size, nullptr); }, stop);
    }
private:
    template<class Operation> DWORD perform(DWORD event, Operation operation, std::stop_token stop) {
        if (stop.stop_requested()) throw std::runtime_error("操作已取消。");
        {
            std::lock_guard lock(state_.mutex); state_.wanted = event; state_.done = false; state_.error = 0; state_.bytes = 0;
        }
        if (!operation() && GetLastError() != ERROR_IO_PENDING) win32Error("HTTPS operation failed");
        std::unique_lock lock(state_.mutex);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        const bool ready = state_.ready.wait_until(lock, stop, deadline, [&] { return state_.done || state_.error; });
        if (!ready || state_.error) {
            const auto error = state_.error;
            lock.unlock();
            // Drain pending I/O while the caller's send/read buffers are still alive.
            close();
            if (error) win32Error("Platform request failed", error);
            throw std::runtime_error(stop.stop_requested() ? "操作已取消。" : "平台响应超时，服务会自动重试。");
        }
        return state_.bytes;
    }
    void close() noexcept {
        if (!request_) return;
        // All operations run on this thread. Async WinHTTP calls have returned before cancellation here.
        // The callback context lives until HANDLE_CLOSING, including callbacks arriving after CloseHandle.
        WinHttpCloseHandle(request_); request_ = nullptr;
        if (registered_) {
            std::unique_lock lock(state_.mutex); state_.ready.wait(lock, [&] { return state_.closed; });
        }
    }
    AsyncState state_;
    InternetHandle session_, connection_;
    HINTERNET request_ = nullptr;
    bool registered_ = false;
    DWORD status_ = 0;
    std::wstring headers_;
};
int statusForCode(int code) {
    switch (code) {
        case 11004: case 11005: case 11006: return 401;
        case 11002: case 11007: case 11008: return 403;
        case 21004: case 10003: return 404;
        case 21003: return 410;
        case 10001: case 10002: case 21001: return 400;
        default: return 500;
    }
}
Json envelope(std::string_view text, int status, bool errorEvent = false) {
    Json document;
    try { document = parseJson(text); } catch (...) { }
    const bool valid = document.is_object() && document.contains("code") && document["code"].is_number_integer();
    const int code = valid ? document["code"].get<int>() : 10004;
    if (status < 200 || status >= 300 || code != 0 || errorEvent) {
        std::string message = "平台请求失败（" + std::to_string(status) + "）。";
        if (document.is_object() && document.contains("message") && document["message"].is_string()) message = document["message"].get<std::string>();
        throw ApiError(message, status >= 200 && status < 300 ? statusForCode(code) : status, code);
    }
    return document.value("data", Json());
}
std::string readBody(AsyncRequest& request, std::stop_token stop) {
    std::string body; std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = request.read(buffer, stop); if (!count) return body;
        if (body.size() + count > MaxMessageBytes) throw std::runtime_error("平台响应超过 1 MiB。");
        body.append(buffer.data(), count);
    }
}
Json requestJson(std::string_view method, std::string_view path, std::string_view token, const Json* body, std::stop_token stop) {
    AsyncRequest request; request.open(method, path, token, false); request.send(body, stop);
    return envelope(readBody(request, stop), static_cast<int>(request.status()));
}
void stream(std::string_view path, std::string_view token, const std::function<bool(const Json&)>& receive, std::stop_token stop) {
    AsyncRequest request; request.open("GET", path, token, true); request.send(nullptr, stop);
    if (request.status() < 200 || request.status() >= 300) { envelope(readBody(request, stop), static_cast<int>(request.status())); return; }
    auto contentType = request.contentType();
    std::transform(contentType.begin(), contentType.end(), contentType.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (!contentType.starts_with(L"text/event-stream")) throw std::runtime_error("平台没有返回 SSE 配置流。");
    SseParser parser; std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = request.read(buffer, stop); if (!count) return;
        if (!parser.feed({buffer.data(), count}, [&](std::string_view event, std::string_view data) {
            if (event == "heartbeat") return true;
            if (event != "message" && event != "snapshot" && event != "error") return true;
            return receive(envelope(data, 200, event == "error"));
        })) return;
    }
}
std::string peerPath(std::string_view id) { return "/v1/vpn/desktop/peers/" + canonicalId(id); }
}

bool SseParser::feed(std::string_view bytes, const std::function<bool(std::string_view, std::string_view)>& callback) {
    for (const char character : bytes) {
        if (character != '\n') {
            if (line_.size() == MaxMessageBytes) throw std::runtime_error("SSE 数据行超过 1 MiB。");
            line_ += character; continue;
        }
        if (!line_.empty() && line_.back() == '\r') line_.pop_back();
        if (line_.empty()) {
            if (!data_.empty()) { data_.pop_back(); if (!callback(event_, data_)) { data_.clear(); event_ = "message"; return false; } }
            data_.clear(); event_ = "message";
        } else if (line_.starts_with("event:")) {
            event_ = line_.substr(6); if (event_.starts_with(' ')) event_.erase(0, 1);
        } else if (line_.starts_with("data:")) {
            auto content = std::string_view(line_).substr(5); if (content.starts_with(' ')) content.remove_prefix(1);
            if (data_.size() + content.size() + 1 > MaxMessageBytes) throw std::runtime_error("SSE 消息超过 1 MiB。");
            data_.append(content); data_ += '\n';
        }
        line_.clear();
    }
    return true;
}
Json WinHttpTransport::login(std::string_view user, std::string_view password, std::stop_token stop) {
    const Json body{{"username", user}, {"password", password}}; return requestJson("POST", "/v1/auth/login", {}, &body, stop);
}
Json WinHttpTransport::refresh(std::string_view token, std::stop_token stop) {
    const Json body{{"refresh_token", token}}; return requestJson("POST", "/v1/auth/refresh", {}, &body, stop);
}
Json WinHttpTransport::devices(std::string_view token, std::stop_token stop) {
    Json result;
    stream("/v1/vpn/desktop/devices", token, [&](const Json& snapshot) { result = snapshot; return false; }, stop);
    if (!result.is_array()) throw std::runtime_error("平台没有返回有效设备列表。"); return result;
}
Json WinHttpTransport::apply(std::string_view token, std::string_view peer, const Json& body, std::stop_token stop) {
    return requestJson(peer.empty() ? "POST" : "PATCH", peer.empty() ? "/v1/vpn/desktop/peers" : peerPath(peer), token, &body, stop);
}
Json WinHttpTransport::config(std::string_view token, std::string_view peer, std::stop_token stop) {
    Json result;
    stream(peerPath(peer) + "/config", token, [&](const Json& snapshot) { result = snapshot; return false; }, stop);
    if (!result.is_object()) throw std::runtime_error("平台没有返回有效配置。"); return result;
}
void WinHttpTransport::remove(std::string_view token, std::string_view peer, std::stop_token stop) { requestJson("DELETE", peerPath(peer), token, nullptr, stop); }
void WinHttpTransport::watchConfig(std::string_view token, std::string_view peer, const std::function<void(const Json&)>& receive, std::stop_token stop) {
    stream(peerPath(peer) + "/config", token, [&](const Json& snapshot) { receive(snapshot); return !stop.stop_requested(); }, stop);
}
}
