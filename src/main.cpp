#include <utility>

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <cctype>
#include <cstdlib>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
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

  struct AppConfig
  {
    std::set<std::string> allowedOrigins;
    bool allowMissingOrigin{true};
  };

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

  std::unordered_map<std::string, std::string> readDotEnv(const std::filesystem::path &path)
  {
    std::unordered_map<std::string, std::string> values;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
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
    if (const char *fromEnv = std::getenv(key); fromEnv && *fromEnv)
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

  template <class Body, class Allocator>
  http::response<http::string_body> makeResponse(
      const http::request<Body, http::basic_fields<Allocator>> &req,
      http::status status,
      std::string body,
      std::string contentType)
  {
    http::response<http::string_body> res{status, req.version()};
    res.set(http::field::server, "VixArena");
    res.set(http::field::content_type, contentType);
    res.set(http::field::cache_control, "no-store");
    res.set("X-Content-Type-Options", "nosniff");
    res.set(http::field::x_frame_options, "DENY");
    res.set("Referrer-Policy", "no-referrer");
    res.set("Permissions-Policy", "camera=(), microphone=(), geolocation=(), payment=(), usb=(), fullscreen=(self)");
    res.set("Content-Security-Policy",
            "default-src 'self'; "
            "connect-src 'self' ws: wss:; "
            "script-src 'self'; "
            "style-src 'self' 'unsafe-inline'; "
            "img-src 'self' data:; "
            "base-uri 'self'; "
            "frame-ancestors 'none'");
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
      }
    }

    ~WebSocketSession()
    {
      closeClient();
    }

    template <class Body, class Allocator>
    void run(http::request<Body, http::basic_fields<Allocator>> req)
    {
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
      auto self = shared_from_this();
      http::async_read(socket_, buffer_, req_, [self](beast::error_code ec, std::size_t)
                       { self->onRead(ec); });
    }

    void onRead(beast::error_code ec)
    {
      if (ec == http::error::end_of_stream)
        return close();
      if (ec)
        return;

      const std::string targetPath = pathOnly(req_.target());
      if (websocket::is_upgrade(req_) && targetPath == "/ws")
      {
        if (!originAllowed(req_, config_))
        {
          const std::string body = "forbidden websocket origin";
          http::response<http::string_body> res = makeResponse(req_, http::status::forbidden, body, "text/plain; charset=utf-8");
          auto self = shared_from_this();
          auto sp = std::make_shared<http::response<http::string_body>>(std::move(res));
          http::async_write(socket_, *sp, [self, sp](beast::error_code ec, std::size_t)
                            { self->onWrite(ec, true); });
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
      std::string target(req_.target());
      if (target.size() > 2048)
      {
        res = makeResponse(req_, http::status::uri_too_long, "uri too long", "text/plain; charset=utf-8");
        auto self = shared_from_this();
        auto sp = std::make_shared<http::response<http::string_body>>(std::move(res));
        http::async_write(socket_, *sp, [self, sp](beast::error_code ec, std::size_t)
                          { self->onWrite(ec, sp->need_eof()); });
        return;
      }
      if (const auto queryPos = target.find('?'); queryPos != std::string::npos)
        target.resize(queryPos);

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
        res = makeResponse(req_, http::status::ok, game_.stateJson().dump(), "application/json; charset=utf-8");
      }
      else if (target == "/api/stats")
      {
        res = makeResponse(req_, http::status::ok, game_.statsJson().dump(), "application/json; charset=utf-8");
      }
      else if (target == "/api/leaderboard")
      {
        res = makeResponse(req_, http::status::ok, game_.leaderboardJson().dump(), "application/json; charset=utf-8");
      }
      else if (target == "/api/matches")
      {
        res = makeResponse(req_, http::status::ok, game_.matchesJson().dump(), "application/json; charset=utf-8");
      }
      else if (target == "/metrics")
      {
        res = makeResponse(req_, http::status::ok, game_.metricsText(), "text/plain; version=0.0.4; charset=utf-8");
      }
      else
      {
        serveFile(target, res);
      }

      auto self = shared_from_this();
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

      std::filesystem::path path = root_ / "public";
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

      std::string body;
      if (!readFile(path, body))
      {
        res = makeResponse(req_, http::status::not_found, "not found", "text/plain; charset=utf-8");
        return;
      }

      res = makeResponse(req_, http::status::ok, std::move(body), mimeType(path));
      res.set(http::field::cache_control, target == "/" || target == "/docs" || target == "/stats" ? "no-store" : "public, max-age=300");
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

int main()
{
  const std::filesystem::path root = std::filesystem::current_path();
  const auto fileEnv = readDotEnv(root / ".env");
  const std::string appHost = envString(fileEnv, "APP_HOST", "127.0.0.1");
  const int appPort = envInt(fileEnv, "APP_PORT", 18080);
  const std::string publicUrl = envString(fileEnv, "PUBLIC_URL", "");
  const std::string databaseUrl = envString(fileEnv, "DATABASE_URL", "");
  const std::filesystem::path dataDir = envString(fileEnv, "DATA_DIR", (root / "data").string());
  AppConfig config;
  config.allowMissingOrigin = envBool(fileEnv, "ALLOW_MISSING_ORIGIN", publicUrl.empty());
  config.allowedOrigins = parseOriginList(envString(fileEnv, "ALLOWED_ORIGINS", ""));
  if (const std::string publicOrigin = originFromUrl(publicUrl); !publicOrigin.empty())
    config.allowedOrigins.insert(publicOrigin);
  config.allowedOrigins.insert("http://127.0.0.1:" + std::to_string(appPort));
  config.allowedOrigins.insert("http://localhost:" + std::to_string(appPort));

  try
  {
    arena::GameServer game(dataDir, databaseUrl, root / "migrations");
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
  }
  catch (const std::exception &e)
  {
    std::cerr << nlohmann::json({{"level", "fatal"}, {"event", "startup_failed"}, {"timestamp", arena::isoTimestampUtc()}, {"error", e.what()}}).dump() << '\n';
    return 1;
  }

  return 0;
}
