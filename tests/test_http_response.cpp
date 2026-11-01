#include "catch_amalgamated.hpp"
#include "ingress/http_response.hpp"

using namespace pg;

TEST_CASE("serialize adds Content-Length and Connection: close") {
  HttpResponse r;
  r.status = 200;
  r.headers = {{"Content-Type", "text/plain"}, {"Content-Length", "999"}, {"Connection", "keep-alive"},
               {"Transfer-Encoding", "chunked"}};
  r.body = "hello";
  std::string s = serialize_response(r);
  REQUIRE(s.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
  REQUIRE(s.find("Content-Length: 5\r\n") != std::string::npos);
  REQUIRE(s.find("Content-Length: 999") == std::string::npos);
  REQUIRE(s.find("keep-alive") == std::string::npos);
  REQUIRE(s.find("chunked") == std::string::npos);
  REQUIRE(s.find("Connection: close\r\n") != std::string::npos);
  REQUIRE(s.size() >= 5);
  REQUIRE(s.substr(s.size() - 5) == "hello");
}

TEST_CASE("HEAD keeps upstream Content-Length and sends no body") {
  HttpResponse r;
  r.headers = {{"Content-Length", "42"}};
  std::string s = serialize_response(r, true);
  REQUIRE(s.find("Content-Length: 42\r\n") != std::string::npos);
  REQUIRE(s.substr(s.size() - 4) == "\r\n\r\n");
}

TEST_CASE("header injection characters are dropped") {
  HttpResponse r;
  r.headers = {{"X-Evil", "a\r\nSet-Cookie: x=1"}, {"X-Ok", "fine"}};
  std::string s = serialize_response(r);
  REQUIRE(s.find("Set-Cookie") == std::string::npos);
  REQUIRE(s.find("X-Ok: fine") != std::string::npos);
}

TEST_CASE("error responses use the right status lines") {
  for (int code : {400, 429, 502, 503, 504}) {
    auto r = make_error_response(code, "detail");
    std::string s = serialize_response(r);
    REQUIRE(s.rfind("HTTP/1.1 " + std::to_string(code) + " ", 0) == 0);
    REQUIRE(r.body.find("detail") != std::string::npos);
  }
  REQUIRE(std::string(reason_phrase(400)) == "Bad Request");
  REQUIRE(std::string(reason_phrase(504)) == "Gateway Timeout");
}
