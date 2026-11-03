// Deterministic mutation fuzzer for every parser that touches untrusted bytes.
//   ./fuzz_parsers [iterations=20000] [seed=1]
// It checks INVARIANTS, not just "does not crash" (build with -fsanitize=address,undefined to catch memory errors):
//   * request parser : Complete => 0 < consumed <= size; re-parsing exactly the consumed bytes gives the same answer;
//                      every shorter prefix is NeedMore/Bad, never Complete; trailing garbage (pipelining) changes nothing
//   * response parser: feeding in random pieces gives the same verdict as feeding everything at once
//   * chunked scanner: consumed <= size, output <= limit, same verdict for the whole input and for the consumed part
//   * SOCKS5 replies : incremental prefixes are consistent with the complete message
//   * JSON / config  : only std::exception on bad input; escapes always produce parseable JSON
#include "config.hpp"
#include "http.hpp"
#include "json.hpp"
#include "net.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>

using namespace gateway;

static std::mt19937_64 rng;
static uint64_t g_case = 0;
static std::string g_what;

static size_t rnd(size_t n) { return n ? static_cast<size_t>(rng() % n) : 0; }

[[noreturn]] static void fail(const std::string& why, const std::string& input) {
    std::fprintf(stderr, "\nFUZZ INVARIANT VIOLATED (case %llu, %s): %s\ninput (%zu bytes, hex): ",
                 static_cast<unsigned long long>(g_case), g_what.c_str(), why.c_str(), input.size());
    for (unsigned char c : input) std::fprintf(stderr, "%02x", c);
    std::fprintf(stderr, "\n");
    std::abort();
}
#define REQUIRE(cond, input) do { if (!(cond)) fail(#cond, (input)); } while (0)

static const std::vector<std::string>& tokens() {
    static const std::vector<std::string> t = {
        "\r\n", "\r\n\r\n", "\n", "\r", ": ", "Content-Length: ", "Transfer-Encoding: chunked\r\n", "Transfer-Encoding: ", "chunked",
        "Connection: close\r\n", "Expect: 100-continue\r\n", "Host: ", "0", "-1", "99999999999999999999", "4294967296", "FFFFFFFF", "0\r\n\r\n",
        "HTTP/1.1", "HTTP/1.0", "HTTP/9.9", " ", "\t", std::string(1, '\0'), "\xff\xfe", "http://", "https://", "[::1]", "@", "%", "..", "//",
        "GET ", "POST ", "CONNECT ", "HEAD ", "1;ext=1\r\n", "a\r\n", ";", ",", "\"", "\\", "{", "}", "[", "]", "null", "true", "1e999", "\\u00",
        "\x05\x01\x00", "\x05\x00", "\x01\x00"};
    return t;
}

static std::string mutate(std::string s, const std::vector<std::string>& seeds) {
    int steps = 1 + static_cast<int>(rnd(6));
    for (int i = 0; i < steps; ++i) {
        switch (rnd(9)) {
            case 0: if (!s.empty()) s[rnd(s.size())] ^= static_cast<char>(1u << rnd(8)); break;
            case 1: if (!s.empty()) s[rnd(s.size())] = static_cast<char>(rnd(256)); break;
            case 2: s.insert(rnd(s.size() + 1), 1, static_cast<char>(rnd(256))); break;
            case 3: if (!s.empty()) { size_t a = rnd(s.size()); s.erase(a, 1 + rnd(std::min<size_t>(16, s.size() - a))); } break;
            case 4: s.insert(rnd(s.size() + 1), tokens()[rnd(tokens().size())]); break;
            case 5: if (!s.empty()) s.resize(rnd(s.size())); break;                                   // truncate
            case 6: if (!s.empty()) { size_t a = rnd(s.size()), l = 1 + rnd(std::min<size_t>(64, s.size() - a)); s.insert(rnd(s.size() + 1), s.substr(a, l)); } break;
            case 7: s.insert(rnd(s.size() + 1), seeds[rnd(seeds.size())].substr(0, rnd(80))); break;  // splice
            case 8: s.insert(rnd(s.size() + 1), std::string(1 + rnd(300), static_cast<char>('a' + rnd(26)))); break;
        }
        if (s.size() > 20000) s.resize(20000);
    }
    return s;
}

static std::string random_bytes() {
    std::string s(rnd(200), '\0');
    for (auto& c : s) c = static_cast<char>(rnd(256));
    return s;
}

// ------------------------------------------------------------------------------------------- targets
static void fuzz_request(const std::string& in) {
    g_what = "request-parser";
    const size_t MH = 4096, MB = 1 << 16;
    ParseResult r = parse_http_request(in, MH, MB);
    if (r.status == ParseStatus::Complete) {
        REQUIRE(r.consumed > 0 && r.consumed <= in.size(), in);
        REQUIRE(!r.req.method.empty() && !r.req.target.empty(), in);
        ParseResult again = parse_http_request(in.substr(0, r.consumed), MH, MB);       // exactly the consumed bytes
        REQUIRE(again.status == ParseStatus::Complete && again.consumed == r.consumed, in);
        REQUIRE(again.req.method == r.req.method && again.req.target == r.req.target && again.req.body == r.req.body, in);
        ParseResult pipelined = parse_http_request(in.substr(0, r.consumed) + "GET /next HTTP/1.1\r\n\r\n", MH, MB);
        REQUIRE(pipelined.status == ParseStatus::Complete && pipelined.consumed == r.consumed, in);
        for (int k = 0; k < 4 && r.consumed > 1; ++k) {                                // no shorter prefix may complete
            size_t cut = rnd(r.consumed);
            REQUIRE(parse_http_request(in.substr(0, cut), MH, MB).status != ParseStatus::Complete, in.substr(0, cut));
        }
    } else if (r.status == ParseStatus::Bad) {
        REQUIRE(r.error_status >= 400 && r.error_status < 600 && !r.error.empty(), in);
    }
}

static void fuzz_response(const std::string& in, bool head) {
    g_what = head ? "response-parser(HEAD)" : "response-parser";
    auto run = [&](const std::vector<size_t>& cuts, ResponseData& out) {
        ResponseParser p(head, 1 << 20);
        ReadStatus st = ReadStatus::NeedMore;
        size_t prev = 0;
        for (size_t c : cuts) {
            if (c > in.size()) c = in.size();
            if (c < prev) continue;
            st = p.feed(in.data() + prev, c - prev);
            prev = c;
            if (st != ReadStatus::NeedMore) break;
        }
        if (st == ReadStatus::NeedMore && prev < in.size()) st = p.feed(in.data() + prev, in.size() - prev);
        if (st == ReadStatus::NeedMore) st = p.finish_eof();
        out = p.data;
        return st;
    };
    ResponseData whole, split;
    ReadStatus a = run({}, whole);
    std::vector<size_t> cuts;
    int n = 1 + static_cast<int>(rnd(8));
    for (int i = 0; i < n; ++i) cuts.push_back(rnd(in.size() + 1));
    std::sort(cuts.begin(), cuts.end());
    ReadStatus b = run(cuts, split);
    std::string ctx = in;
    REQUIRE(a == b, ctx);
    if (a == ReadStatus::Ok) REQUIRE(whole.status == split.status && whole.framed == split.framed && whole.status >= 100 && whole.status <= 999, ctx);
    ResponseParser bytewise(head, 1 << 20);                                            // one byte at a time
    ReadStatus c = ReadStatus::NeedMore;
    for (char ch : in) { c = bytewise.feed(&ch, 1); if (c != ReadStatus::NeedMore) break; }
    if (c == ReadStatus::NeedMore) c = bytewise.finish_eof();
    REQUIRE(a == c, ctx);
}

static void fuzz_chunked(const std::string& in) {
    g_what = "chunked-scanner";
    const size_t MAXO = 4096;
    std::string out;
    size_t consumed = 0;
    ChunkStatus st = scan_chunked(in.data(), in.size(), &out, consumed, MAXO);
    REQUIRE(consumed <= in.size() && out.size() <= MAXO, in);
    if (st == ChunkStatus::Complete) {
        std::string out2;
        size_t c2 = 0;
        REQUIRE(scan_chunked(in.data(), consumed, &out2, c2, MAXO) == ChunkStatus::Complete && c2 == consumed && out2 == out, in);
        if (consumed > 1) {
            std::string o3;
            size_t c3 = 0;
            REQUIRE(scan_chunked(in.data(), rnd(consumed), &o3, c3, MAXO) != ChunkStatus::Complete, in);
        }
    }
    size_t c4 = 0;                                                                     // size-only mode (no output buffer)
    scan_chunked(in.data(), in.size(), nullptr, c4, MAXO);
    REQUIRE(c4 <= in.size(), in);
}

static void fuzz_socks(const std::string& in) {
    g_what = "socks5";
    size_t total = 0;
    int rep = 0;
    int st = socks5_parse_reply(in, total, rep);
    if (st == 1) {
        REQUIRE(total >= 7 && total <= in.size() && rep == 0, in);
        for (int k = 0; k < 4; ++k) {
            size_t cut = rnd(total);
            size_t t2 = 0; int r2 = 0;
            REQUIRE(socks5_parse_reply(in.substr(0, cut), t2, r2) == 0, in.substr(0, cut));
        }
        size_t t3 = 0; int r3 = 0;
        REQUIRE(socks5_parse_reply(in.substr(0, total), t3, r3) == 1 && t3 == total, in);
    }
    int m = socks5_parse_method_reply(in);
    REQUIRE(m >= -1 && m <= 2, in);
    int a = socks5_parse_auth_reply(in);
    REQUIRE(a >= -1 && a <= 1, in);
    std::string req;
    std::string host = in.substr(0, rnd(300));
    if (socks5_connect_request(host, static_cast<uint16_t>(rnd(65536)), req)) REQUIRE(req.size() >= 8 && req.size() <= 262, host);
    std::string ar;
    if (socks5_auth_request(in.substr(0, rnd(300)), in.substr(rnd(in.size() + 1), rnd(300)), ar)) REQUIRE(ar.size() >= 5 && ar.size() <= 513, in);
}

static void fuzz_urls(const std::string& in) {
    g_what = "url/authority";
    std::string host;
    uint16_t port = 0;
    if (parse_authority(in, host, port, 80)) REQUIRE(!host.empty() && port != 0 && host.find('/') == std::string::npos, in);
    UrlParts u;
    if (parse_absolute_url(in, u)) REQUIRE(!u.host.empty() && !u.path.empty() && u.path[0] == '/' && u.port != 0, in);
    SockAddr a;
    if (parse_ip_literal(in, 1, a)) REQUIRE(a.len > 0 && !addr_to_string(a).empty(), in);
}

static void fuzz_json_and_config(const std::string& in) {
    g_what = "json/config";
    try { json::Value v = json::parse(in); (void)v; } catch (const std::exception&) {}
    GatewayConfig cfg;
    std::streambuf* old = std::cerr.rdbuf(nullptr);        // unknown-key warnings are expected noise here
    try { load_config_from_string(in, cfg); } catch (const std::exception&) {}
    std::cerr.rdbuf(old);
    std::string e = json_escape(in);                       // must always be a valid JSON string body
    try { json::parse("\"" + e + "\""); } catch (const std::exception& ex) { fail(std::string("json_escape output rejected: ") + ex.what(), in); }
    std::string c = csv_escape(in);
    REQUIRE(c.find('\0') == std::string::npos && c.find('\n') == std::string::npos && c.find('\r') == std::string::npos, in);
    std::string l = label_escape(in);
    REQUIRE(l.find('\n') == std::string::npos && l.find('"') == std::string::npos ? true : l.find("\\\"") != std::string::npos, in);
}

int main(int argc, char** argv) {
    uint64_t iters = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000;
    uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    rng.seed(seed);

    const std::vector<std::string> req_seeds = {
        "GET http://example.org/a?b=c HTTP/1.1\r\nHost: example.org\r\n\r\n",
        "POST http://example.org:8080/up HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello",
        "POST http://example.org/c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
        "CONNECT example.org:443 HTTP/1.1\r\nHost: example.org:443\r\n\r\n",
        "PUT http://[::1]:9/x HTTP/1.1\r\nHost: [::1]:9\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\nabc",
        "GET /metrics HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer abc\r\nConnection: close\r\n\r\n",
        "HEAD http://a/ HTTP/1.0\r\n\r\n"};
    const std::vector<std::string> resp_seeds = {
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\nTrailer: x\r\n\r\nEXTRA",
        "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n",
        "HTTP/1.0 200 OK\r\nConnection: close\r\n\r\nuntil-close",
        "HTTP/1.1 304 Not Modified\r\nETag: \"x\"\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"};
    const std::vector<std::string> chunk_seeds = {"5\r\nhello\r\n0\r\n\r\n", "a;ext=1\r\n0123456789\r\n0\r\nTrailer: x\r\n\r\n", "0\r\n\r\n",
                                                  "1\r\nx\r\n1\r\ny\r\n0\r\n\r\n"};
    const std::vector<std::string> socks_seeds = {std::string("\x05\x00\x00\x01\x7f\x00\x00\x01\x1f\x90", 10), std::string("\x05\x00\x00\x03\x03" "abc" "\x00\x50", 10),
                                                  std::string("\x05\x00\x00\x04", 4) + std::string(18, 'x'), std::string("\x05\x05\x00\x01", 4), std::string("\x05\x02", 2),
                                                  std::string("\x01\x00", 2), "example.org", "10.0.0.1", "::1"};
    const std::vector<std::string> url_seeds = {"http://example.org/a?b#c", "https://[::1]:8443/x", "example.org:443", "[2001:db8::1]:80", "user@host:1", "http://h:99999/"};
    const std::vector<std::string> json_seeds = {
        R"({"listen_port":8000,"proxies":[{"host":"a","port":1,"protocol":"socks5","username":"u","password":"p"}],"admin":{"token":"12345678"}})",
        R"({"workers":{"min":1,"max":8,"initial":4},"timeouts":{"connect_ms":100},"rate_limits":{"a:1":{"rate":1,"burst":2}},"proxies":[{"host":"h","port":2}]})",
        R"(["\u00e9\ud83d\ude00", 1.5e3, -0, true, null, {"a":{}}])", "{\"a\":\"\\u00zz\"}"};

    uint64_t last = 0;
    for (g_case = 0; g_case < iters; ++g_case) {
        std::string a = rnd(8) == 0 ? random_bytes() : mutate(req_seeds[rnd(req_seeds.size())], req_seeds);
        fuzz_request(a);
        std::string b = rnd(8) == 0 ? random_bytes() : mutate(resp_seeds[rnd(resp_seeds.size())], resp_seeds);
        fuzz_response(b, rnd(4) == 0);
        std::string c = rnd(8) == 0 ? random_bytes() : mutate(chunk_seeds[rnd(chunk_seeds.size())], chunk_seeds);
        fuzz_chunked(c);
        std::string d = rnd(6) == 0 ? random_bytes() : mutate(socks_seeds[rnd(socks_seeds.size())], socks_seeds);
        fuzz_socks(d);
        std::string e = rnd(8) == 0 ? random_bytes() : mutate(url_seeds[rnd(url_seeds.size())], url_seeds);
        fuzz_urls(e);
        std::string f = rnd(10) == 0 ? random_bytes() : mutate(json_seeds[rnd(json_seeds.size())], json_seeds);
        fuzz_json_and_config(f);
        if (g_case - last >= 100000) { last = g_case; std::fprintf(stderr, "  ... %llu cases\n", static_cast<unsigned long long>(g_case)); }
    }
    std::cout << "[PASS] Parser fuzzing: " << iters << " cases x 6 targets (seed " << seed << "), all invariants held\n";
    return 0;
}
