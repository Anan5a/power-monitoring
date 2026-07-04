// http_stub.cpp — see http_stub.h for the rationale.

#include "http_stub.h"
#include <algorithm>

// ── Global capture buffer ─────────────────────────────────────────────────
std::vector<CapturedCall>& http_capture() {
    static std::vector<CapturedCall> buf;
    return buf;
}

namespace {
int g_next_status = 200;
std::string g_next_body;
}  // namespace

void http_set_next_response(int status, const std::string& body) {
    g_next_status = status;
    g_next_body = body;
}

// ── WiFiClientSecure stub ────────────────────────────────────────────────
WiFiClientSecure::WiFiClientSecure() {}
void WiFiClientSecure::setInsecure() {}
void WiFiClientSecure::setHandshakeTimeout(int) {}
int  WiFiClientSecure::connect(const char*, uint16_t) { return 0; }
int  WiFiClientSecure::connected() { return 0; }  // never report "alive"
void WiFiClientSecure::stop() {}
WiFiClientSecure::operator bool() { return false; }

// ── FakeStreamBuf ────────────────────────────────────────────────────────
FakeStreamBuf::FakeStreamBuf() : pos_(0) {}
void FakeStreamBuf::set_body(const std::string& body) {
    body_ = body;
    pos_ = 0;
}
int FakeStreamBuf::available() {
    return pos_ < body_.size() ? 1 : 0;
}
int FakeStreamBuf::read() {
    if (pos_ >= body_.size()) return -1;
    return static_cast<unsigned char>(body_[pos_++]);
}

// ── HTTPClient ──────────────────────────────────────────────────────────
HTTPClient::HTTPClient()
    : status_code_(0), reuse_(false), ended_(false) {}

HTTPClient::~HTTPClient() {}

void HTTPClient::init(const char* url) {
    url_ = url ? url : "";
    headers_.clear();
    body_.clear();
    status_code_ = 0;
    ended_ = false;
    response_string_.clear();
    // Push a fresh captured call onto the global capture stack.
    CapturedCall c;
    c.url = url_;
    c.status_code = g_next_status;
    c.response_body = g_next_body;
    http_capture().push_back(std::move(c));
    // Stream body is set by the caller, but seed it here so that even a
    // POST that didn't get a response body can read the seed.
    stream_.set_body(g_next_body);
}

bool HTTPClient::begin(WiFiClientSecure&, const char* url) {
    init(url);
    return true;
}
bool HTTPClient::begin(const char* url) {
    init(url);
    return true;
}

void HTTPClient::setReuse(bool reuse) { reuse_ = reuse; }

void HTTPClient::addHeader(const char* name, const char* value) {
    headers_.push_back({name ? name : "", value ? value : ""});
    if (!http_capture().empty()) {
        http_capture().back().headers.push_back({name ? name : "", value ? value : ""});
    }
}

int HTTPClient::POST(const uint8_t* body, size_t len) {
    if (http_capture().empty()) init(url_.c_str());
    auto& c = http_capture().back();
    c.method = "POST";
    c.body.assign(body, body + len);
    body_.assign(body, body + len);
    status_code_ = c.status_code;
    stream_.set_body(c.response_body);
    return status_code_;
}

int HTTPClient::POST(const char* body, size_t len) {
    return POST(reinterpret_cast<const uint8_t*>(body), len);
}

int HTTPClient::PATCH(const uint8_t* body, size_t len) {
    if (http_capture().empty()) init(url_.c_str());
    auto& c = http_capture().back();
    c.method = "PATCH";
    c.body.assign(body, body + len);
    body_.assign(body, body + len);
    status_code_ = c.status_code;
    stream_.set_body(c.response_body);
    return status_code_;
}

int HTTPClient::sendRequest(const char* method, const uint8_t* body, size_t len) {
    if (http_capture().empty()) init(url_.c_str());
    auto& c = http_capture().back();
    c.method = method ? method : "";
    if (body && len) c.body.assign(body, body + len);
    body_ = c.body;
    status_code_ = c.status_code;
    stream_.set_body(c.response_body);
    return status_code_;
}

int HTTPClient::GET() {
    if (http_capture().empty()) init(url_.c_str());
    auto& c = http_capture().back();
    c.method = "GET";
    status_code_ = c.status_code;
    stream_.set_body(c.response_body);
    return status_code_;
}

void HTTPClient::end() {
    ended_ = true;
}

int HTTPClient::getStatusCode() { return status_code_; }

FakeStreamBuf& HTTPClient::getStream() { return stream_; }

const char* HTTPClient::getString() {
    response_string_ = g_next_body;
    return response_string_.c_str();
}

// ── CapturedCallView helpers ─────────────────────────────────────────────
std::string CapturedCallView::body_as_string() const {
    return std::string(call.body.begin(), call.body.end());
}

std::string CapturedCallView::header(const std::string& name) const {
    for (const auto& h : call.headers) {
        // case-insensitive name match
        if (!h.name.empty() && !name.empty() &&
            h.name.size() == name.size() &&
            std::equal(h.name.begin(), h.name.end(), name.begin(),
                       [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) ==
                                  std::tolower(static_cast<unsigned char>(b));
                       })) {
            return h.value;
        }
    }
    return "";
}
