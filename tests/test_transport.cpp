#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <bulutklinik/bulutklinik.hpp>

using namespace bulutklinik;

namespace {

class MockBackend : public HttpBackend {
public:
    std::function<HttpResponse(const HttpRequest&)> responder;
    std::vector<HttpRequest> requests;

    HttpResponse send(const HttpRequest& request) override {
        requests.push_back(request);
        return responder(request);
    }
};

HttpResponse json_resp(int status, const std::string& body) {
    HttpResponse r;
    r.status = status;
    r.body = body;
    return r;
}

ClientOptions base_options(const std::shared_ptr<MockBackend>& backend, std::shared_ptr<TokenStore> store) {
    ClientOptions o;
    o.base_url = "http://localhost";
    o.http_backend = backend;
    o.token_store = std::move(store);
    return o;
}

}  // namespace

TEST_CASE("unwraps data and sends headers") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(200, R"({"resultType":0,"data":{"searchedDoctors":[]}})");
    };
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    auto data = client.doctors().quick_search("kardiyo");

    REQUIRE(data["searchedDoctors"].is_array());
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/quickSearch");
    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer abc");
    REQUIRE(backend->requests.at(0).headers.at("lang") == "tr");
}

TEST_CASE("request escape hatch issues a bearer GET to the right URL") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(200, R"({"resultType":0,"data":{"ok":true}})");
    };
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    auto data = client.request("GET", "/patients/customEndpoint");

    REQUIRE(data["ok"].get<bool>());
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/customEndpoint");
    REQUIRE(backend->requests.at(0).method == "GET");
    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer abc");
}

TEST_CASE("request escape hatch sends a public POST body and omits Authorization") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) { return json_resp(200, R"({"resultType":0,"data":{"id":7}})"); };
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    RequestOptions options;
    options.auth = Auth::Public;
    options.body = {{"foo", "bar"}};
    auto data = client.request("POST", "/general/somePublicEndpoint", options);

    REQUIRE(data["id"].get<int>() == 7);
    REQUIRE(backend->requests.at(0).method == "POST");
    REQUIRE(backend->requests.at(0).headers.find("Authorization") == backend->requests.at(0).headers.end());
    auto body = nlohmann::json::parse(backend->requests.at(0).body.value());
    REQUIRE(body["foo"] == "bar");
}

TEST_CASE("maps 422 to ValidationError") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(422, R"({"resultType":1,"errorType":"validation"})");
    };
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("a"), std::nullopt)));

    REQUIRE_THROWS_AS(client.doctors().branches(), ValidationError);
}

TEST_CASE("maps numeric 404 to NotFoundError") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(404, R"({"resultType":1,"errorType":1,"errorMessage":"Bilinmeyen"})");
    };
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("a"), std::nullopt)));

    REQUIRE_THROWS_AS(client.doctors().quick_search("x"), NotFoundError);
}

TEST_CASE("refreshes once then retries with the new token") {
    auto backend = std::make_shared<MockBackend>();
    int data_calls = 0;
    backend->responder = [&data_calls](const HttpRequest& req) {
        if (req.url.find("/general/refreshApi") != std::string::npos) {
            return json_resp(200, R"({"resultType":0,"data":{"access_token":"new","refresh_token":"r2"}})");
        }
        ++data_calls;
        if (data_calls == 1) {
            return json_resp(401, R"({"resultType":4})");
        }
        return json_resp(200, R"({"resultType":0,"data":{"ok":true}})");
    };
    auto store = std::make_shared<InMemoryTokenStore>(std::string("old"), std::string("r"));
    ClientOptions o = base_options(backend, store);
    o.client_id = "c";
    o.client_secret = "s";
    Client client(o);

    auto data = client.measures().last();

    REQUIRE(data["ok"].get<bool>());
    REQUIRE(store->access_token().value() == "new");
    REQUIRE(backend->requests.back().headers.at("Authorization") == "Bearer new");
}

TEST_CASE("logout clears the store") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(200, R"({"resultType":2,"errorMessage":"logged out"})");
    };
    auto store = std::make_shared<InMemoryTokenStore>(std::string("a"), std::string("r"));
    Client client(base_options(backend, store));

    REQUIRE_THROWS_AS(client.measures().last(), AuthenticationError);
    REQUIRE_FALSE(store->access_token().has_value());
}

TEST_CASE("connect stores tokens and fills credentials") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(200, R"({"resultType":0,"data":{"access_token":"t","refresh_token":"r"}})");
    };
    auto store = std::make_shared<InMemoryTokenStore>();
    ClientOptions o = base_options(backend, store);
    o.client_id = "c";
    o.client_secret = "s";
    Client client(o);

    auto result = client.auth().connect("u", std::string("p"), "email");

    REQUIRE_FALSE(result.two_factor_required);
    REQUIRE(store->access_token().value() == "t");
    auto body = nlohmann::json::parse(backend->requests.at(0).body.value());
    REQUIRE(body["apiClientId"] == "c");
    REQUIRE(body["loginMode"] == "email");
}

TEST_CASE("uses the partner token") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) { return json_resp(200, R"({"resultType":0,"data":null})"); };
    ClientOptions o = base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("a"), std::nullopt));
    o.partner_token = "PT";
    Client client(o);

    std::vector<nlohmann::json> data = {
        {{"type", "pulse"}, {"date_time", "2026-06-17 09:00"}, {"pulse", 72}},
    };
    client.measures().partner_health_information(std::nullopt, std::string("5551112233"), data);

    REQUIRE(backend->requests.back().headers.at("Authorization") == "Bearer PT");
}
