// PrusaSlicer-derived API retained for legacy UI callers during the SLA split.
// This implementation has no transport, DNS, TLS, retries, or file access.
#include "Http.hpp"
#include <atomic>
#include <ostream>
#include <thread>
#include <utility>

namespace Slic3r {
struct Http::priv {
  ErrorFn error;
  std::atomic<bool> cancelled{false};
};

Http::Http(const std::string &) : p(std::make_unique<priv>()) {}
Http::Http(Http &&other) : p(std::move(other.p)) {}
Http::~Http() = default;
Http Http::get(std::string url) { return Http(url); }
Http Http::post(std::string url) { return Http(url); }
Http Http::put(std::string url) { return Http(url); }

const HttpRetryOpt &HttpRetryOpt::no_retry() {
  static const HttpRetryOpt options{std::chrono::milliseconds(0),
                                    std::chrono::milliseconds(0), 0};
  return options;
}
const HttpRetryOpt &HttpRetryOpt::default_retry() { return no_retry(); }

// Return an explicit failure through the normal callback contract. Never call
// on_complete: a legacy updater must not mistake a disabled request for
// success.
void Http::perform_sync(const HttpRetryOpt &) {
  if (p && !p->cancelled && p->error)
    p->error({}, "Network access is disabled in this offline SLA application",
             0);
}
Http::Ptr Http::perform(const HttpRetryOpt &) {
  auto self = std::make_shared<Http>(std::move(*this));
  std::thread([self] { self->perform_sync(); }).detach();
  return self;
}
void Http::cancel() {
  if (p)
    p->cancelled = true;
}

Http &Http::on_error(ErrorFn callback) {
  if (p)
    p->error = std::move(callback);
  return *this;
}
Http &Http::on_complete(CompleteFn) { return *this; }
Http &Http::on_progress(ProgressFn) { return *this; }
Http &Http::on_ip_resolve(IPResolveFn) { return *this; }
Http &Http::on_retry(RetryFn) { return *this; }
Http &Http::on_headers(HeadersFn) { return *this; }
Http &Http::timeout_connect(long) { return *this; }
Http &Http::timeout_max(long) { return *this; }
Http &Http::size_limit(size_t) { return *this; }
Http &Http::set_range(const std::string &) { return *this; }
Http &Http::header(std::string, const std::string &) { return *this; }
Http &Http::remove_header(std::string) { return *this; }
Http &Http::auth_digest(const std::string &, const std::string &) {
  return *this;
}
Http &Http::auth_basic(const std::string &, const std::string &) {
  return *this;
}
Http &Http::ca_file(const std::string &) { return *this; }
Http &Http::form_add(const std::string &, const std::string &) { return *this; }
Http &Http::form_add_file(const std::string &,
                          const boost::filesystem::path &) {
  return *this;
}
Http &Http::form_add_file(const std::string &, const boost::filesystem::path &,
                          const std::string &) {
  return *this;
}
Http &Http::set_post_body(const boost::filesystem::path &) { return *this; }
Http &Http::set_post_body(const std::string &) { return *this; }
Http &Http::set_put_body(const boost::filesystem::path &) { return *this; }
Http &Http::cookie_file(const std::string &) { return *this; }
Http &Http::cookie_jar(const std::string &) { return *this; }
Http &Http::set_referer(const std::string &) { return *this; }
#ifdef WIN32
Http &Http::ssl_revoke_best_effort(bool) { return *this; }
#endif
bool Http::ca_file_supported() { return false; }
std::string Http::tls_global_init() { return {}; }
std::string Http::tls_system_cert_store() { return {}; }

// Retained as a pure string operation; UTF-8 is escaped byte by byte.
std::string Http::url_encode(const std::string &input) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char byte : input) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
        byte == '.' || byte == '~')
      result += char(byte);
    else {
      result += '%';
      result += hex[byte >> 4];
      result += hex[byte & 15];
    }
  }
  return result;
}
std::ostream &operator<<(std::ostream &stream, const Http::Progress &progress) {
  return stream << "Http::Progress(dltotal=" << progress.dltotal
                << ",dlnow=" << progress.dlnow
                << ",ultotal=" << progress.ultotal
                << ",ulnow=" << progress.ulnow << ')';
}
} // namespace Slic3r
