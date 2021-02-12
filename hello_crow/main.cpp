#include "crow_all.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <boost/filesystem.hpp>

#include <unordered_set>
#include <mutex>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/client.hpp>
#include <mongocxx/stdx.hpp>
#include <mongocxx/uri.hpp>
#include <mongocxx/instance.hpp>

#include <string>

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::stream::close_array;
using bsoncxx::builder::stream::close_document;
using bsoncxx::builder::stream::document;
using bsoncxx::builder::stream::finalize;
using bsoncxx::builder::stream::open_array;
using bsoncxx::builder::stream::open_document;
using mongocxx::cursor;

using namespace crow::mustache;

void getView(crow::response &res, const std::string &filename, context &x)
{
  res.set_header("Content-Type", "text/html");
  auto text = load("../public/" + filename + ".html").render(x);
  res.write(text);
  res.end();
}

void sendFile(crow::response &res, std::string filename, std::string contentType)
{
  std::ifstream in("../public/" + filename, std::ifstream::in);
  if (in)
  {
    std::ostringstream contents;
    contents << in.rdbuf();
    in.close();
    res.set_header("Content-Type", contentType);
    res.write(contents.str());
  }
  else
  {
    res.code = 404;
    res.write("Not found");
  }
  res.end();
}

void sendHtml(crow::response &res, std::string filename)
{
  sendFile(res, filename + ".html", "text/html");
}

void sendImage(crow::response &res, std::string filename)
{
  sendFile(res, "images/" + filename, "image/jpeg");
}

void sendScript(crow::response &res, std::string filename)
{
  sendFile(res, "scripts/" + filename, "text/javascript");
}

void sendStyle(crow::response &res, std::string filename)
{
  sendFile(res, "styles/" + filename, "text/css");
}

void notFound(crow::response &res, const std::string &message)
{
  res.code = 404;
  res.write(message + ": Not Found");
  res.end();
}

int main(int argc, char *argv[])
{
  std::mutex mtx;
  std::unordered_set<crow::websocket::connection *> users;
  crow::SimpleApp app;
  set_base(".");

  mongocxx::instance inst{};
  std::string mongoConnect = std::string(getenv("MONGODB_URI"));
  mongocxx::client conn{mongocxx::uri{mongoConnect}};
  auto collection = conn["heroku_crow"]["contacts"];

  CROW_ROUTE(app, "/")
  ([](const crow::request &req, crow::response &res) {
    sendHtml(res, "index");
  });

  CROW_ROUTE(app, "/ws")
      .websocket()
      .onopen([&](crow::websocket::connection &conn) {
        std::lock_guard<std::mutex> _(mtx);
        users.insert(&conn);
      })
      .onclose([&](crow::websocket::connection &conn, const std::string &reason) {
        std::lock_guard<std::mutex> _(mtx);
        users.erase(&conn);
      })
      .onmessage([&](crow::websocket::connection &conn, const std::string &data, bool is_binary) {
        std::lock_guard<std::mutex> _(mtx);
        for (auto user : users)
        {
          if (is_binary)
          {
            user->send_binary(data);
          }
          else
          {
            user->send_text(data);
          }
        }
      });

  CROW_ROUTE(app, "/chat")
  ([](const crow::request &req, crow::response &res) {
    sendHtml(res, "chat");
  });

  CROW_ROUTE(app, "/rest_test")
      .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Get, crow::HTTPMethod::Put)([](const crow::request &req, crow::response &res) {
        std::string method = crow::method_name(req.method);
        res.set_header("Content-Type", "text/plain");
        res.write(method + " res_test");
        res.end();
      });

  CROW_ROUTE(app, "/add/<int>/<int>")
  ([](const crow::request &req, crow::response &res, int a, int b) {
    res.set_header("Content-Type", "text/plain");
    std::ostringstream os;
    os << "Integer: " << a << " + " << b << " = " << a + b << "\n";
    res.write(os.str());
    res.end();
  });

  CROW_ROUTE(app, "/add/<double>/<double>")
  ([](const crow::request &req, crow::response &res, double a, double b) {
    res.set_header("Content-Type", "text/plain");
    std::ostringstream os;
    os << "Double: " << a << " + " << b << " = " << a + b << "\n";
    res.write(os.str());
    res.end();
  });

  CROW_ROUTE(app, "/add/<string>/<string>")
  ([](const crow::request &req, crow::response &res, std::string a, std::string b) {
    res.set_header("Content-Type", "text/plain");
    std::ostringstream os;
    os << "String: " << a << " + " << b << " = " << a + b << "\n";
    res.write(os.str());
    res.end();
  });

  CROW_ROUTE(app, "/query")
  ([](const crow::request &req, crow::response &res) {
    auto firstName = req.url_params.get("firstname");
    auto lastName = req.url_params.get("lastname");
    std::ostringstream os;
    os << "Hello " << (firstName ? firstName : "") << " " << (lastName ? lastName : "") << "\n";
    res.set_header("Content-Type", "text/plain");
    res.write(os.str());
    res.end();
  });

  CROW_ROUTE(app, "/contacts")
  ([&collection](const crow::request &req, crow::response &res) {
    mongocxx::options::find opts;
    opts.skip(9);
    opts.limit(10);
    auto docs = collection.find({}, opts);
    crow::json::wvalue dto;
    std::vector<crow::json::rvalue> contacts;
    contacts.reserve(10);

    for (auto doc : docs)
    {
      contacts.push_back(crow::json::load(bsoncxx::to_json(doc)));
    }
    dto["contacts"] = contacts;
    getView(res, "contacts", dto);
  });

  CROW_ROUTE(app, "/api/contacts")
  ([&collection](const crow::request &req) {
    mongocxx::options::find opts;
    auto querySkip = req.url_params.get("skip");
    auto queryLimit = req.url_params.get("limit");
    int skip = querySkip ? std::stoi(querySkip) : 0;
    int limit = queryLimit ? std::stoi(queryLimit) : 10;
    opts.skip(skip);
    opts.limit(limit);
    auto docs = collection.find({}, opts);
    crow::json::wvalue dto;
    std::vector<crow::json::rvalue> contacts;
    contacts.reserve(10);

    for (auto doc : docs)
    {
      contacts.push_back(crow::json::load(bsoncxx::to_json(doc)));
    }
    dto["contacts"] = contacts;
    return crow::response{dto};
  });

  CROW_ROUTE(app, "/contact/<string>")
  ([&collection](const crow::request &req, crow::response &res, std::string email) {
    auto doc = collection.find_one(make_document(kvp("email", email)));
    crow::json::wvalue dto;
    dto["contact"] = crow::json::load(bsoncxx::to_json(doc.value().view()));
    getView(res, "contact", dto);
  });

  CROW_ROUTE(app, "/contact/<string>/<string>")
  ([&collection](const crow::request &req, crow::response &res, std::string firstName, std::string lastName) {
    auto doc = collection.find_one(
        make_document(kvp("firstName", firstName), kvp("lastName", lastName)));
    if (!doc)
    {
      return notFound(res, "Contact");
    }
    crow::json::wvalue dto;
    dto["contact"] = crow::json::load(bsoncxx::to_json(doc.value().view()));
    getView(res, "contact", dto);
  });

  CROW_ROUTE(app, "/about")
  ([](const crow::request &req, crow::response &res) {
    sendHtml(res, "about");
  });

  CROW_ROUTE(app, "/styles/<string>")
  ([](const crow::request &req, crow::response &res, std::string filename) {
    sendStyle(res, filename);
  });

  CROW_ROUTE(app, "/scripts/<string>")
  ([](const crow::request &req, crow::response &res, std::string filename) {
    sendScript(res, filename);
  });

  CROW_ROUTE(app, "/images/<string>")
  ([](const crow::request &req, crow::response &res, std::string filename) {
    sendImage(res, filename);
  });

  char *port = getenv("PORT");
  uint16_t iPort = static_cast<uint16_t>(port != NULL ? std::stoi(port) : 18080);
  std::cout << "PORT = " << iPort << "\n";

  app.port(iPort).multithreaded().run();
}
