// Content update downloads in the emulator. The game's HTTPClient (HTTPQueue -> -[HTTPClientController
// download:to:withSha1hex:]) uses NSURLConnection, which is not emulated; this replacement fetches the file over plain
// HTTP on the calling thread, checks it against the expected SHA1 (as connectionDidFinishLoading does), writes it to
// the target path and leaves the controller's ivars as the original callbacks would: status 2 (done) or 3 (failed),
// statusCode, the byte counts. HTTPQueue::update polls the status.
#include "Hle.h"
#include "Loader.h"
#include "Log.h"
#include "Memory.h"
#include "foundation/Foundation.h"
#include "libc/Crypto.h"
#include "libc/Files.h"
#include "platform/Socket.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
// HTTP/1.0 GET; returns the status code (0 when unreachable) and the body.
int httpGet(const std::string &url, std::string &body) {
    if (url.rfind("http://", 0) != 0) return 0;
    auto rest = url.substr(7);
    const auto slash = rest.find('/');
    const auto hostPort = rest.substr(0, slash);
    const auto path = slash == std::string::npos ? std::string("/") : rest.substr(slash);
    const auto colon = hostPort.rfind(':');
    const auto host = hostPort.substr(0, colon);
    const auto port = colon == std::string::npos ? std::string("80") : hostPort.substr(colon + 1);
    platform::initSockets();
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || !found) return 0;
    const platform::socket_t s = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    platform::setReceiveTimeout(s, 15000);
    const bool connected = connect(s, found->ai_addr, int(found->ai_addrlen)) == 0;
    freeaddrinfo(found);
    if (!connected) { platform::closeSocket(s); return 0; }
    const auto request = "GET " + path + " HTTP/1.0\r\nHost: " + hostPort + "\r\nConnection: close\r\n\r\n";
    send(s, request.data(), int(request.size()), platform::kNoSignal);
    std::string response;
    char buffer[65536];
    for (int n; (n = int(recv(s, buffer, sizeof buffer, 0))) > 0;) response.append(buffer, std::size_t(n));
    platform::closeSocket(s);
    const auto headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos || response.rfind("HTTP/", 0) != 0) return 0;
    body = response.substr(headerEnd + 4);
    return std::atoi(response.c_str() + response.find(' ') + 1);
}
std::int64_t ivarOffset(const char *ivar) {
    const auto symbol = loader::symbol(std::string("_OBJC_IVAR_$_HTTPClientController.") + ivar);
    return symbol ? std::int32_t(mem::r32(symbol)) : -1;
}
void setIvar(std::uint64_t self, const char *ivar, std::uint32_t value) {
    const auto offset = ivarOffset(ivar);
    if (offset >= 0) mem::w32(self + std::uint64_t(offset), value);
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
} // namespace

// -[HTTPClientController download:(NSURLRequest *)request to:(NSString *)path withSha1hex:(NSString *)sha1]
static hle::PatchRegistrar p_download("-[HTTPClientController download:to:withSha1hex:]", [] {
    const auto self = hle::arg(0);
    const auto urlObject = hle::arg(2);
    // The URL arrives as an NSMutableURLRequest (an NSURL or NSString is accepted too).
    auto urlValue = urlObject && objc::isKindOf(urlObject, "NSURLRequest") ? objc::send(urlObject, "URL") : urlObject;
    const auto url = !urlValue ? std::string() : ns::isString(urlValue) ? ns::text(urlValue) : ns::text(objc::send(urlValue, "absoluteString"));
    const auto target = ns::text(hle::arg(3));
    const auto expected = hle::arg(4) ? lower(ns::text(hle::arg(4))) : std::string();
    setIvar(self, "requestType", 0); // a download
    std::string body;
    const int code = httpGet(url, body);
    setIvar(self, "statusCode", std::uint32_t(code));
    setIvar(self, "statsReceivedBytes", std::uint32_t(body.size()));
    setIvar(self, "statsExpectedBytes", std::uint32_t(body.size()));
    bool ok = code == 200;
    if (ok && !expected.empty() && crypto::sha1Hex(body.data(), body.size()) != expected) {
        hlog("http", "%s: SHA1 mismatch (expected %s)", url.c_str(), expected.c_str());
        ok = false;
    }
    if (ok) {
        const auto host = std::filesystem::u8path(files::hostPath(target));
        std::error_code ec;
        std::filesystem::create_directories(host.parent_path(), ec);
        std::ofstream out(host, std::ios::binary | std::ios::trunc);
        out.write(body.data(), std::streamsize(body.size()));
        ok = bool(out);
    }
    setIvar(self, "status", ok ? 2 : 3);
    hlog("http", "download %s -> %s: %s (HTTP %d, %zu bytes)", url.c_str(), target.c_str(), ok ? "done" : "failed", code, body.size());
});
