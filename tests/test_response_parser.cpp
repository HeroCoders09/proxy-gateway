#include <random>

#include "catch_amalgamated.hpp"
#include "common/http_util.hpp"
#include "net/http_response_parser.hpp"

using namespace pg;

constexpr size_t kMax = 1 << 20;

TEST_CASE("Content-Length response") {
  auto r = parse_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-A: b\r\n\r\nhello", false, false, kMax);
  REQUIRE(r.status == RespStatus::Complete);
  REQUIRE(r.response.status == 200);
  REQUIRE(r.response.reason == "OK");
  REQUIRE(r.response.body == "hello");
  REQUIRE(*find_header(r.response.headers, "x-a") == "b");
}

TEST_CASE("Content-Length: extra bytes beyond the body are ignored") {
  auto r = parse_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhiEXTRA", false, false, kMax);
  REQUIRE(r.status == RespStatus::Complete);
  REQUIRE(r.response.body == "hi");
}

TEST_CASE("partial body needs more; eof makes it malformed") {
  std::string partial = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
  REQUIRE(parse_response(partial, false, false, kMax).status == RespStatus::NeedMore);
  REQUIRE(parse_response(partial, false, true, kMax).status == RespStatus::Malformed);
  REQUIRE(parse_response("HTTP/1.1 200 OK\r\nCont", false, false, kMax).status == RespStatus::NeedMore);
  REQUIRE(parse_response("HTTP/1.1 200 OK\r\nCont", false, true, kMax).status == RespStatus::Malformed);
  REQUIRE(parse_response("", false, false, kMax).status == RespStatus::NeedMore);
}

TEST_CASE("chunked response, with extensions and trailers") {
  std::string body = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5\r\nhello\r\n"
                     "6;ext=1\r\n world\r\n"
                     "0\r\nTrailer-X: y\r\n\r\n";
  auto r = parse_response(body, false, false, kMax);
  REQUIRE(r.status == RespStatus::Complete);
  REQUIRE(r.response.body == "hello world");
}

TEST_CASE("chunked: every prefix is NeedMore until the end") {
  std::string full = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  for (size_t n = 0; n < full.size(); ++n) {
    auto r = parse_response(std::string_view(full).substr(0, n), false, false, kMax);
    REQUIRE(r.status == RespStatus::NeedMore);
  }
  REQUIRE(parse_response(full, false, false, kMax).status == RespStatus::Complete);
}

TEST_CASE("chunked: bad sizes and missing CRLF are malformed") {
  auto mk = [](const std::string& chunks) { return "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunks; };
  REQUIRE(parse_response(mk("zz\r\nabc\r\n0\r\n\r\n"), false, false, kMax).status == RespStatus::Malformed);
  REQUIRE(parse_response(mk("3\r\nabcXX0\r\n\r\n"), false, false, kMax).status == RespStatus::Malformed);
  REQUIRE(parse_response(mk("\r\nabc\r\n"), false, false, kMax).status == RespStatus::Malformed);
  REQUIRE(parse_response(mk("FFFFFFFFFFFFFFFFFF\r\n"), false, false, kMax).status == RespStatus::Malformed);
}

TEST_CASE("unsupported transfer coding is rejected") {
  auto r = parse_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n", false, false, kMax);
  REQUIRE(r.status == RespStatus::Malformed);
}

TEST_CASE("read-until-close body") {
  std::string r1 = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nstreamed body";
  REQUIRE(parse_response(r1, false, false, kMax).status == RespStatus::NeedMore);
  auto r = parse_response(r1, false, true, kMax);
  REQUIRE(r.status == RespStatus::Complete);
  REQUIRE(r.response.body == "streamed body");
  REQUIRE(r.response.upstream_wants_close);
}

TEST_CASE("no-body statuses and HEAD") {
  REQUIRE(parse_response("HTTP/1.1 204 No Content\r\n\r\n", false, false, kMax).status == RespStatus::Complete);
  REQUIRE(parse_response("HTTP/1.1 304 Not Modified\r\nContent-Length: 50\r\n\r\n", false, false, kMax).status == RespStatus::Complete);
  auto h = parse_response("HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n", true, false, kMax);
  REQUIRE(h.status == RespStatus::Complete);
  REQUIRE(h.response.body.empty());
}

TEST_CASE("interim 100-continue is skipped") {
  auto r = parse_response("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok", false, false, kMax);
  REQUIRE(r.status == RespStatus::Complete);
  REQUIRE(r.response.status == 201);
  REQUIRE(r.response.body == "ok");
}

TEST_CASE("5xx statuses from the proxy are parsed normally") {
  auto r = parse_response("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 4\r\n\r\nbusy", false, false, kMax);
  REQUIRE(r.status == RespStatus::Complete);
  REQUIRE(r.response.status == 503);
}

TEST_CASE("malformed responses") {
  const char* bad[] = {
      "garbage\r\n\r\n",
      "HTTP/2 200 OK\r\n\r\n",
      "HTTP/1.1 20 OK\r\n\r\n",
      "HTTP/1.1 abc OK\r\n\r\n",
      "HTTP/1.1 099 Low\r\n\r\n",
      "HTTP/1.1 200 OK\r\nbadheader\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
  };
  for (const char* b : bad) {
    INFO(b);
    REQUIRE(parse_response(b, false, false, kMax).status == RespStatus::Malformed);
  }
}

TEST_CASE("size cap") {
  std::string big = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n" + std::string(100, 'x');
  REQUIRE(parse_response(big, false, false, 50).status == RespStatus::TooLarge);
  REQUIRE(parse_response("HTTP/1.1 200 OK\r\nContent-Length: 999999\r\n\r\nx", false, false, 1000).status == RespStatus::TooLarge);
}

TEST_CASE("random bytes never crash the response parser") {
  std::mt19937 rng(777);
  for (int i = 0; i < 3000; ++i) {
    std::string s(rng() % 300, '\0');
    for (char& c : s) c = static_cast<char>(rng() % 256);
    (void)parse_response(s, false, i % 2 == 0, kMax);
  }
  std::string valid = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n0\r\n\r\n";
  for (int i = 0; i < 3000; ++i) {
    std::string s = valid;
    for (int k = 0; k < 3; ++k) s[rng() % s.size()] = static_cast<char>(rng() % 256);
    if (rng() % 3 == 0) s.resize(rng() % s.size());
    (void)parse_response(s, false, i % 2 == 0, kMax);
  }
  SUCCEED();
}
