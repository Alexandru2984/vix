#include <utility>

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "GameServer.hpp"
#include "Utils.hpp"
#include "Validation.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace
{
  constexpr std::size_t maxWsOutboxMessages = 128;
  constexpr std::size_t maxWsOutboxBytes = 1024 * 1024;
  constexpr std::uint64_t maxHttpRequestBodyBytes = 8 * 1024;
  constexpr std::uint32_t maxHttpRequestHeaderBytes = 16 * 1024;
  constexpr std::uintmax_t maxStaticFileBytes = 2 * 1024 * 1024;
  constexpr std::uintmax_t maxDotEnvBytes = 64 * 1024;
  constexpr std::size_t maxDotEnvLines = 512;
  constexpr std::size_t maxDotEnvLineBytes = 4 * 1024;
  constexpr double httpRateLimitBurst = 180.0;
  constexpr double httpRateLimitRefillPerSecond = 12.0;
  constexpr std::chrono::minutes httpRateLimitIdleTtl{10};

  struct AppConfig
  {
    std::set<std::string> allowedOrigins;
    bool allowMissingOrigin{false};
  };

  struct HttpRateLimitBucket
  {
    double tokens{0.0};
    std::chrono::steady_clock::time_point lastRefill{std::chrono::steady_clock::now()};
  };

  class HttpRateLimiter
  {
  public:
    void configure(double burst, double refillPerSecond, std::vector<std::string> benchmarkIps = {}, double benchmarkBurst = 0.0, double benchmarkRefillPerSecond = 0.0)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      burst_ = std::max(1.0, burst);
      refillPerSecond_ = std::max(0.1, refillPerSecond);
      benchmarkIps_ = std::move(benchmarkIps);
      benchmarkBurst_ = std::max(burst_, benchmarkBurst > 0.0 ? benchmarkBurst : burst_);
      benchmarkRefillPerSecond_ = std::max(refillPerSecond_, benchmarkRefillPerSecond > 0.0 ? benchmarkRefillPerSecond : refillPerSecond_);
      buckets_.clear();
    }

    bool allow(const std::string &key)
    {
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> lock(mutex_);
      auto &bucket = buckets_[key.empty() ? "unknown" : key];
      const bool benchmark = std::find(benchmarkIps_.begin(), benchmarkIps_.end(), key) != benchmarkIps_.end();
      const double burst = benchmark ? benchmarkBurst_ : burst_;
      const double refillPerSecond = benchmark ? benchmarkRefillPerSecond_ : refillPerSecond_;
      const double elapsed = std::chrono::duration<double>(now - bucket.lastRefill).count();
      if (bucket.tokens <= 0.0)
      {
        bucket.tokens = burst;
      }
      bucket.tokens = std::min(burst, bucket.tokens + elapsed * refillPerSecond);
      bucket.lastRefill = now;
      if (bucket.tokens < 1.0)
      {
        cleanupLocked(now);
        return false;
      }
      bucket.tokens -= 1.0;
      cleanupLocked(now);
      return true;
    }

  private:
    void cleanupLocked(std::chrono::steady_clock::time_point now)
    {
      if (buckets_.size() < 4096)
      {
        return;
      }
      for (auto it = buckets_.begin(); it != buckets_.end();)
      {
        if (now - it->second.lastRefill > httpRateLimitIdleTtl)
        {
          it = buckets_.erase(it);
        }
        else
        {
          ++it;
        }
      }
    }

    std::mutex mutex_;
    std::unordered_map<std::string, HttpRateLimitBucket> buckets_;
    double burst_{httpRateLimitBurst};
    double refillPerSecond_{httpRateLimitRefillPerSecond};
    std::vector<std::string> benchmarkIps_;
    double benchmarkBurst_{httpRateLimitBurst};
    double benchmarkRefillPerSecond_{httpRateLimitRefillPerSecond};
  };

  HttpRateLimiter gHttpRateLimiter;
  std::atomic<std::uint64_t> gHttpResponsesTotal{0};
  std::atomic<std::uint64_t> gHttpDynamicRequestsTotal{0};
  std::atomic<std::uint64_t> gHttpRateLimitRejectionsTotal{0};
  std::atomic<std::uint64_t> gHttpResponses1xx{0};
  std::atomic<std::uint64_t> gHttpResponses2xx{0};
  std::atomic<std::uint64_t> gHttpResponses3xx{0};
  std::atomic<std::uint64_t> gHttpResponses4xx{0};
  std::atomic<std::uint64_t> gHttpResponses5xx{0};

  std::string trim(std::string value)
  {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
      value.pop_back();
    return value;
  }

  std::string originFromUrl(const std::string &url)
  {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
      return {};

    const auto authorityStart = schemeEnd + 3;
    auto authorityEnd = url.find_first_of("/?#", authorityStart);
    if (authorityEnd == std::string::npos)
      authorityEnd = url.size();

    if (authorityEnd <= authorityStart)
      return {};

    return url.substr(0, authorityEnd);
  }

  std::set<std::string> parseOriginList(const std::string &value)
  {
    std::set<std::string> origins;
    std::size_t start = 0;
    while (start < value.size())
    {
      const auto comma = value.find(',', start);
      const auto end = comma == std::string::npos ? value.size() : comma;
      std::string item = trim(value.substr(start, end - start));
      if (!item.empty())
      {
        if (const std::string origin = originFromUrl(item); !origin.empty())
          item = origin;
        origins.insert(std::move(item));
      }
      if (comma == std::string::npos)
        break;
      start = comma + 1;
    }
    return origins;
  }

  std::vector<std::string> parseCsvList(const std::string &value)
  {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start < value.size())
    {
      const auto comma = value.find(',', start);
      const auto end = comma == std::string::npos ? value.size() : comma;
      std::string item = trim(value.substr(start, end - start));
      if (!item.empty())
      {
        items.push_back(std::move(item));
      }
      if (comma == std::string::npos)
        break;
      start = comma + 1;
    }
    return items;
  }

  int hexValue(char c)
  {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return 10 + c - 'a';
    if (c >= 'A' && c <= 'F')
      return 10 + c - 'A';
    return -1;
  }

  std::string urlDecode(std::string_view value)
  {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i)
    {
      if (value[i] == '+')
      {
        out.push_back(' ');
      }
      else if (value[i] == '%' && i + 2 < value.size())
      {
        const int hi = hexValue(value[i + 1]);
        const int lo = hexValue(value[i + 2]);
        if (hi >= 0 && lo >= 0)
        {
          out.push_back(static_cast<char>((hi << 4) | lo));
          i += 2;
        }
      }
      else
      {
        out.push_back(static_cast<char>(value[i]));
      }
    }
    return out;
  }

  std::string queryParam(const std::string &query, const std::string &key)
  {
    std::size_t start = 0;
    while (start < query.size())
    {
      const auto amp = query.find('&', start);
      const auto end = amp == std::string::npos ? query.size() : amp;
      const auto eq = query.find('=', start);
      const std::size_t nameEnd = eq == std::string::npos || eq > end ? end : eq;
      if (urlDecode(std::string_view(query).substr(start, nameEnd - start)) == key)
      {
        if (eq == std::string::npos || eq > end)
          return {};
        return urlDecode(std::string_view(query).substr(eq + 1, end - eq - 1));
      }
      if (amp == std::string::npos)
        break;
      start = amp + 1;
    }
    return {};
  }

  std::unordered_map<std::string, std::string> readDotEnv(const std::filesystem::path &path)
  {
    std::unordered_map<std::string, std::string> values;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
      return values;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && size > maxDotEnvBytes)
      throw std::runtime_error(".env exceeds maximum supported size");

    std::ifstream in(path);
    std::string line;
    std::size_t lineCount = 0;
    while (std::getline(in, line))
    {
      ++lineCount;
      if (lineCount > maxDotEnvLines)
        throw std::runtime_error(".env has too many lines");
      if (line.size() > maxDotEnvLineBytes)
        throw std::runtime_error(".env line exceeds maximum supported size");
      line = trim(line);
      if (line.empty() || line.front() == '#')
        continue;
      const auto pos = line.find('=');
      if (pos == std::string::npos)
        continue;
      values[trim(line.substr(0, pos))] = trim(line.substr(pos + 1));
    }
    return values;
  }

  std::string envString(const std::unordered_map<std::string, std::string> &fileEnv,
                        const char *key,
                        const std::string &fallback)
  {
    if (const char *fromEnv = std::getenv(key); fromEnv)
      return fromEnv;
    auto it = fileEnv.find(key);
    return it == fileEnv.end() || it->second.empty() ? fallback : it->second;
  }

  int envInt(const std::unordered_map<std::string, std::string> &fileEnv,
             const char *key,
             int fallback)
  {
    try
    {
      return std::stoi(envString(fileEnv, key, std::to_string(fallback)));
    }
    catch (...)
    {
      return fallback;
    }
  }

  double envDouble(const std::unordered_map<std::string, std::string> &fileEnv,
                   const char *key,
                   double fallback)
  {
    try
    {
      return std::stod(envString(fileEnv, key, std::to_string(fallback)));
    }
    catch (...)
    {
      return fallback;
    }
  }

  bool envBool(const std::unordered_map<std::string, std::string> &fileEnv,
               const char *key,
               bool fallback)
  {
    std::string value = envString(fileEnv, key, fallback ? "true" : "false");
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return value == "1" || value == "true" || value == "yes" || value == "on";
  }

  std::string mimeType(const std::filesystem::path &path)
  {
    const auto ext = path.extension().string();
    if (ext == ".html")
      return "text/html; charset=utf-8";
    if (ext == ".css")
      return "text/css; charset=utf-8";
    if (ext == ".js")
      return "application/javascript; charset=utf-8";
    if (ext == ".json")
      return "application/json; charset=utf-8";
    if (ext == ".png")
      return "image/png";
    if (ext == ".svg")
      return "image/svg+xml";
    return "application/octet-stream";
  }

  bool readFile(const std::filesystem::path &path, std::string &out)
  {
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
  }

  bool pathInside(const std::filesystem::path &root, const std::filesystem::path &path)
  {
    std::error_code ec;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec)
      return false;
    const auto canonicalPath = std::filesystem::weakly_canonical(path, ec);
    if (ec)
      return false;

    const auto mismatch = std::mismatch(canonicalRoot.begin(), canonicalRoot.end(), canonicalPath.begin(), canonicalPath.end());
    return mismatch.first == canonicalRoot.end();
  }

  bool safeTarget(std::string_view target)
  {
    return !target.empty() &&
           target.front() == '/' &&
           (target.size() == 1 || target[1] != '/') &&
           target.find("..") == std::string_view::npos &&
           target.find('\\') == std::string_view::npos &&
           target.find('%') == std::string_view::npos;
  }

  std::string pathOnly(std::string_view target)
  {
    const auto queryPos = target.find('?');
    if (queryPos == std::string_view::npos)
      return std::string(target);
    return std::string(target.substr(0, queryPos));
  }

  void recordHttpResponse(http::status status)
  {
    ++gHttpResponsesTotal;
    const auto code = static_cast<unsigned>(status);
    if (code < 200)
      ++gHttpResponses1xx;
    else if (code < 300)
      ++gHttpResponses2xx;
    else if (code < 400)
      ++gHttpResponses3xx;
    else if (code < 500)
      ++gHttpResponses4xx;
    else
      ++gHttpResponses5xx;
  }

  void appendMetric(std::ostringstream &out, const char *name, const char *help, const char *type, std::uint64_t value)
  {
    out << "# HELP " << name << ' ' << help << '\n';
    out << "# TYPE " << name << ' ' << type << '\n';
    out << name << ' ' << value << '\n';
  }

  std::string httpMetricsText()
  {
    std::ostringstream out;
    appendMetric(out, "vix_arena_http_responses_total", "Total HTTP responses returned by the embedded server.", "counter", gHttpResponsesTotal.load());
    appendMetric(out, "vix_arena_http_dynamic_requests_total", "Total dynamic HTTP endpoint requests subject to rate limiting.", "counter", gHttpDynamicRequestsTotal.load());
    appendMetric(out, "vix_arena_http_rate_limit_rejections_total", "Total dynamic HTTP requests rejected by rate limiting.", "counter", gHttpRateLimitRejectionsTotal.load());
    appendMetric(out, "vix_arena_http_responses_1xx_total", "Total 1xx HTTP responses.", "counter", gHttpResponses1xx.load());
    appendMetric(out, "vix_arena_http_responses_2xx_total", "Total 2xx HTTP responses.", "counter", gHttpResponses2xx.load());
    appendMetric(out, "vix_arena_http_responses_3xx_total", "Total 3xx HTTP responses.", "counter", gHttpResponses3xx.load());
    appendMetric(out, "vix_arena_http_responses_4xx_total", "Total 4xx HTTP responses.", "counter", gHttpResponses4xx.load());
    appendMetric(out, "vix_arena_http_responses_5xx_total", "Total 5xx HTTP responses.", "counter", gHttpResponses5xx.load());
    return out.str();
  }

  bool rateLimitedTarget(const std::string &target)
  {
    return target == "/health" ||
           target == "/ready" ||
           target == "/metrics" ||
           target.rfind("/api/", 0) == 0;
  }

  bool safeAuthorityForCsp(std::string_view value)
  {
    return !value.empty() && value.size() <= 253 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c)
                       {
                         return std::isalnum(c) ||
                                c == '.' ||
                                c == '-' ||
                                c == ':' ||
                                c == '[' ||
                                c == ']'; });
  }

  template <class Body, class Allocator>
  std::string cspConnectSrc(const http::request<Body, http::basic_fields<Allocator>> &req)
  {
    const auto hostIt = req.find(http::field::host);
    if (hostIt == req.end())
    {
      return "'self'";
    }

    std::string host = trim(std::string(hostIt->value()));
    if (!safeAuthorityForCsp(host))
    {
      return "'self'";
    }

    return "'self' ws://" + host + " wss://" + host;
  }

  std::string firstForwardedAddress(std::string value)
  {
    if (const auto comma = value.find(','); comma != std::string::npos)
    {
      value.resize(comma);
    }
    value = trim(std::move(value));
    if (value.size() > 96)
    {
      value.resize(96);
    }

    beast::error_code ec;
    const auto address = asio::ip::make_address(value, ec);
    return ec ? std::string{} : address.to_string();
  }

  template <class Body, class Allocator>
  std::string forwardedClientAddress(const http::request<Body, http::basic_fields<Allocator>> &req, bool trustForwardedHeaders)
  {
    if (!trustForwardedHeaders)
    {
      return {};
    }
    if (const auto it = req.find("X-Real-IP"); it != req.end())
    {
      if (std::string value = firstForwardedAddress(std::string(it->value())); !value.empty())
        return value;
    }
    if (const auto it = req.find("CF-Connecting-IP"); it != req.end())
    {
      if (std::string value = firstForwardedAddress(std::string(it->value())); !value.empty())
        return value;
    }
    if (const auto it = req.find("X-Forwarded-For"); it != req.end())
    {
      if (std::string value = firstForwardedAddress(std::string(it->value())); !value.empty())
        return value;
    }
    return {};
  }

  template <class Body, class Allocator>
  http::response<http::string_body> makeResponse(
      const http::request<Body, http::basic_fields<Allocator>> &req,
      http::status status,
      std::string body,
      std::string contentType)
  {
    recordHttpResponse(status);
    http::response<http::string_body> res{status, req.version()};
    res.set(http::field::server, "VixArena");
    res.set(http::field::content_type, contentType);
    res.set(http::field::cache_control, "no-store");
    res.set("X-Content-Type-Options", "nosniff");
    res.set(http::field::x_frame_options, "DENY");
    res.set("Referrer-Policy", "no-referrer");
    res.set("Permissions-Policy", "camera=(), microphone=(), geolocation=(), payment=(), usb=(), fullscreen=(self)");
    const std::string contentSecurityPolicy =
        "default-src 'self'; "
        "connect-src " +
        cspConnectSrc(req) +
        "; script-src 'self'; "
        "style-src 'self'; "
        "img-src 'self' data:; "
        "worker-src 'self'; "
        "manifest-src 'self'; "
        "base-uri 'self'; "
        "frame-ancestors 'none'";
    res.set("Content-Security-Policy", contentSecurityPolicy);
    res.keep_alive(req.keep_alive());
    res.body() = std::move(body);
    res.prepare_payload();
    return res;
  }

  template <class Body, class Allocator>
  bool originAllowed(const http::request<Body, http::basic_fields<Allocator>> &req, const AppConfig &config)
  {
    const auto originIt = req.find(http::field::origin);
    if (originIt == req.end())
      return config.allowMissingOrigin;

    const std::string origin = std::string(originIt->value());
    return config.allowedOrigins.contains(origin);
  }

  void logJson(std::string level, std::string event, nlohmann::json fields = nlohmann::json::object())
  {
    fields["level"] = std::move(level);
    fields["event"] = std::move(event);
    fields["timestamp"] = arena::isoTimestampUtc();
    std::cout << fields.dump() << std::endl;
  }

  class WebSocketSession : public std::enable_shared_from_this<WebSocketSession>
  {
  public:
    WebSocketSession(tcp::socket socket, arena::GameServer &game)
        : ws_(std::move(socket)),
          game_(game),
          client_(std::make_shared<arena::ClientConnection>())
    {
      beast::error_code ec;
      const auto endpoint = ws_.next_layer().remote_endpoint(ec);
      if (!ec)
      {
        client_->remoteAddress = endpoint.address().to_string();
        trustForwardedHeaders_ = endpoint.address().is_loopback();
      }
    }

    ~WebSocketSession()
    {
      closeClient();
    }

    template <class Body, class Allocator>
    void run(http::request<Body, http::basic_fields<Allocator>> req)
    {
      if (std::string forwarded = forwardedClientAddress(req, trustForwardedHeaders_); !forwarded.empty())
      {
        client_->remoteAddress = std::move(forwarded);
      }
      ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
      ws_.read_message_max(arena::maxWsPayloadBytes);
      ws_.set_option(websocket::stream_base::decorator(
          [](websocket::response_type &res)
          { res.set(http::field::server, "VixArena"); }));

      auto self = shared_from_this();
      client_->send = [weak = weak_from_this()](const std::string &message)
      {
        if (auto s = weak.lock())
          s->send(message);
      };
      client_->close = [weak = weak_from_this()](const std::string &reason)
      {
        if (auto s = weak.lock())
          s->close(reason);
      };

      ws_.async_accept(req, [self](beast::error_code ec)
                       { self->onAccept(ec); });
    }

  private:
    void onAccept(beast::error_code ec)
    {
      if (ec)
        return;
      if (!game_.onOpen(client_))
      {
        close("connection limit");
        return;
      }
      read();
    }

    void read()
    {
      auto self = shared_from_this();
      ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t)
                     { self->onRead(ec); });
    }

    void onRead(beast::error_code ec)
    {
      if (ec)
      {
        closeClient();
        return;
      }

      const std::string payload = beast::buffers_to_string(buffer_.data());
      buffer_.consume(buffer_.size());
      game_.onMessage(client_.get(), payload);
      read();
    }

    void send(const std::string &message)
    {
      auto self = shared_from_this();
      asio::post(ws_.get_executor(), [self, message]
                 {
                   if (self->outbox_.size() >= maxWsOutboxMessages ||
                       self->outboxBytes_ + message.size() > maxWsOutboxBytes)
                   {
                     self->closeClient();
                     beast::error_code ec;
                     self->ws_.close(websocket::close_code::policy_error, ec);
                     return;
                   }
                   const bool writing = !self->outbox_.empty();
                   self->outbox_.push_back(message);
                   self->outboxBytes_ += message.size();
                   if (!writing)
                     self->write();
                 });
    }

    void write()
    {
      auto self = shared_from_this();
      ws_.text(true);
      ws_.async_write(asio::buffer(outbox_.front()), [self](beast::error_code ec, std::size_t)
                      { self->onWrite(ec); });
    }

    void onWrite(beast::error_code ec)
    {
      if (ec)
      {
        closeClient();
        return;
      }
      outboxBytes_ -= std::min(outboxBytes_, outbox_.front().size());
      outbox_.pop_front();
      if (!outbox_.empty())
        write();
    }

    void closeClient()
    {
      bool expected = true;
      if (client_->open.compare_exchange_strong(expected, false))
        game_.onClose(client_.get());
    }

    void close(const std::string &reason)
    {
      auto self = shared_from_this();
      asio::post(ws_.get_executor(), [self, reason]
                 {
                   self->closeClient();
                   beast::error_code ec;
                   websocket::close_reason closeReason(websocket::close_code::policy_error);
                   closeReason.reason = reason.size() > 120 ? reason.substr(0, 120) : reason;
                   self->ws_.close(closeReason, ec);
                 });
    }

    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    arena::GameServer &game_;
    std::shared_ptr<arena::ClientConnection> client_;
    std::deque<std::string> outbox_;
    std::size_t outboxBytes_{0};
    bool trustForwardedHeaders_{false};
  };

  class HttpSession : public std::enable_shared_from_this<HttpSession>
  {
  public:
    HttpSession(tcp::socket socket, arena::GameServer &game, std::filesystem::path root, const AppConfig &config)
        : socket_(std::move(socket)), game_(game), root_(std::move(root)), config_(config)
    {
    }

    void run()
    {
      read();
    }

  private:
    void read()
    {
      req_ = {};
      auto parser = std::make_shared<http::request_parser<http::string_body>>();
      parser->body_limit(maxHttpRequestBodyBytes);
      parser->header_limit(maxHttpRequestHeaderBytes);
      auto self = shared_from_this();
      http::async_read(socket_, buffer_, *parser, [self, parser](beast::error_code ec, std::size_t)
                       {
                         if (!ec)
                         {
                           self->req_ = parser->release();
                         }
                         self->onRead(ec);
                       });
    }

    void onRead(beast::error_code ec)
    {
      if (ec == http::error::end_of_stream)
        return close();
      if (ec)
        return;

      const auto requestTarget = req_.target();
      const std::string targetPath = pathOnly(std::string_view(requestTarget.data(), requestTarget.size()));
      if (websocket::is_upgrade(req_) && targetPath == "/ws")
      {
        if (!originAllowed(req_, config_))
        {
          const std::string body = "forbidden websocket origin";
          http::response<http::string_body> res = makeResponse(req_, http::status::forbidden, body, "text/plain; charset=utf-8");
          res.keep_alive(false);
          writeResponse(std::move(res));
          return;
        }
        std::make_shared<WebSocketSession>(std::move(socket_), game_)->run(std::move(req_));
        return;
      }

      handleRequest();
    }

    void handleRequest()
    {
      http::response<http::string_body> res;
      const auto requestTarget = req_.target();
      std::string target(requestTarget.data(), requestTarget.size());
      std::string query;
      if (target.size() > 2048)
      {
        writeResponse(makeResponse(req_, http::status::uri_too_long, "uri too long", "text/plain; charset=utf-8"));
        return;
      }
      if (const auto queryPos = target.find('?'); queryPos != std::string::npos)
      {
        query = target.substr(queryPos + 1);
        target.resize(queryPos);
      }
      const std::string rawRoomFilter = queryParam(query, "room");
      const std::string roomFilter = rawRoomFilter.empty() ? "" : arena::sanitizeRoomCode(rawRoomFilter);

      if (rateLimitedTarget(target))
      {
        ++gHttpDynamicRequestsTotal;
        beast::error_code endpointEc;
        const auto endpoint = socket_.remote_endpoint(endpointEc);
        const bool trustForwardedHeaders = !endpointEc && endpoint.address().is_loopback();
        std::string clientAddress = forwardedClientAddress(req_, trustForwardedHeaders);
        if (clientAddress.empty() && !endpointEc)
        {
          clientAddress = endpoint.address().to_string();
        }
        if (!gHttpRateLimiter.allow(clientAddress))
        {
          ++gHttpRateLimitRejectionsTotal;
          res = makeResponse(req_, http::status::too_many_requests, "rate limit", "text/plain; charset=utf-8");
          res.set(http::field::retry_after, "1");
          writeResponse(std::move(res));
          return;
        }
      }

      if (req_.method() != http::verb::get && req_.method() != http::verb::head)
      {
        res = makeResponse(req_, http::status::method_not_allowed, "method not allowed", "text/plain; charset=utf-8");
      }
      else if (target == "/health")
      {
        res = makeResponse(req_, http::status::ok, game_.healthJson().dump(), "application/json; charset=utf-8");
      }
      else if (target == "/ready")
      {
        const nlohmann::json ready = game_.readyJson();
        res = makeResponse(req_, ready.value("ready", false) ? http::status::ok : http::status::service_unavailable, ready.dump(), "application/json; charset=utf-8");
      }
      else if (target == "/api/state")
      {
        res = makeResponse(req_, http::status::ok, game_.stateJson(roomFilter).dump(), "application/json; charset=utf-8");
      }
      else if (target == "/api/stats")
      {
        res = makeResponse(req_, http::status::ok, game_.statsJson(roomFilter).dump(), "application/json; charset=utf-8");
      }
      else if (target == "/api/rooms")
      {
        res = makeResponse(req_, http::status::ok, game_.roomsJson().dump(), "application/json; charset=utf-8");
      }
      else if (target == "/api/leaderboard")
      {
        res = makeResponse(req_, http::status::ok, game_.leaderboardJson(roomFilter).dump(), "application/json; charset=utf-8");
      }
      else if (target == "/api/matches")
      {
        res = makeResponse(req_, http::status::ok, game_.matchesJson(roomFilter).dump(), "application/json; charset=utf-8");
      }
      else if (target == "/metrics")
      {
        std::string metrics = game_.metricsText();
        metrics += httpMetricsText();
        res = makeResponse(req_, http::status::ok, std::move(metrics), "text/plain; version=0.0.4; charset=utf-8");
      }
      else
      {
        serveFile(target, res);
      }

      writeResponse(std::move(res));
    }

    void writeResponse(http::response<http::string_body> res)
    {
      auto self = shared_from_this();
      if (req_.method() == http::verb::head)
      {
        auto sp = std::make_shared<http::response<http::empty_body>>();
        sp->base() = res.base();
        sp->content_length(res.body().size());
        http::async_write(socket_, *sp, [self, sp](beast::error_code ec, std::size_t)
                          { self->onWrite(ec, sp->need_eof()); });
        return;
      }
      auto sp = std::make_shared<http::response<http::string_body>>(std::move(res));
      http::async_write(socket_, *sp, [self, sp](beast::error_code ec, std::size_t)
                        { self->onWrite(ec, sp->need_eof()); });
    }

    void serveFile(const std::string &target, http::response<http::string_body> &res)
    {
      if (!safeTarget(target))
      {
        res = makeResponse(req_, http::status::bad_request, "bad request", "text/plain; charset=utf-8");
        return;
      }

      const std::filesystem::path publicRoot = root_ / "public";
      std::filesystem::path path = publicRoot;
      if (target == "/")
      {
        path /= "index.html";
      }
      else if (target == "/docs")
      {
        path /= "docs.html";
      }
      else if (target == "/stats")
      {
        path /= "stats.html";
      }
      else
      {
        const std::filesystem::path relative = std::filesystem::path(target.substr(1)).lexically_normal();
        if (relative.empty() || relative.is_absolute())
        {
          res = makeResponse(req_, http::status::bad_request, "bad request", "text/plain; charset=utf-8");
          return;
        }
        path /= relative;
      }

      std::error_code ec;
      if (!std::filesystem::exists(path, ec))
      {
        res = makeResponse(req_, http::status::not_found, "not found", "text/plain; charset=utf-8");
        return;
      }
      if (!pathInside(publicRoot, path))
      {
        res = makeResponse(req_, http::status::bad_request, "bad request", "text/plain; charset=utf-8");
        return;
      }
      const auto size = std::filesystem::file_size(path, ec);
      if (ec)
      {
        res = makeResponse(req_, http::status::not_found, "not found", "text/plain; charset=utf-8");
        return;
      }
      if (size > maxStaticFileBytes)
      {
        res = makeResponse(req_, http::status::payload_too_large, "payload too large", "text/plain; charset=utf-8");
        return;
      }

      std::string body;
      if (!readFile(path, body))
      {
        res = makeResponse(req_, http::status::not_found, "not found", "text/plain; charset=utf-8");
        return;
      }

      res = makeResponse(req_, http::status::ok, std::move(body), mimeType(path));
      res.set(http::field::cache_control, target == "/" || target == "/docs" || target == "/stats" || target == "/sw.js" || target == "/manifest.json" ? "no-store" : "public, max-age=300");
    }

    void onWrite(beast::error_code ec, bool closeAfter)
    {
      if (ec)
        return;
      if (closeAfter)
        return close();
      read();
    }

    void close()
    {
      beast::error_code ec;
      socket_.shutdown(tcp::socket::shutdown_send, ec);
    }

    tcp::socket socket_;
    beast::flat_buffer buffer_;
    arena::GameServer &game_;
    std::filesystem::path root_;
    const AppConfig &config_;
    http::request<http::string_body> req_;
  };

  class Listener : public std::enable_shared_from_this<Listener>
  {
  public:
    Listener(asio::io_context &ioc, tcp::endpoint endpoint, arena::GameServer &game, std::filesystem::path root, const AppConfig &config)
        : ioc_(ioc), acceptor_(ioc), game_(game), root_(std::move(root)), config_(config)
    {
      beast::error_code ec;
      acceptor_.open(endpoint.protocol(), ec);
      if (ec)
        throw beast::system_error(ec);
      acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
      if (ec)
        throw beast::system_error(ec);
      acceptor_.bind(endpoint, ec);
      if (ec)
        throw beast::system_error(ec);
      acceptor_.listen(asio::socket_base::max_listen_connections, ec);
      if (ec)
        throw beast::system_error(ec);
    }

    void run()
    {
      accept();
    }

  private:
    void accept()
    {
      auto self = shared_from_this();
      acceptor_.async_accept(asio::make_strand(ioc_), [self](beast::error_code ec, tcp::socket socket)
                             { self->onAccept(ec, std::move(socket)); });
    }

    void onAccept(beast::error_code ec, tcp::socket socket)
    {
      if (!ec)
        std::make_shared<HttpSession>(std::move(socket), game_, root_, config_)->run();
      accept();
    }

    asio::io_context &ioc_;
    tcp::acceptor acceptor_;
    arena::GameServer &game_;
    std::filesystem::path root_;
    const AppConfig &config_;
  };
}

int runServer()
{
  const std::filesystem::path root = std::filesystem::current_path();
  const auto fileEnv = readDotEnv(root / ".env");
  const std::string appHost = envString(fileEnv, "APP_HOST", "127.0.0.1");
  const int appPort = envInt(fileEnv, "APP_PORT", 18080);
  const std::string publicUrl = envString(fileEnv, "PUBLIC_URL", "");
  const std::string databaseUrl = envString(fileEnv, "DATABASE_URL", "");
  const std::filesystem::path dataDir = envString(fileEnv, "DATA_DIR", (root / "data").string());
  arena::GameServer::Limits gameLimits;
  gameLimits.maxPlayersPerRoom = static_cast<std::size_t>(std::max(1, envInt(fileEnv, "MAX_PLAYERS_PER_ROOM", static_cast<int>(gameLimits.maxPlayersPerRoom))));
  gameLimits.maxActiveRooms = static_cast<std::size_t>(std::max(1, envInt(fileEnv, "MAX_ACTIVE_ROOMS", static_cast<int>(gameLimits.maxActiveRooms))));
  gameLimits.maxConnectionsPerIp = static_cast<std::size_t>(std::max(1, envInt(fileEnv, "MAX_CONNECTIONS_PER_IP", static_cast<int>(gameLimits.maxConnectionsPerIp))));
  gameLimits.stalePlayerSeconds = std::max(1, envInt(fileEnv, "STALE_PLAYER_SECONDS", gameLimits.stalePlayerSeconds));
  gameLimits.wsMessageBurst = std::max(1.0, envDouble(fileEnv, "WS_MESSAGE_BURST", gameLimits.wsMessageBurst));
  gameLimits.wsMessageRefillPerSecond = std::max(0.1, envDouble(fileEnv, "WS_MESSAGE_REFILL_PER_SECOND", gameLimits.wsMessageRefillPerSecond));
  gameLimits.maxInvalidMessagesPerConnection = static_cast<std::uint32_t>(std::max(1, envInt(fileEnv, "MAX_INVALID_MESSAGES_PER_CONNECTION", static_cast<int>(gameLimits.maxInvalidMessagesPerConnection))));
  gameLimits.benchmarkSourceIps = parseCsvList(envString(fileEnv, "BENCHMARK_SOURCE_IPS", ""));
  gameLimits.benchmarkMaxPlayersPerRoom = static_cast<std::size_t>(std::max(1, envInt(fileEnv, "BENCHMARK_MAX_PLAYERS_PER_ROOM", static_cast<int>(gameLimits.benchmarkMaxPlayersPerRoom))));
  gameLimits.benchmarkMaxConnectionsPerIp = static_cast<std::size_t>(std::max(1, envInt(fileEnv, "BENCHMARK_MAX_CONNECTIONS_PER_IP", static_cast<int>(gameLimits.benchmarkMaxConnectionsPerIp))));
  gameLimits.benchmarkWsMessageBurst = std::max(gameLimits.wsMessageBurst, envDouble(fileEnv, "BENCHMARK_WS_MESSAGE_BURST", gameLimits.benchmarkWsMessageBurst));
  gameLimits.benchmarkWsMessageRefillPerSecond = std::max(gameLimits.wsMessageRefillPerSecond, envDouble(fileEnv, "BENCHMARK_WS_MESSAGE_REFILL_PER_SECOND", gameLimits.benchmarkWsMessageRefillPerSecond));
  const double httpRateBurst = std::max(1.0, envDouble(fileEnv, "HTTP_RATE_LIMIT_BURST", httpRateLimitBurst));
  const double httpRateRefillPerSecond = std::max(0.1, envDouble(fileEnv, "HTTP_RATE_LIMIT_REFILL_PER_SECOND", httpRateLimitRefillPerSecond));
  const double benchmarkHttpRateBurst = std::max(httpRateBurst, envDouble(fileEnv, "BENCHMARK_HTTP_RATE_LIMIT_BURST", 5000.0));
  const double benchmarkHttpRateRefillPerSecond = std::max(httpRateRefillPerSecond, envDouble(fileEnv, "BENCHMARK_HTTP_RATE_LIMIT_REFILL_PER_SECOND", 1000.0));
  gHttpRateLimiter.configure(httpRateBurst, httpRateRefillPerSecond, gameLimits.benchmarkSourceIps, benchmarkHttpRateBurst, benchmarkHttpRateRefillPerSecond);
  AppConfig config;
  config.allowMissingOrigin = envBool(fileEnv, "ALLOW_MISSING_ORIGIN", false);
  config.allowedOrigins = parseOriginList(envString(fileEnv, "ALLOWED_ORIGINS", ""));
  if (const std::string publicOrigin = originFromUrl(publicUrl); !publicOrigin.empty())
    config.allowedOrigins.insert(publicOrigin);
  config.allowedOrigins.insert("http://127.0.0.1:" + std::to_string(appPort));
  config.allowedOrigins.insert("http://localhost:" + std::to_string(appPort));

  arena::GameServer game(dataDir, databaseUrl, root / "migrations", gameLimits);
  game.start();

  asio::io_context ioc{static_cast<int>(std::max(2u, std::thread::hardware_concurrency()))};
  const auto address = asio::ip::make_address(appHost);
  std::make_shared<Listener>(ioc, tcp::endpoint{address, static_cast<unsigned short>(appPort)}, game, root, config)->run();

  asio::signal_set signals(ioc, SIGINT, SIGTERM);
  signals.async_wait([&](const beast::error_code &ec, int signal)
                     {
                       if (!ec)
                       {
                         logJson("info", "shutdown_requested", {{"signal", signal}});
                         game.stop();
                         ioc.stop();
                       }
                     });

  logJson("info", "server_started", {
                                       {"host", appHost},
                                       {"port", appPort},
                                       {"websocketPath", "/ws"},
                                       {"publicUrl", publicUrl},
                                       {"dataDir", dataDir.string()},
                                       {"postgresConfigured", !databaseUrl.empty()},
                                       {"allowedOrigins", config.allowedOrigins},
                                       {"allowMissingOrigin", config.allowMissingOrigin},
                                       {"maxPlayersPerRoom", gameLimits.maxPlayersPerRoom},
                                       {"maxActiveRooms", gameLimits.maxActiveRooms},
                                       {"maxConnectionsPerIp", gameLimits.maxConnectionsPerIp},
                                       {"wsMessageBurst", gameLimits.wsMessageBurst},
                                       {"wsMessageRefillPerSecond", gameLimits.wsMessageRefillPerSecond},
                                       {"benchmarkSourceIps", gameLimits.benchmarkSourceIps.size()},
                                       {"benchmarkMaxPlayersPerRoom", gameLimits.benchmarkMaxPlayersPerRoom},
                                       {"benchmarkMaxConnectionsPerIp", gameLimits.benchmarkMaxConnectionsPerIp},
                                       {"benchmarkWsMessageBurst", gameLimits.benchmarkWsMessageBurst},
                                       {"benchmarkWsMessageRefillPerSecond", gameLimits.benchmarkWsMessageRefillPerSecond},
                                       {"httpRateLimitBurst", httpRateBurst},
                                       {"httpRateLimitRefillPerSecond", httpRateRefillPerSecond},
                                       {"benchmarkHttpRateLimitBurst", benchmarkHttpRateBurst},
                                       {"benchmarkHttpRateLimitRefillPerSecond", benchmarkHttpRateRefillPerSecond},
                                   });

  std::vector<std::thread> threads;
  const unsigned count = std::max(1u, std::thread::hardware_concurrency());
  threads.reserve(count);
  for (unsigned i = 0; i < count; ++i)
    threads.emplace_back([&ioc] { ioc.run(); });
  for (auto &thread : threads)
    thread.join();

  game.stop();
  logJson("info", "server_stopped");
  return 0;
}

int main()
{
  try
  {
    return runServer();
  }
  catch (const std::exception &e)
  {
    std::cerr << nlohmann::json({{"level", "fatal"}, {"event", "startup_failed"}, {"timestamp", arena::isoTimestampUtc()}, {"error", e.what()}}).dump() << '\n';
    return 1;
  }
}
