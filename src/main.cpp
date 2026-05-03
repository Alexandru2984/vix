#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "GameServer.hpp"
#include "Validation.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace
{
  std::string trim(std::string value)
  {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
      value.pop_back();
    return value;
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
    res.keep_alive(req.keep_alive());
    res.body() = std::move(body);
    res.prepare_payload();
    return res;
  }

  class WebSocketSession : public std::enable_shared_from_this<WebSocketSession>
  {
  public:
    WebSocketSession(tcp::socket socket, arena::GameServer &game)
        : ws_(std::move(socket)),
          game_(game),
          client_(std::make_shared<arena::ClientConnection>())
    {
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

      ws_.async_accept(req, [self](beast::error_code ec)
                       { self->onAccept(ec); });
    }

  private:
    void onAccept(beast::error_code ec)
    {
      if (ec)
        return;
      game_.onOpen(client_);
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
                   const bool writing = !self->outbox_.empty();
                   self->outbox_.push_back(message);
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

    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    arena::GameServer &game_;
    std::shared_ptr<arena::ClientConnection> client_;
    std::deque<std::string> outbox_;
  };

  class HttpSession : public std::enable_shared_from_this<HttpSession>
  {
  public:
    HttpSession(tcp::socket socket, arena::GameServer &game, std::filesystem::path root)
        : socket_(std::move(socket)), game_(game), root_(std::move(root))
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

      if (websocket::is_upgrade(req_) && req_.target().starts_with("/ws"))
      {
        std::make_shared<WebSocketSession>(std::move(socket_), game_)->run(std::move(req_));
        return;
      }

      handleRequest();
    }

    void handleRequest()
    {
      http::response<http::string_body> res;
      std::string target(req_.target());
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
      else if (target == "/api/state")
      {
        res = makeResponse(req_, http::status::ok, game_.stateJson().dump(), "application/json; charset=utf-8");
      }
      else if (target == "/api/stats")
      {
        res = makeResponse(req_, http::status::ok, game_.statsJson().dump(), "application/json; charset=utf-8");
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
      res.set(http::field::cache_control, target == "/" || target == "/docs" ? "no-store" : "public, max-age=300");
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
    http::request<http::string_body> req_;
  };

  class Listener : public std::enable_shared_from_this<Listener>
  {
  public:
    Listener(asio::io_context &ioc, tcp::endpoint endpoint, arena::GameServer &game, std::filesystem::path root)
        : ioc_(ioc), acceptor_(ioc), game_(game), root_(std::move(root))
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
        std::make_shared<HttpSession>(std::move(socket), game_, root_)->run();
      accept();
    }

    asio::io_context &ioc_;
    tcp::acceptor acceptor_;
    arena::GameServer &game_;
    std::filesystem::path root_;
  };
}

int main()
{
  const std::filesystem::path root = std::filesystem::current_path();
  const auto fileEnv = readDotEnv(root / ".env");
  const std::string appHost = envString(fileEnv, "APP_HOST", "127.0.0.1");
  const int appPort = envInt(fileEnv, "APP_PORT", 18080);

  try
  {
    arena::GameServer game;
    game.start();

    asio::io_context ioc{static_cast<int>(std::max(2u, std::thread::hardware_concurrency()))};
    const auto address = asio::ip::make_address(appHost);
    std::make_shared<Listener>(ioc, tcp::endpoint{address, static_cast<unsigned short>(appPort)}, game, root)->run();

    std::cout << "VixArena listening on " << appHost << ':' << appPort << '\n';
    std::cout << "WebSocket endpoint: ws://" << appHost << ':' << appPort << "/ws\n";

    std::vector<std::thread> threads;
    const unsigned count = std::max(1u, std::thread::hardware_concurrency());
    threads.reserve(count);
    for (unsigned i = 0; i < count; ++i)
      threads.emplace_back([&ioc] { ioc.run(); });
    for (auto &thread : threads)
      thread.join();

    game.stop();
  }
  catch (const std::exception &e)
  {
    std::cerr << "fatal: " << e.what() << '\n';
    return 1;
  }

  return 0;
}
