#include "aimd.hpp"
#include "config.hpp"
#include "fd_budget.hpp"
#include "security.hpp"
#include "upstream_engine.hpp"
#include "http.hpp"
#include "json.hpp"
#include "net.hpp"
#include "proxy_manager.hpp"
#include "scheduler.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <sys/socket.h>

using namespace gateway;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "CHECK FAILED: " #cond " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            std::exit(1);                                                                \
        }                                                                                \
    } while (0)

template <typename F>
static bool throws(F f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}

// ------------------------------------------------------------------ scheduler
void test_scheduler_and_token_bucket() {
    GatewayConfig cfg;
    cfg.max_queue_size = 5;
    cfg.domain_rate_limits["sitea.example"] = {10.0, 2.0};  // 10 req/s, burst 2
    JobScheduler sched(cfg);

    for (int i = 0; i < 5; ++i) {
        Job j;
        j.id = i + 1;
        j.domain = "sitea.example";
        CHECK(sched.enqueue(j));
    }
    Job overflow_job;
    overflow_job.domain = "sitea.example";
    CHECK(!sched.enqueue(overflow_job));  // FR-1: bounded queue

    CHECK(sched.try_pop_ready_job().has_value());
    CHECK(sched.try_pop_ready_job().has_value());
    CHECK(!sched.try_pop_ready_job().has_value());  // burst exhausted (FR-2)
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    auto j = sched.try_pop_ready_job();
    CHECK(j.has_value() && j->id == 3);  // FIFO order preserved
    std::cout << "[PASS] Scheduler FIFO & token-bucket rate limiter\n";
}

void test_scheduler_blocking_expiry_and_shutdown() {
    GatewayConfig cfg;
    cfg.default_rate_limit = {1.0, 1.0};
    JobScheduler sched(cfg);
    auto now = Clock::now();
    for (int i = 0; i < 3; ++i) {
        Job j;
        j.id = i + 1;
        j.domain = "slow.example";
        j.deadline = now + std::chrono::milliseconds(300);  // 1 token available, the other two must expire
        CHECK(sched.enqueue(j));
    }
    std::vector<Job> expired;
    auto first = sched.pop_blocking(expired);
    CHECK(first.has_value() && first->id == 1 && expired.empty());

    auto t0 = Clock::now();
    auto second = sched.pop_blocking(expired);  // blocks until the deadline, no busy loop
    auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(!second.has_value());
    CHECK(expired.size() == 2 && expired[0].id == 2 && expired[1].id == 3);
    CHECK(waited >= 200 && waited < 700);
    CHECK(sched.total_queued() == 0);

    // A blocked worker is woken by enqueue() and by shutdown().
    std::vector<Job> ex2;
    std::optional<Job> got;
    std::thread waiter([&]() { got = sched.pop_blocking(ex2); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Job j;
    j.id = 99;
    j.domain = "other.example";
    sched.enqueue(j);
    waiter.join();
    CHECK(got.has_value() && got->id == 99);

    std::thread waiter2([&]() { got = sched.pop_blocking(ex2); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sched.shutdown();
    waiter2.join();
    CHECK(!got.has_value());
    std::cout << "[PASS] Scheduler blocking pop, queue timeout expiry, wake-up and shutdown\n";
}

// ------------------------------------------------------------ circuit breaker
void test_circuit_breaker_and_routing() {
    GatewayConfig cfg;
    cfg.policy = RoutingPolicy::LOWEST_LATENCY;
    cfg.circuit_threshold = 5;
    cfg.circuit_cooldown_sec = 1;
    cfg.proxies = {{"127.0.0.1", 8101}, {"127.0.0.1", 8102}};

    ProxyManager pm(cfg);
    auto p0 = pm.select_proxy();
    (void)p0;
    pm.record_outcome(0, true, 5.0);
    pm.record_outcome(1, true, 120.0);

    auto chosen = pm.select_proxy();
    CHECK(chosen != nullptr && chosen->id == 0);
    pm.record_outcome(chosen->id, true, 5.0);

    for (int i = 0; i < 5; ++i) pm.record_outcome(0, false, 2000.0);
    CHECK(pm.proxies()[0]->circuit == CircuitState::OPEN);

    auto fallback = pm.select_proxy();
    CHECK(fallback != nullptr && fallback->id == 1);
    pm.record_outcome(1, true, 100.0);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto recovery = pm.select_proxy({1});
    CHECK(recovery != nullptr && recovery->id == 0);
    pm.record_outcome(0, true, 6.0);
    CHECK(pm.proxies()[0]->circuit == CircuitState::CLOSED);
    std::cout << "[PASS] Circuit breaker (CLOSED -> OPEN -> HALF-OPEN -> CLOSED) & routing\n";
}

void test_lowest_latency_concurrent_no_deadlock() {
    GatewayConfig cfg;
    cfg.policy = RoutingPolicy::LOWEST_LATENCY;
    cfg.proxies = {{"127.0.0.1", 8101}, {"127.0.0.1", 8102}, {"127.0.0.1", 8103}};
    ProxyManager pm(cfg);
    std::atomic<bool> done{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&]() {
            for (int i = 0; i < 20000; ++i) {
                auto p = pm.select_proxy();
                if (p) pm.record_outcome(p->id, true, 1.0 + (i % 7));
            }
        });
    }
    std::thread watchdog([&]() {
        for (int i = 0; i < 300 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!done) { std::cerr << "DEADLOCK in lowest-latency selection\n"; std::_Exit(1); }
    });
    for (auto& t : ts) t.join();
    done = true;
    watchdog.join();
    std::cout << "[PASS] Lowest-latency selection is deadlock-free under concurrency\n";
}

void test_aimd_controller() {
    GatewayConfig cfg;
    cfg.workers_min = 2;
    cfg.workers_max = 32;
    cfg.workers_initial = 10;
    cfg.aimd_alpha = 2;
    cfg.aimd_beta = 0.5;
    AIMDController aimd(cfg);
    int notifications = 0;
    aimd.set_on_change([&]() { ++notifications; });
    CHECK(aimd.limit() == 10);
    for (int i = 0; i < 20; ++i) aimd.record_feedback(true, false);
    CHECK(aimd.limit() == 12);
    for (int i = 0; i < 20; ++i) aimd.record_feedback(false, true);
    CHECK(aimd.limit() == 6);
    CHECK(notifications == 2);
    std::cout << "[PASS] AIMD concurrency controller\n";
}

// ---------------------------------------------------------------------- JSON
void test_json_parser() {
    auto v = json::parse(" {\n \"a\" : [1, 2.5e1, -3, true, false, null],\"b\":{\"c\":\"x\\ny\\u00e9\\ud83d\\ude00\"}\t} ");
    CHECK(v.is_object());
    CHECK(v.find("a")->items.size() == 6);
    CHECK(v.find("a")->items[1].number == 25.0);
    CHECK(v.find("b")->find("c")->str == "x\nyé😀");
    CHECK(json::parse("\xEF\xBB\xBF{}").is_object());  // BOM tolerated
    CHECK(json::parse("[]").is_array());

    const char* bad[] = {"", "{", "{\"a\":1,}", "[1,]", "{'a':1}", "{\"a\":01}", "{\"a\":1} x", "[1 2]", "{\"a\" 1}",
                         "\"abc", "[\"\\x\"]", "{\"a\":1,\"a\":2}", "[\"\t\"]", "1.", "[-]", "nul", "{\"a\":\"\\ud800\"}"};
    for (const char* b : bad) {
        if (!throws([&]() { json::parse(b); })) { std::cerr << "should reject: " << b << "\n"; std::exit(1); }
    }
    std::string deep(200, '[');
    CHECK(throws([&]() { json::parse(deep); }));  // depth limit

    try { json::parse("{\n  \"a\": 1,\n  \"b\": ?\n}"); CHECK(false); }
    catch (const json::ParseError& e) { CHECK(std::string(e.what()).find("line 3") != std::string::npos); }
    std::cout << "[PASS] JSON parser (valid, invalid, unicode, depth, error positions)\n";
}

void test_config_loading() {
    GatewayConfig cfg;
    // Unusual formatting, key order, escapes, extra whitespace -- all legal JSON.
    load_config_from_string(R"({"proxies":[{"port":8101,"host":"a.example"},
        {"host":"127.0.0.1","port":8102}],
        "workers":{"initial":8,"max":32,"min":2,"alpha":1,"beta":0.25},
        "policy":"round_robin","listen_port":9001,
        "rate_limits":{"Foo.Example":{"burst":10,"rate":5}},
        "default_rate_limit":{"rate":7,"burst":9},
        "timeouts":{"queue_ms":1234},
        "output":{"csv_path":"","json_path":""}})", cfg);
    CHECK(cfg.proxies.size() == 2 && cfg.proxies[0].host == "a.example" && cfg.proxies[0].port == 8101);
    CHECK(cfg.workers_min == 2 && cfg.workers_max == 32 && cfg.workers_initial == 8 && cfg.aimd_alpha == 1);
    CHECK(cfg.aimd_beta == 0.25 && cfg.policy == RoutingPolicy::ROUND_ROBIN && cfg.listen_port == 9001);
    CHECK(cfg.domain_rate_limits.count("foo.example") == 1);  // domain keys are case-insensitive
    CHECK(cfg.default_rate_limit.rate == 7 && cfg.default_rate_limit.burst == 9);
    CHECK(cfg.queue_timeout_ms == 1234 && cfg.csv_output_path.empty());

    auto base = R"("proxies":[{"host":"h","port":1}])";
    auto bad = [&](const std::string& s) { GatewayConfig c; return throws([&]() { load_config_from_string(s, c); }); };
    CHECK(bad("{}"));                                                         // no proxies
    CHECK(bad(std::string("{") + base + ",\"listen_port\":\"80\"}"));          // wrong type
    CHECK(bad(std::string("{") + base + ",\"listen_port\":70000}"));           // range
    CHECK(bad(std::string("{") + base + ",\"policy\":\"nope\"}"));
    CHECK(bad(std::string("{") + base + ",\"workers\":{\"min\":10,\"max\":5}}"));
    CHECK(bad(std::string("{") + base + ",\"workers\":{\"beta\":1.5}}"));
    CHECK(bad(std::string("{") + base + ",\"default_rate_limit\":{\"rate\":0,\"burst\":5}}"));
    CHECK(bad(std::string("{") + base + ",\"max_retries\":1.5}"));             // integer required
    CHECK(bad("{\"proxies\":[{\"host\":\"h\",\"port\":0}]}"));
    CHECK(bad("{\"proxies\":[{\"host\":\"h\"}]}"));
    CHECK(bad("[1,2]"));
    std::cout << "[PASS] Config loading: strict types/ranges, any key order/whitespace\n";
}

// ----------------------------------------------------------------- HTTP parse
void test_http_request_parser() {
    auto parse = [](const std::string& s) { return parse_http_request(s, 8192, 1 << 20); };

    auto r = parse("GET http://a.example/x HTTP/1.1\r\nHost: a.example\r\n\r\n");
    CHECK(r.status == ParseStatus::Complete && r.req.method == "GET" && r.req.keep_alive);

    // Partial requests: every strict prefix must be NeedMore, and the full text Complete.
    std::string post = "POST http://a.example/p HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";
    for (size_t i = 0; i < post.size(); ++i) CHECK(parse(post.substr(0, i)).status == ParseStatus::NeedMore);
    r = parse(post + "GET / HTTP/1.1\r\n\r\n");  // pipelined leftover is not consumed
    CHECK(r.status == ParseStatus::Complete && r.req.body == "hello" && r.consumed == post.size());

    std::string chunked = "POST http://a/p HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n4;ext=1\r\ndefg\r\n0\r\nX: y\r\n\r\n";
    for (size_t i = 0; i < chunked.size(); ++i) CHECK(parse(chunked.substr(0, i)).status == ParseStatus::NeedMore);
    r = parse(chunked);
    CHECK(r.status == ParseStatus::Complete && r.req.body == "abcdefg" && r.consumed == chunked.size());

    r = parse("POST http://a/p HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n");
    CHECK(r.status == ParseStatus::NeedMore && r.want_continue);

    CHECK(parse("GET / HTTP/1.0\r\n\r\n").req.keep_alive == false);
    CHECK(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").req.keep_alive == true);
    CHECK(parse("GET / HTTP/1.1\r\nConnection: close\r\n\r\n").req.keep_alive == false);
    CHECK(parse("\r\n\r\nGET / HTTP/1.1\r\n\r\n").status == ParseStatus::Complete);  // leading CRLF tolerated

    // Rejections (request smuggling guards, malformed input, limits)
    CHECK(parse("GARBAGE\r\n\r\n").status == ParseStatus::Bad);
    CHECK(parse("GET / HTTP/2.0\r\n\r\n").status == ParseStatus::Bad);
    CHECK(parse("GET /a b HTTP/1.1\r\n\r\n").status == ParseStatus::Bad);
    CHECK(parse("GET / HTTP/1.1\r\nBad Header: x\r\n\r\n").status == ParseStatus::Bad);
    CHECK(parse("GET / HTTP/1.1\r\n folded: x\r\n\r\n").status == ParseStatus::Bad);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n").status == ParseStatus::Bad);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n").status == ParseStatus::Bad);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n").status == ParseStatus::Bad);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n").status == ParseStatus::Bad);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 5000000\r\n\r\n").error_status == 413);
    CHECK(parse("POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n").error_status == 501);
    CHECK(parse("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n").status == ParseStatus::Bad);
    CHECK(parse(std::string("GET / HTTP/1.1\r\nX: ") + std::string(9000, 'a') + "\r\n\r\n").error_status == 431);
    CHECK(parse(std::string(9000, 'a')).error_status == 431);  // header never terminates
    std::cout << "[PASS] HTTP request parser: bodies, chunked, partial prefixes, pipelining, smuggling guards\n";
}

void test_url_and_authority() {
    UrlParts u;
    CHECK(parse_absolute_url("http://Test.Example:8080/a/b?q=1#frag", u));
    CHECK(u.authority == "test.example:8080" && u.host == "Test.Example" && u.port == 8080 && u.path == "/a/b?q=1" && !u.https);
    CHECK(parse_absolute_url("http://h", u) && u.path == "/" && u.port == 80);
    CHECK(parse_absolute_url("http://h?x=1", u) && u.path == "/?x=1");
    CHECK(parse_absolute_url("https://h/x", u) && u.https && u.port == 443);
    CHECK(parse_absolute_url("http://[::1]:81/z", u) && u.host == "::1" && u.port == 81);
    CHECK(!parse_absolute_url("ftp://h/", u));
    CHECK(!parse_absolute_url("http://", u));
    CHECK(!parse_absolute_url("http://h:99999/", u));
    CHECK(!parse_absolute_url("http://h:0/", u));
    CHECK(!parse_absolute_url("http://u:p@h/", u));
    CHECK(!parse_absolute_url("http://::1/", u));
    std::string host;
    uint16_t port = 0;
    CHECK(parse_authority("example.com:443", host, port, -1) && port == 443);
    CHECK(!parse_authority("example.com", host, port, -1));
    CHECK(parse_authority("example.com", host, port, 80) && port == 80);
    std::cout << "[PASS] URL / authority parsing\n";
}

void test_response_reader_and_escapes() {
    auto feed = [](const std::string& data, bool head, ResponseData& rd, bool close_after = true) {
        int sv[2];
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        std::thread w([&]() {
            for (size_t i = 0; i < data.size(); i += 7) {  // dribble 7 bytes at a time
                ::send(sv[1], data.data() + i, std::min<size_t>(7, data.size() - i), 0);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            if (close_after) ::shutdown(sv[1], SHUT_WR);
        });
        ReadStatus rs = read_http_response(sv[0], head, 2000, 1 << 20, rd);
        w.join();
        ::close(sv[0]);
        ::close(sv[1]);
        return rs;
    };
    ResponseData rd;
    CHECK(feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", false, rd, false) == ReadStatus::Ok && rd.framed && rd.status == 200);
    CHECK(feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", false, rd, false) == ReadStatus::Ok && rd.framed);
    CHECK(feed("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nbody", false, rd) == ReadStatus::Ok && !rd.framed && rd.close_header);
    CHECK(feed("HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort", false, rd) == ReadStatus::Error);  // truncated
    CHECK(feed("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n", true, rd, false) == ReadStatus::Ok && rd.framed);  // HEAD: no body
    CHECK(feed("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n", false, rd, false) == ReadStatus::Ok && rd.status == 204);
    CHECK(feed("NOT HTTP\r\n\r\n", false, rd) == ReadStatus::Error);
    CHECK(feed("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokEXTRA", false, rd, false) == ReadStatus::Ok && rd.raw.size() == 40);  // never returns bytes beyond the message

    CHECK(csv_escape("a\"b,c\nd\x01") == "\"a\"\"b,c\\x0ad\\x01\"");
    CHECK(csv_escape("bad\xff" "x") == "\"bad\\xffx\"");  // invalid UTF-8 never reaches the CSV file raw
    CHECK(json_escape("q\"\\\n\x01 é") == "q\\\"\\\\\\n\\u0001 é");
    CHECK(json_escape("bad\xff" "x") == "bad\\ufffdx");
    CHECK(json_escape("\xE2\x82\xAC") == "\xE2\x82\xAC");  // valid multibyte passes through
    CHECK(label_escape("a\"b\\c") == "a\\\"b\\\\c");
    std::cout << "[PASS] Response reader (dribbled bytes, chunked, HEAD, 1xx, truncation) & log escaping\n";
}

void test_incremental_response_parser() {
    struct Case { std::string wire; bool head; ReadStatus expect; size_t keep; };
    std::vector<Case> cases = {
        {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", false, ReadStatus::Ok, 0},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", false, ReadStatus::Ok, 0},
        {"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n", false, ReadStatus::Ok, 0},
        {"HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n", true, ReadStatus::Ok, 0},
        {"HTTP/1.0 200 OK\r\n\r\nuntil-close", false, ReadStatus::Ok, 1},  // finished by EOF
        {"HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort", false, ReadStatus::Error, 1},
        {"NOT HTTP\r\n\r\n", false, ReadStatus::Error, 0},
    };
    for (auto& c : cases) {
        // Every possible split into two reads, and one byte at a time, must give the same result.
        for (size_t cut = 0; cut <= c.wire.size(); ++cut) {
            ResponseParser p(c.head, 1 << 20);
            ReadStatus st = ReadStatus::NeedMore;
            if (cut) st = p.feed(c.wire.data(), cut);
            if (st == ReadStatus::NeedMore && cut < c.wire.size()) st = p.feed(c.wire.data() + cut, c.wire.size() - cut);
            if (st == ReadStatus::NeedMore) st = p.finish_eof();
            CHECK(st == c.expect);
            if (st == ReadStatus::Ok) CHECK(p.data.raw.size() <= c.wire.size());
        }
        ResponseParser p(c.head, 1 << 20);
        ReadStatus st = ReadStatus::NeedMore;
        for (char ch : c.wire) { st = p.feed(&ch, 1); if (st != ReadStatus::NeedMore) break; }
        if (st == ReadStatus::NeedMore) st = p.finish_eof();
        CHECK(st == c.expect);
    }
    ResponseParser big(false, 100);  // size cap
    CHECK(big.feed("HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n", 41) == ReadStatus::TooLarge);
    std::cout << "[PASS] Incremental response parser (all split points + byte-at-a-time + size cap)\n";
}

void test_scheduler_expire_without_pop() {
    GatewayConfig cfg;
    cfg.default_rate_limit = {0.001, 1.0};
    JobScheduler sched(cfg);
    for (int i = 0; i < 4; ++i) {
        Job j; j.id = i + 1; j.domain = "d.example";
        j.deadline = Clock::now() + std::chrono::milliseconds(i < 2 ? 50 : 100000);
        CHECK(sched.enqueue(j));
    }
    std::vector<Job> out;
    sched.expire(out); CHECK(out.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    sched.expire(out);
    CHECK(out.size() == 2 && out[0].id == 1 && out[1].id == 2 && sched.total_queued() == 2);
    std::cout << "[PASS] Scheduler expire() answers overdue jobs while all slots are busy\n";
}

void test_socks5_client_helpers() {
    std::string req;
    CHECK(socks5_connect_request("example.org", 443, req));
    CHECK(req == std::string("\x05\x01\x00\x03\x0b" "example.org" "\x01\xbb", 18));
    CHECK(socks5_connect_request("10.1.2.3", 80, req));
    CHECK(req == std::string("\x05\x01\x00\x01\x0a\x01\x02\x03\x00\x50", 10));
    CHECK(!socks5_connect_request("", 80, req));
    CHECK(!socks5_connect_request(std::string(256, 'a'), 80, req));
    CHECK(socks5_connect_request("::1", 80, req) && req.size() == 22 && req[3] == 4 && req[19] == 1 && req[21] == 80);   // ATYP 4
    CHECK(!socks5_connect_request("not:an:ip", 80, req));
    CHECK(socks5_parse_method_reply("") == 0 && socks5_parse_method_reply("\x05") == 0);
    CHECK(socks5_parse_method_reply(std::string("\x05\x00", 2)) == 1);
    CHECK(socks5_parse_method_reply(std::string("\x05\xff", 2)) == -1 && socks5_parse_method_reply("HT") == -1);
    CHECK(socks5_parse_method_reply(std::string("\x05\x02", 2)) == 2 && socks5_parse_method_reply(std::string("\x05\x03", 2)) == -1);
    CHECK(socks5_greeting() == std::string("\x05\x01\x00", 3) && socks5_greeting(true) == std::string("\x05\x02\x00\x02", 4));
    // RFC 1929 sub-negotiation
    std::string ar;
    CHECK(socks5_auth_request("bob", "s3cret", ar) && ar == std::string("\x01\x03" "bob" "\x06" "s3cret", 12));
    CHECK(!socks5_auth_request("", "x", ar) && !socks5_auth_request("x", "", ar));
    CHECK(!socks5_auth_request(std::string(256, 'u'), "x", ar) && socks5_auth_request(std::string(255, 'u'), std::string(255, 'p'), ar) && ar.size() == 513);
    CHECK(socks5_parse_auth_reply("") == 0 && socks5_parse_auth_reply("\x01") == 0);
    CHECK(socks5_parse_auth_reply(std::string("\x01\x00", 2)) == 1);
    CHECK(socks5_parse_auth_reply(std::string("\x01\x01", 2)) == -1 && socks5_parse_auth_reply(std::string("\x05\x00", 2)) == -1);
    // replies: every prefix is "need more" until the last byte; atyp 1 / 3 / 4; failures; garbage
    auto check_reply = [&](const std::string& w, int expect_last) {
        for (size_t n = 0; n < w.size(); ++n) { size_t t = 0; int rep = 0; CHECK(socks5_parse_reply(w.substr(0, n), t, rep) == 0); }
        size_t t = 0; int rep = 0;
        CHECK(socks5_parse_reply(w, t, rep) == expect_last);
        if (expect_last == 1) CHECK(t == w.size());
    };
    check_reply(std::string("\x05\x00\x00\x01\x7f\x00\x00\x01\x1f\x90", 10), 1);
    check_reply(std::string("\x05\x00\x00\x03\x03" "abc" "\x00\x50", 10), 1);
    check_reply(std::string("\x05\x00\x00\x04", 4) + std::string(18, 'x'), 1);
    size_t t = 0; int rep = 0;
    CHECK(socks5_parse_reply(std::string("\x05\x05\x00\x01", 4), t, rep) == -1 && rep == 5);
    CHECK(socks5_parse_reply(std::string("\x05\x00\x00\x09\x00\x00", 6), t, rep) == -1);
    CHECK(socks5_parse_reply("HTTP/1.1 200", t, rep) == -1);
    // trailing bytes after the reply are reported via `total`
    CHECK(socks5_parse_reply(std::string("\x05\x00\x00\x01\x01\x02\x03\x04\x00\x50", 10) + "HTTP", t, rep) == 1 && t == 10);
    std::cout << "[PASS] SOCKS5 client helpers (request encoding, incremental reply parsing)\n";
}

void test_ip_addresses_and_secrets() {
    SockAddr a;
    CHECK(parse_ip_literal("127.0.0.1", 8080, a) && a.family() == AF_INET && sockaddr_get_port(a.ss) == 8080 && addr_to_string(a) == "127.0.0.1");
    CHECK(parse_ip_literal("::1", 443, a) && a.family() == AF_INET6 && sockaddr_get_port(a.ss) == 443 && addr_to_string(a) == "::1");
    CHECK(parse_ip_literal("[2001:db8::7]", 1, a) && a.family() == AF_INET6 && addr_to_string(a) == "2001:db8::7" && a.len == sizeof(sockaddr_in6));
    CHECK(parse_ip_literal("::", 9, a) && a.family() == AF_INET6);
    CHECK(!parse_ip_literal("example.org", 80, a) && !parse_ip_literal("1.2.3", 80, a) && !parse_ip_literal("", 80, a) && !parse_ip_literal("[::1", 80, a));
    CHECK(resolve_addr("localhost", 80, a) && sockaddr_get_port(a.ss) == 80);   // names still resolve (either family)
    CHECK(resolve_addr("::1", 7, a) && a.family() == AF_INET6);                 // literals never touch DNS
    ProxyState p6(0, "::1", 1080), p4(1, "10.0.0.1", 1080);
    CHECK(p6.endpoint() == "[::1]:1080" && p4.endpoint() == "10.0.0.1:1080");
    // listening: IPv4 always works; IPv6 where the host supports it
    int l4 = listen_on("127.0.0.1", 0, 8);
    CHECK(l4 >= 0); if (l4 >= 0) ::close(l4);
    CHECK(listen_on("not-an-ip", 0, 8) < 0);
    int l6 = listen_on("::", 0, 8);
    if (l6 >= 0) ::close(l6); else std::cout << "      (no IPv6 on this host: dual-stack listen not exercised)\n";

    // config: listen_host / proxies may be IPv6; secrets from env and files; admin token rules
    ::setenv("GW_TEST_PW", "from-env-secret", 1);
    std::string tmp = "/tmp/gw_unit_secret_" + std::to_string(::getpid());
    { std::ofstream f(tmp); f << "from-file-secret\n\n"; }
    GatewayConfig c;
    load_config_from_string(R"({"listen_host":"::","proxies":[
        {"host":"::1","port":1080,"protocol":"socks5","username":"u","password_env":"GW_TEST_PW"},
        {"host":"[2001:db8::1]","port":1081,"protocol":"socks5","username":"u","password_file":")" + tmp + R"("},
        {"host":"h","port":3}],"admin":{"token_env":"GW_TEST_PW"}})", c);
    CHECK(c.listen_host == "::" && c.proxies.size() == 3);
    CHECK(c.proxies[0].password == "from-env-secret" && c.proxies[1].password == "from-file-secret" && c.proxies[2].username.empty());
    CHECK(c.admin_token == "from-env-secret" && !c.inline_secrets);
    auto bad_cfg = [&](const std::string& text) { GatewayConfig x; try { load_config_from_string(text, x); } catch (const std::exception&) { return true; } return false; };
    CHECK(bad_cfg(R"({"proxies":[{"host":"h","port":1,"protocol":"socks5","username":"u","password_env":"GW_NOPE_UNSET"}]})"));
    CHECK(bad_cfg(R"({"proxies":[{"host":"h","port":1,"protocol":"socks5","username":"u","password":"a","password_env":"GW_TEST_PW"}]})"));
    CHECK(bad_cfg(R"({"proxies":[{"host":"h","port":1,"protocol":"socks5","username":"u","password_file":"/nonexistent/x"}]})"));
    CHECK(bad_cfg(R"({"proxies":[{"host":"h","port":1}],"admin":{"token":"short"}})"));
    CHECK(bad_cfg(R"({"proxies":[{"host":"h","port":1}],"admin":{"token":"has space in it"}})"));
    GatewayConfig d;
    load_config_from_string(R"({"proxies":[{"host":"h","port":1}],"admin":{"token":"long-enough-token"}})", d);
    CHECK(d.admin_token == "long-enough-token" && d.inline_secrets);
    ::unlink(tmp.c_str());
    std::cout << "[PASS] IPv4/IPv6 addressing, SOCKS5 IPv6 literals, secrets from env/file, admin token validation\n";
}

void test_network_classification() {
    auto sa = [](const char* h) { SockAddr a; CHECK(parse_ip_literal(h, 1, a)); return a; };
    for (const char* h : {"127.0.0.1", "127.9.9.9", "::1", "::ffff:127.0.0.1"}) CHECK(addr_is_loopback(sa(h).ss));
    for (const char* h : {"8.8.8.8", "10.0.0.1", "::2", "::ffff:8.8.8.8", "2001:db8::1"}) CHECK(!addr_is_loopback(sa(h).ss));
    for (const char* h : {"10.255.1.1", "172.16.0.1", "172.31.255.255", "192.168.0.1", "169.254.1.1", "100.64.0.1", "100.127.255.255",
                          "fc00::1", "fd12:3456::1", "fe80::1", "::1", "::ffff:192.168.1.1"}) CHECK(addr_is_private(sa(h).ss));
    for (const char* h : {"8.8.8.8", "172.15.255.255", "172.32.0.1", "100.63.255.255", "100.128.0.1", "192.169.0.1", "11.0.0.1",
                          "2001:db8::1", "2606:4700::1", "::ffff:8.8.8.8"}) CHECK(!addr_is_private(sa(h).ss));
    CHECK(host_is_loopback_literal("localhost") && host_is_loopback_literal("127.0.0.1") && host_is_loopback_literal("[::1]"));
    CHECK(!host_is_loopback_literal("0.0.0.0") && !host_is_loopback_literal("::") && !host_is_loopback_literal("example.org"));
    GatewayConfig c;
    CHECK(!admin_exposed(c));
    c.admin_allow_remote = true; CHECK(admin_exposed(c));
    c.admin_allow_remote = false; c.admin_port = 9001; CHECK(!admin_exposed(c));
    c.admin_host = "0.0.0.0"; CHECK(admin_exposed(c));
    std::cout << "[PASS] Network classification (loopback / private / public) and admin exposure rules\n";
}

void test_chunked_trailers_and_trailing_bytes() {
    const std::string head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    const std::vector<std::pair<std::string, std::string>> cases = {   // body, bytes after the message
        {"5\r\nhello\r\n0\r\n\r\n", ""},
        {"5\r\nhello\r\n0\r\nTrailer: x\r\nX-Sum: 1\r\n\r\n", ""},              // trailers
        {"5\r\nhello\r\n0\r\nTrailer: x\r\n\r\n", "GARBAGE"},                     // trailers + bytes after the message
        {"3;ext=1\r\nabc\r\n0\r\n\r\n", "HTTP/1.1 200 OK\r\n"},                     // pipelined next response
        {"1\r\nx\r\n1\r\ny\r\n1\r\nz\r\n0\r\n\r\n", "1.1 3"},
    };
    for (const auto& cs : cases) {
        std::string wire = head + cs.first + cs.second;
        for (size_t cut = 0; cut <= wire.size(); ++cut) {   // every split point
            ResponseParser p(false, 1 << 20);
            ReadStatus st = cut ? p.feed(wire.data(), cut) : ReadStatus::NeedMore;
            if (st == ReadStatus::NeedMore && cut < wire.size()) st = p.feed(wire.data() + cut, wire.size() - cut);
            CHECK(st == ReadStatus::Ok);
            CHECK(p.data.framed && p.data.status == 200);
            CHECK(p.data.raw == head + cs.first);              // trailing bytes are cut off the message...
        }
        ResponseParser whole(false, 1 << 20);
        CHECK(whole.feed(wire.data(), wire.size()) == ReadStatus::Ok);
        CHECK(whole.data.extra_bytes == !cs.second.empty());   // ...and flagged, so the connection is not reused
        ResponseParser bytewise(false, 1 << 20);
        ReadStatus st = ReadStatus::NeedMore;
        for (char ch : wire) { st = bytewise.feed(&ch, 1); if (st != ReadStatus::NeedMore) break; }
        CHECK(st == ReadStatus::Ok && bytewise.data.raw == head + cs.first);
    }
    // a 20 MB chunked body arriving in 1 KB reads must not be re-scanned from the start every time (linear time)
    std::string big = head;
    for (int i = 0; i < 20; ++i) big += "100000\r\n" + std::string(0x100000, 'x') + "\r\n";
    big += "0\r\n\r\n";
    ResponseParser lp(false, size_t(1) << 31);
    auto t0 = std::chrono::steady_clock::now();
    ReadStatus st = ReadStatus::NeedMore;
    for (size_t off = 0; off < big.size() && st == ReadStatus::NeedMore; off += 1024) st = lp.feed(big.data() + off, std::min<size_t>(1024, big.size() - off));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(st == ReadStatus::Ok);
    CHECK(ms < 5000);   // quadratic rescanning would take minutes
    std::cout << "[PASS] Chunked responses: trailers, bytes after the end, every split point, linear-time scan (" << static_cast<int>(ms) << " ms for 20 MB)\n";
}

void test_strict_status_lines() {
    auto st = [](const std::string& l) { int s = 0; return parse_status_line(l.data(), l.size(), s) ? s : -1; };
    CHECK(st("HTTP/1.1 200 OK") == 200 && st("HTTP/1.0 404 Not Found") == 404 && st("HTTP/1.1 200") == 200 && st("HTTP/1.1 599 x") == 599);
    CHECK(st("HTTP/1.1 100 Continue") == 100 && st("HTTP/1.9 204 ") == 204);
    for (const char* bad : {"HTTP/1.1 2000 OK", "HTTP/1HTTP/1.1 200 OK", "HTTP/1.1  200 OK", "HTTP/2 200 OK", "HTTP/1.1 20 OK", "HTTP/1.1 abc OK",
                            "HTTP/1.1 099 x", "HTTP/1.1 600 x", "HTTP/1.1 200OK", "http/1.1 200 OK", "HTTP/1.x 200 OK", "HTTP/1.1", "", "HTTP/1.1 -20 x", "HTTP/1.1 +200 x"})
        CHECK(st(bad) == -1);
    // the parsers use it: a lenient reading of "2000" as "200" is a response-splitting / smuggling hazard
    ResponseParser p(false, 1 << 20);
    std::string w = "HTTP/1.1 2000 OK\r\nContent-Length: 0\r\n\r\n";
    CHECK(p.feed(w.data(), w.size()) == ReadStatus::Error);
    ResponseParser q(false, 1 << 20);
    w = "HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n";   // reason phrase is optional
    CHECK(q.feed(w.data(), w.size()) == ReadStatus::Ok && q.data.status == 200);
    std::cout << "[PASS] Strict status-line parsing (rejects 2000 / HTTP/1HTTP/1.1 / HTTP/2 / signs / short codes)\n";
}

void test_address_failover_pool_and_fd_budget() {
    // resolve_all: literal -> exactly one; names -> at least one, no duplicates, capped
    std::vector<SockAddr> v;
    CHECK(resolve_all("127.0.0.1", 80, v) && v.size() == 1 && sockaddr_get_port(v[0].ss) == 80);
    CHECK(resolve_all("[::1]", 81, v) && v.size() == 1 && v[0].family() == AF_INET6);
    CHECK(resolve_all("localhost", 82, v) && !v.empty() && v.size() <= 4);
    for (size_t i = 0; i < v.size(); ++i)
        for (size_t j = i + 1; j < v.size(); ++j)
            CHECK(!(v[i].family() == v[j].family() && addr_to_string_raw(v[i]) == addr_to_string_raw(v[j])));
    CHECK(!resolve_all("no-such-host.invalid", 80, v) && v.empty());

    // connect_addrs: a refused first address (closed port) fails over to the second one
    int lfd = listen_on("127.0.0.1", 0, 8);
    CHECK(lfd >= 0);
    SockAddr good, dead;
    socklen_t sl = sizeof(good.ss);
    CHECK(::getsockname(lfd, reinterpret_cast<sockaddr*>(&good.ss), &sl) == 0);
    good.len = sl;
    int tmp = listen_on("127.0.0.1", 0, 1);              // grab a free port, then close it again -> nothing listens there
    dead = good;
    sl = sizeof(dead.ss);
    CHECK(::getsockname(tmp, reinterpret_cast<sockaddr*>(&dead.ss), &sl) == 0);
    ::close(tmp);
    bool timed_out = false;
    size_t used = 99;
    int fd = connect_addrs({dead, good}, 2000, timed_out, &used);
    CHECK(fd >= 0 && used == 1 && !timed_out);
    if (fd >= 0) ::close(fd);
    CHECK(connect_addrs({dead, dead}, 500, timed_out) < 0);
    CHECK(connect_addrs({}, 500, timed_out) < 0);
    ::close(lfd);

    // pool limits: per key and total, 0 disables pooling; the count is exposed
    auto mk = [](const std::string& key) {
        int sv[2];
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        ::close(sv[1]);
        auto c = std::make_unique<Connection>(sv[0], 0, ConnState::IDLE);
        c->key = key;
        return c;
    };
    {
        UpstreamPool pool(30000, 2, 3);
        for (int i = 0; i < 5; ++i) pool.release(mk("a"));
        CHECK(pool.size() == 2);                         // per-key cap
        for (int i = 0; i < 5; ++i) pool.release(mk("b"));
        CHECK(pool.size() == 3);                         // total cap
    }
    {
        UpstreamPool off(30000, 0, 100);
        off.release(mk("a"));
        CHECK(off.size() == 0);
    }

    // fd budget: nothing changes with plenty of descriptors, limits are lowered with a small RLIMIT_NOFILE
    GatewayConfig big;
    CHECK(apply_fd_budget(big, 1048576).empty() && big.max_client_connections == 10000 && big.pool_max_idle_total == 1024);
    GatewayConfig small;
    auto notes = apply_fd_budget(small, 1024);
    CHECK(notes.size() == 2 && small.pool_max_idle_total == 240 && small.max_client_connections == 360);
    CHECK(small.pool_max_idle_per_proxy <= small.pool_max_idle_total);
    CHECK(small.max_client_connections + small.pool_max_idle_total <= 1024 - kFdReserve);
    GatewayConfig tiny;
    CHECK(!apply_fd_budget(tiny, 40).empty() && tiny.max_client_connections >= 1);

    // defaults and config keys
    GatewayConfig d;
    CHECK(d.listen_host == "127.0.0.1" && d.pool_max_idle_per_proxy == 64 && d.pool_max_idle_total == 1024);
    GatewayConfig c;
    load_config_from_string(R"({"proxies":[{"host":"127.0.0.1","port":1}],"limits":{"pool_max_idle_per_proxy":7,"pool_max_idle_total":9}})", c);
    CHECK(c.pool_max_idle_per_proxy == 7 && c.pool_max_idle_total == 9);
    GatewayConfig bad;
    bool threw = false;
    try { load_config_from_string(R"({"proxies":[{"host":"127.0.0.1","port":1}],"limits":{"pool_max_idle_total":-1}})", bad); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // exposed proxy listener is flagged (open proxy), loopback is not
    GatewayConfig w;
    w.proxies.push_back({"127.0.0.1", 1});
    auto flagged = [](const GatewayConfig& g) { for (auto& s : security_warnings(g)) if (s.find("open proxy") != std::string::npos) return true; return false; };
    CHECK(!flagged(w));
    w.listen_host = "0.0.0.0";
    CHECK(flagged(w));
    w.listen_host = "::";
    CHECK(flagged(w));
    w.listen_host = "::1";
    CHECK(!flagged(w));
    // pasted hosts: surrounding whitespace is trimmed, junk and "host:port" are rejected with a clear message
    {
        GatewayConfig h1;
        load_config_from_string("{\"proxies\":[{\"host\":\" 89.251.21.134\\t\\n\",\"port\":8080}]}", h1);
        CHECK(h1.proxies.size() == 1 && h1.proxies[0].host == "89.251.21.134");
        auto rejects = [](const std::string& host) {
            GatewayConfig x;
            try { load_config_from_string("{\"proxies\":[{\"host\":\"" + host + "\",\"port\":8080}]}", x); } catch (const std::exception&) { return true; }
            return false;
        };
        CHECK(rejects("1.2.3\\t4") && rejects("a b.example") && rejects("89.251.21.134:8080") && rejects("http://x.example")
              && rejects("x\\u0001y") && rejects("evil\\\"host") && rejects("   "));
        GatewayConfig h2;
        load_config_from_string(R"({"proxies":[{"host":"[::1]","port":1},{"host":"::1","port":2},{"host":"my-proxy_1.example.com","port":3}]})", h2);
        CHECK(h2.proxies.size() == 3);
    }
    std::cout << "[PASS] Address fail-over, pool limits, fd budget, loopback default, open-proxy warning\n";
}

int main() {
    test_scheduler_and_token_bucket();
    test_scheduler_blocking_expiry_and_shutdown();
    test_circuit_breaker_and_routing();
    test_lowest_latency_concurrent_no_deadlock();
    test_aimd_controller();
    test_json_parser();
    test_config_loading();
    test_http_request_parser();
    test_url_and_authority();
    test_response_reader_and_escapes();
    test_incremental_response_parser();
    test_scheduler_expire_without_pop();
    test_socks5_client_helpers();
    test_ip_addresses_and_secrets();
    test_network_classification();
    test_chunked_trailers_and_trailing_bytes();
    test_strict_status_lines();
    test_address_failover_pool_and_fd_budget();
    std::cout << "All unit tests passed successfully.\n";
    return 0;
}
