#include <random>

#include "catch_amalgamated.hpp"
#include "ingress/http_parser.hpp"

using namespace pg;

static const ParseLimits kLimits{};

TEST_CASE("valid GET with absolute URL") {
  auto r = parse_request("GET http://test.example/products/123 HTTP/1.1\r\nHost: test.example\r\nX-A: b\r\n\r\n", kLimits);
  REQUIRE(r.status == ParseStatus::Complete);
  REQUIRE(r.request.method == "GET");
  REQUIRE(r.request.host == "test.example");
  REQUIRE(r.request.port == 80);
  REQUIRE(r.request.path == "/products/123");
  REQUIRE(r.request.url == "http://test.example/products/123");
  REQUIRE(r.request.headers.size() == 2);
  REQUIRE(r.request.body.empty());
}

TEST_CASE("host is lower-cased, port and query parsed, fragment dropped") {
  auto r = parse_request("GET http://Test.EXAMPLE:8081/a/b?x=1&y=2#frag HTTP/1.1\r\n\r\n", kLimits);
  REQUIRE(r.status == ParseStatus::Complete);
  REQUIRE(r.request.host == "test.example");
  REQUIRE(r.request.port == 8081);
  REQUIRE(r.request.path == "/a/b?x=1&y=2");
}

TEST_CASE("bare host and bare query normalise to a path") {
  auto a = parse_request("GET http://h.example HTTP/1.1\r\n\r\n", kLimits);
  REQUIRE(a.status == ParseStatus::Complete);
  REQUIRE(a.request.path == "/");
  auto b = parse_request("GET http://h.example?q=1 HTTP/1.1\r\n\r\n", kLimits);
  REQUIRE(b.status == ParseStatus::Complete);
  REQUIRE(b.request.path == "/?q=1");
}

TEST_CASE("HTTP/1.0 is accepted, other versions are not") {
  REQUIRE(parse_request("GET http://h/ HTTP/1.0\r\n\r\n", kLimits).status == ParseStatus::Complete);
  REQUIRE(parse_request("GET http://h/ HTTP/2.0\r\n\r\n", kLimits).status == ParseStatus::Malformed);
  REQUIRE(parse_request("GET http://h/ HTTP/1.2\r\n\r\n", kLimits).status == ParseStatus::Malformed);
}

TEST_CASE("incomplete input asks for more") {
  REQUIRE(parse_request("", kLimits).status == ParseStatus::NeedMore);
  REQUIRE(parse_request("GET http://h/ HTTP/1.1\r\nHost: h\r\n", kLimits).status == ParseStatus::NeedMore);
  REQUIRE(parse_request("GET http://h/ HT", kLimits).status == ParseStatus::NeedMore);
}

TEST_CASE("incremental parse gives the same result byte by byte") {
  std::string full = "POST http://h.example/p HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
  for (size_t n = 0; n < full.size(); ++n) {
    REQUIRE(parse_request(std::string_view(full).substr(0, n), kLimits).status == ParseStatus::NeedMore);
  }
  auto r = parse_request(full, kLimits);
  REQUIRE(r.status == ParseStatus::Complete);
  REQUIRE(r.request.body == "hello");
  REQUIRE(r.consumed == full.size());
}

TEST_CASE("malformed request lines") {
  const char* bad[] = {
      "garbage\r\n\r\n",
      "GET\r\n\r\n",
      "GET http://h/\r\n\r\n",
      "GET  http://h/ HTTP/1.1\r\n\r\n",
      "GET /relative/path HTTP/1.1\r\n\r\n",
      "get http://h/ HTTP/1.1\r\n\r\n",      // lower-case method is not in our allowed set
      "BREW http://h/ HTTP/1.1\r\n\r\n",
      "CONNECT h:443 HTTP/1.1\r\n\r\n",
      "GET ftp://h/ HTTP/1.1\r\n\r\n",
      "GET https://h/ HTTP/1.1\r\n\r\n",
      "GET http:// HTTP/1.1\r\n\r\n",
      "GET http://:80/ HTTP/1.1\r\n\r\n",
      "GET http://h:0/ HTTP/1.1\r\n\r\n",
      "GET http://h:99999/ HTTP/1.1\r\n\r\n",
      "GET http://h:abc/ HTTP/1.1\r\n\r\n",
      "GET http://h:/ HTTP/1.1\r\n\r\n",
      "GET http://user:pw@h/ HTTP/1.1\r\n\r\n",
      "GET http://[::1]/ HTTP/1.1\r\n\r\n",
      "GET http://h_$/ HTTP/1.1\r\n\r\n",
      "GET http://h/ HTTP/1.1\r\nNoColonHeader\r\n\r\n",
      "GET http://h/ HTTP/1.1\r\n: empty-name\r\n\r\n",
      "GET http://h/ HTTP/1.1\r\n folded: x\r\n\r\n",
      "GET http://h/ HTTP/1.1\r\nBad Name: x\r\n\r\n",
      "GET http://h/ HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
      "POST http://h/ HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
      "POST http://h/ HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
      "POST http://h/ HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx",
  };
  for (const char* b : bad) {
    INFO(b);
    auto r = parse_request(b, kLimits);
    REQUIRE((r.status == ParseStatus::Malformed || r.status == ParseStatus::TooLarge));
    REQUIRE_FALSE(r.reason.empty());
  }
}

TEST_CASE("duplicate identical Content-Length is accepted") {
  auto r = parse_request("POST http://h/ HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nok", kLimits);
  REQUIRE(r.status == ParseStatus::Complete);
  REQUIRE(r.request.body == "ok");
}

TEST_CASE("size limits") {
  ParseLimits small;
  small.max_header_bytes = 300;
  small.max_body_bytes = 10;
  std::string huge_header = "GET http://h/ HTTP/1.1\r\nX: " + std::string(400, 'a') + "\r\n\r\n";
  REQUIRE(parse_request(huge_header, small).status == ParseStatus::TooLarge);
  // No terminator yet but already past the limit: reject instead of buffering forever.
  REQUIRE(parse_request(std::string(400, 'a'), small).status == ParseStatus::TooLarge);
  REQUIRE(parse_request("POST http://h/ HTTP/1.1\r\nContent-Length: 11\r\n\r\n", small).status == ParseStatus::TooLarge);
  REQUIRE(parse_request("POST http://h/ HTTP/1.1\r\nContent-Length: 10\r\n\r\n", small).status == ParseStatus::NeedMore);
  REQUIRE(parse_request("POST http://h/ HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n", small).status ==
          ParseStatus::Malformed);
}

TEST_CASE("pipelined bytes after the request are not consumed") {
  std::string one = "GET http://h/ HTTP/1.1\r\n\r\n";
  auto r = parse_request(one + "GET http://h/2 HTTP/1.1\r\n\r\n", kLimits);
  REQUIRE(r.status == ParseStatus::Complete);
  REQUIRE(r.consumed == one.size());
}

TEST_CASE("random garbage never crashes the parser") {
  std::mt19937 rng(12345);
  for (int i = 0; i < 3000; ++i) {
    std::string s(rng() % 300, '\0');
    for (char& c : s) c = static_cast<char>(rng() % 256);
    (void)parse_request(s, kLimits);
  }
  // Mutations of a valid request.
  std::string valid = "POST http://h.example:81/p?q=1 HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc";
  for (int i = 0; i < 3000; ++i) {
    std::string s = valid;
    for (int k = 0; k < 3; ++k) s[rng() % s.size()] = static_cast<char>(rng() % 256);
    if (rng() % 3 == 0) s.resize(rng() % s.size());
    (void)parse_request(s, kLimits);
  }
  SUCCEED();
}
