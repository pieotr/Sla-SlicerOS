#include "slic3r/Utils/Http.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <future>
#include <tuple>

using namespace Slic3r;

TEST_CASE("Offline HTTP rejects all transports before file or network access",
          "[sla_offline]") {
  const auto url =
      GENERATE("https://127.0.0.1/", "http://invalid.invalid/",
               "ftp://invalid.invalid/", "file:///nonexistent/sla-file");
  bool completed = false, progressed = false, resolved = false, retried = false,
       headers = false;
  unsigned errors = 0;
  auto request = Http::post(url);
  request
      .form_add_file("body", boost::filesystem::path("/nonexistent/sla-input"))
      .set_post_body(boost::filesystem::path("/nonexistent/sla-body"))
      .on_complete([&](std::string, unsigned) { completed = true; })
      .on_progress([&](Http::Progress, bool &) { progressed = true; })
      .on_ip_resolve([&](std::string) { resolved = true; })
      .on_retry([&](int, unsigned) {
        retried = true;
        return true;
      })
      .on_headers([&](const std::string &) { headers = true; })
      .on_error([&](std::string body, std::string message, unsigned status) {
        ++errors;
        REQUIRE(body.empty());
        REQUIRE(status == 0);
        REQUIRE(message.find("disabled") != std::string::npos);
      });
  request.perform_sync(HttpRetryOpt::default_retry());
  REQUIRE(errors == 1);
  REQUIRE_FALSE(completed);
  REQUIRE_FALSE(progressed);
  REQUIRE_FALSE(resolved);
  REQUIRE_FALSE(retried);
  REQUIRE_FALSE(headers);
  request.cancel();
  request.perform_sync();
  REQUIRE(errors == 1);
  REQUIRE_FALSE(Http::ca_file_supported());
  REQUIRE(Http::tls_global_init().empty());
  REQUIRE(Http::tls_system_cert_store().empty());
}

TEST_CASE("Offline asynchronous requests report failure and pure URL escaping "
          "is preserved",
          "[sla_offline]") {
  std::promise<std::tuple<std::string, std::string, unsigned>> result;
  auto future = result.get_future();
  auto request = Http::put("https://invalid.invalid/");
  request.on_error([&](std::string body, std::string error, unsigned status) {
    result.set_value({body, error, status});
  });
  auto active = request.perform();
  REQUIRE(future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
  const auto [body, error, status] = future.get();
  REQUIRE(body.empty());
  REQUIRE(status == 0);
  REQUIRE(error.find("disabled") != std::string::npos);
  REQUIRE(Http::url_encode("abc-_.~ /?%+Ż") == "abc-_.~%20%2F%3F%25%2B%C5%BB");
}
