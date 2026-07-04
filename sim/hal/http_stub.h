// http_stub.h
// =============================================================================
// Mock HTTPClient + WiFiClientSecure for host-side publish-path validation.
//
// The sim's real connectivity_manager.cpp pulls in PubSubClient, Blynk, and
// the ESP32 WiFiClientSecure (mbedTLS) — none of which compile on the host.
// Rather than link the whole module, the test calls the small JSON-construction
// paths in this directory directly, and the HTTPClient here captures the URL,
// headers, and body to an in-memory buffer for inspection.
//
// The mock is "good enough" to satisfy the bits the firmware code touches
// (begin(), addHeader(), POST(), sendRequest(), GET(), end(), getStream()).
// It does NOT actually open a socket — instead it records the call and
// returns a configurable status code so the caller can branch on it.
// =============================================================================

#ifndef SIM_HTTP_STUB_H
#define SIM_HTTP_STUB_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <utility>

class FakeHttpResponseStream;

// --- Header pair ---------------------------------------------------------
struct HttpHeader {
    std::string name;
    std::string value;
};

// --- One captured HTTP call ----------------------------------------------
struct CapturedCall {
    std::string url;          // full URL passed to begin()
    std::string method;       // "POST" / "PATCH" / "GET"
    std::vector<HttpHeader> headers;  // name=value pairs in insertion order
    std::vector<uint8_t> body;        // raw POST/PATCH body
    int    status_code;       // what to return to the caller (200, 204, ...)
    std::string response_body;  // what the caller reads back from getStream()
};

// --- Global capture buffer ----------------------------------------------
// Every call to HttpClient::begin() pushes a fresh CapturedCall onto this
// stack. Tests clear() between scenarios so each one gets a clean slate.
std::vector<CapturedCall>& http_capture();

// Configure the response code and body that the NEXT POST/GET will return.
// Cleared by each call to begin().
void http_set_next_response(int status, const std::string& body);

// --- Stubs for the WiFiClientSecure + HTTPClient surfaces the firmware uses

// Stand-in for the Arduino Client that WiFiClientSecure inherits. The
// firmware's HTTPClient takes a `Client&`, so we need *something* to hand
// it; this class is empty other than the `connected()` probe that some
// code paths use to detect a dead TLS session.
class WiFiClientSecure {
public:
    WiFiClientSecure();
    void setInsecure();
    void setHandshakeTimeout(int s);
    int  connect(const char* host, uint16_t port);
    int  connected();
    void stop();
    operator bool();
};

// Minimal Stream surface (only what HTTPClient::getStream() needs).
class FakeStreamBuf {
public:
    FakeStreamBuf();
    void set_body(const std::string& body);
    int  available();
    int  read();
private:
    std::string body_;
    size_t      pos_;
};

// HTTPClient — small subset of the Arduino API: begin, addHeader, POST, PATCH,
// GET, sendRequest, end, getStream, getStatusCode. Anything else is a no-op
// so an actual firmware path that calls something exotic still compiles.
class HTTPClient {
public:
    HTTPClient();
    ~HTTPClient();

    // begin() returns true on "success". The real library would do a DNS
    // lookup + TLS handshake; here we just record the URL.
    bool begin(WiFiClientSecure& client, const char* url);
    bool begin(const char* url);

    void setReuse(bool reuse);
    void addHeader(const char* name, const char* value);

    // POST with raw body. status_code = http_set_next_response().
    int  POST(const uint8_t* body, size_t len);
    int  POST(const char* body, size_t len);

    int  PATCH(const uint8_t* body, size_t len);
    int  sendRequest(const char* method, const uint8_t* body, size_t len);
    int  GET();

    void end();
    int  getStatusCode();
    FakeStreamBuf& getStream();
    const char* getString();

private:
    void init(const char* url);

    std::string url_;
    std::vector<HttpHeader> headers_;
    std::vector<uint8_t> body_;
    int  status_code_;
    bool reuse_;
    bool ended_;
    FakeStreamBuf stream_;
    std::string response_string_;
};

// C++ streams don't exist on the firmware, but the test code may want a
// std::ostream-style view of the captured body. Provide a tiny adapter.
struct CapturedCallView {
    const CapturedCall& call;
    explicit CapturedCallView(const CapturedCall& c) : call(c) {}
    // Return a copy of the body as std::string (so it survives a clear()).
    std::string body_as_string() const;
    // Return a single header value or "" if not present.
    std::string header(const std::string& name) const;
};

#endif // SIM_HTTP_STUB_H
