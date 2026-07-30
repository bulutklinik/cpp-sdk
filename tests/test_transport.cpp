#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
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

/// Options pointing at the mock backend. Unless a store is supplied, the
/// credential is the partner token "PT".
ClientOptions base_options(const std::shared_ptr<MockBackend>& backend,
                           std::shared_ptr<TokenStore> store = nullptr) {
    ClientOptions o;
    o.base_url = "http://localhost";
    o.http_backend = backend;
    if (store) {
        o.token_store = std::move(store);
    } else {
        o.partner_token = "PT";
    }
    return o;
}

std::shared_ptr<MockBackend> ok_backend() {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(200, R"({"resultType":0,"data":{"ok":true}})");
    };
    return backend;
}

Patient reference_patient() {
    Patient p;
    p.identity_number = "12345678901";
    return p;
}

}  // namespace

TEST_CASE("unwraps data and sends the partner token and lang header") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(200, R"({"resultType":0,"data":{"foundDoctors":[]}})");
    };
    Client client(base_options(backend));

    auto data = client.doctors().search(nlohmann::json{{"withFreeText", "kardiyoloji"}}, 1, {"slot"});

    REQUIRE(data["foundDoctors"].is_array());
    REQUIRE(backend->requests.at(0).url == "http://localhost/outher/search");
    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer PT");
    REQUIRE(backend->requests.at(0).headers.at("lang") == "tr");
}

TEST_CASE("api version selects the base URL without changing any path") {
    for (const auto& pair : std::vector<std::pair<ApiVersion, std::string>>{
             {ApiVersion::V3, "https://apitest.bulutklinik.com/api/v3/outher/branches"},
             {ApiVersion::V4, "https://apitest.bulutklinik.com/api/v4/outher/branches"},
         }) {
        auto backend = ok_backend();
        ClientOptions o;
        o.environment = Environment::Test;
        o.api_version = pair.first;
        o.partner_token = "PT";
        o.http_backend = backend;
        Client client(o);

        client.doctors().branches();

        REQUIRE(backend->requests.at(0).url == pair.second);
    }
}

TEST_CASE("partner_token and token_store together is rejected") {
    ClientOptions o;
    o.partner_token = "PT";
    o.token_store = std::make_shared<InMemoryTokenStore>(std::string("OTHER"));

    REQUIRE_THROWS_AS(Client(o), std::invalid_argument);
}

TEST_CASE("partner_token seeds the default store") {
    auto backend = ok_backend();
    Client client(base_options(backend));

    REQUIRE(client.token_store().token().value() == "PT");
}

TEST_CASE("a missing token fails before dispatch") {
    auto backend = ok_backend();
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>()));

    REQUIRE_THROWS_AS(client.doctors().branches(), AuthenticationError);
    REQUIRE(backend->requests.empty());
}

TEST_CASE("the token is read from the store on every call") {
    auto backend = ok_backend();
    auto store = std::make_shared<InMemoryTokenStore>(std::string("first"));
    Client client(base_options(backend, store));

    client.doctors().branches();
    store->set_token(std::string("second"));
    client.doctors().branches();

    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer first");
    REQUIRE(backend->requests.at(1).headers.at("Authorization") == "Bearer second");
}

TEST_CASE("request escape hatch defaults to the partner token") {
    auto backend = ok_backend();
    Client client(base_options(backend));

    auto data = client.request("GET", "/outher/customEndpoint");

    REQUIRE(data["ok"].get<bool>());
    REQUIRE(backend->requests.at(0).url == "http://localhost/outher/customEndpoint");
    REQUIRE(backend->requests.at(0).method == "GET");
    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer PT");
}

TEST_CASE("request escape hatch sends a public POST body and omits Authorization") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) { return json_resp(200, R"({"resultType":0,"data":{"id":7}})"); };
    Client client(base_options(backend));

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
    Client client(base_options(backend));

    REQUIRE_THROWS_AS(client.doctors().branches(), ValidationError);
}

TEST_CASE("maps 403 to AuthorizationError") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) { return json_resp(403, R"({"resultType":1})"); };
    Client client(base_options(backend));

    REQUIRE_THROWS_AS(client.doctors().branches(), AuthorizationError);
}

TEST_CASE("maps numeric 404 to NotFoundError") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(404, R"({"resultType":1,"errorType":1,"errorMessage":"Bilinmeyen"})");
    };
    Client client(base_options(backend));

    REQUIRE_THROWS_AS(client.doctors().branches(), NotFoundError);
}

TEST_CASE("an expired token is surfaced without retrying") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(401, R"({"resultType":4,"errorMessage":"You must log in."})");
    };
    auto store = std::make_shared<InMemoryTokenStore>(std::string("expired"));
    Client client(base_options(backend, store));

    REQUIRE_THROWS_AS(client.measures().last(reference_patient()), AuthenticationError);
    REQUIRE(backend->requests.size() == 1);
    // An expired token is kept: the caller may want to inspect it while
    // installing the replacement. Only a revoked one is cleared.
    REQUIRE(store->token().value() == "expired");
}

TEST_CASE("logout clears the store") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(200, R"({"resultType":2,"errorMessage":"logged out"})");
    };
    auto store = std::make_shared<InMemoryTokenStore>(std::string("revoked"));
    Client client(base_options(backend, store));

    REQUIRE_THROWS_AS(client.measures().last(reference_patient()), AuthenticationError);
    REQUIRE_FALSE(store->token().has_value());
}

TEST_CASE("transport failures become TransportError") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        HttpResponse r;
        r.transport_error = true;
        r.error_message = "boom";
        return r;
    };
    Client client(base_options(backend));

    REQUIRE_THROWS_AS(client.doctors().branches(), TransportError);
}
