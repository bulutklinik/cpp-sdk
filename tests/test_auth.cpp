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

const char* kTokens = R"({"resultType":0,"data":{"access_token":"AT","refresh_token":"RT"}})";

/// Options carrying portal client credentials and pointing at the mock backend.
ClientOptions credentialed(const std::shared_ptr<MockBackend>& backend,
                           std::shared_ptr<TokenStore> store = nullptr) {
    ClientOptions o;
    o.base_url = "http://localhost";
    o.http_backend = backend;
    o.client_id = "cid";
    o.client_secret = "csecret";
    o.token_store = store ? std::move(store) : std::make_shared<InMemoryTokenStore>();
    return o;
}

Patient reference_patient() {
    Patient p;
    p.identity_number = "12345678901";
    return p;
}

/// A path suffix match — the mock base URL has no query string to worry about.
bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

ConnectInput login(const std::string& user = "svc@app.bulutklinik",
                   const std::string& password = "hunter2") {
    ConnectInput in;
    in.api_user_name = user;
    in.api_user_password = password;
    return in;
}

/// A store written against spec 1.0.x: access token only.
class LegacyStore : public TokenStore {
public:
    std::optional<std::string> token() const override { return token_; }
    void set_token(const std::optional<std::string>& token) override { token_ = token; }
    void clear() override { token_.reset(); }

private:
    std::optional<std::string> token_;
};

}  // namespace

TEST_CASE("connect posts the portal credentials and stores both tokens") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) { return json_resp(200, kTokens); };
    auto store = std::make_shared<InMemoryTokenStore>();
    Client client(credentialed(backend, store));

    LoginResult result = client.auth().connect(login());

    REQUIRE_FALSE(result.two_factor_required);
    REQUIRE(backend->requests.at(0).url == "http://localhost/general/connectApi");
    // The login call is public — it is what produces the credential.
    REQUIRE(backend->requests.at(0).headers.find("Authorization") == backend->requests.at(0).headers.end());
    auto body = nlohmann::json::parse(backend->requests.at(0).body.value());
    REQUIRE(body["apiClientId"] == "cid");
    REQUIRE(body["apiSecretKey"] == "csecret");
    REQUIRE(body["apiUserName"] == "svc@app.bulutklinik");
    REQUIRE(body["loginMode"] == "email");
    REQUIRE(store->token().value() == "AT");
    REQUIRE(store->refresh_token().value() == "RT");
}

TEST_CASE("connect surfaces a two-factor challenge as a result") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(200, R"({"resultType":0,"data":{"response":"BLOB"}})");
    };
    auto store = std::make_shared<InMemoryTokenStore>();
    Client client(credentialed(backend, store));

    LoginResult result = client.auth().connect(login());

    REQUIRE(result.two_factor_required);
    REQUIRE(result.two_factor_response.value() == "BLOB");
    REQUIRE_FALSE(store->token().has_value());
}

TEST_CASE("connect requires a client id and secret") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) { return json_resp(200, kTokens); };
    ClientOptions o;
    o.base_url = "http://localhost";
    o.http_backend = backend;
    o.partner_token = "PT";
    Client client(o);

    REQUIRE_THROWS_AS(client.auth().connect(login()), std::invalid_argument);
    REQUIRE(backend->requests.empty());
}

TEST_CASE("refreshes once and retries with the new token") {
    int data_calls = 0;
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [&data_calls](const HttpRequest& req) {
        if (ends_with(req.url, "/general/refreshApi")) {
            return json_resp(200, R"({"resultType":0,"data":{"access_token":"AT2","refresh_token":"RT2"}})");
        }
        ++data_calls;
        return data_calls == 1 ? json_resp(401, R"({"resultType":4})")
                               : json_resp(200, R"({"resultType":0,"data":{"ok":true}})");
    };
    auto store = std::make_shared<InMemoryTokenStore>(std::string("AT"), std::string("RT"));
    Client client(credentialed(backend, store));

    auto data = client.measures().last(reference_patient());

    REQUIRE(data["ok"].get<bool>());
    REQUIRE(store->token().value() == "AT2");
    REQUIRE(store->refresh_token().value() == "RT2");
    auto refresh_body = nlohmann::json::parse(backend->requests.at(1).body.value());
    REQUIRE(refresh_body["refreshToken"] == "RT");
    REQUIRE(refresh_body["clientId"] == "cid");
    REQUIRE(refresh_body["clientSecretKey"] == "csecret");
    REQUIRE(backend->requests.at(2).headers.at("Authorization") == "Bearer AT2");
}

TEST_CASE("retries at most once and clears the store when the refresh fails") {
    int refresh_calls = 0;
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [&refresh_calls](const HttpRequest& req) {
        if (ends_with(req.url, "/general/refreshApi")) {
            ++refresh_calls;
            return json_resp(401, R"({"resultType":1})");
        }
        return json_resp(401, R"({"resultType":4})");
    };
    auto store = std::make_shared<InMemoryTokenStore>(std::string("AT"), std::string("RT"));
    Client client(credentialed(backend, store));

    REQUIRE_THROWS_AS(client.measures().last(reference_patient()), AuthenticationError);
    REQUIRE(refresh_calls == 1);
    REQUIRE_FALSE(store->token().has_value());
}

TEST_CASE("no refresh is attempted without a refresh token") {
    int refresh_calls = 0;
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [&refresh_calls](const HttpRequest& req) {
        if (ends_with(req.url, "/general/refreshApi")) {
            ++refresh_calls;
        }
        return json_resp(401, R"({"resultType":4})");
    };
    auto store = std::make_shared<InMemoryTokenStore>(std::string("AT"));
    Client client(credentialed(backend, store));

    REQUIRE_THROWS_AS(client.doctors().branches(), AuthenticationError);
    REQUIRE(refresh_calls == 0);
    REQUIRE(backend->requests.size() == 1);
}

TEST_CASE("a store without refresh support still refreshes in memory") {
    int data_calls = 0;
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [&data_calls](const HttpRequest& req) {
        if (ends_with(req.url, "/general/connectApi")) {
            return json_resp(200, kTokens);
        }
        if (ends_with(req.url, "/general/refreshApi")) {
            return json_resp(200, R"({"resultType":0,"data":{"access_token":"AT2"}})");
        }
        ++data_calls;
        return data_calls == 1 ? json_resp(401, R"({"resultType":4})")
                               : json_resp(200, R"({"resultType":0,"data":{"ok":true}})");
    };
    auto legacy = std::make_shared<LegacyStore>();
    Client client(credentialed(backend, legacy));

    client.auth().connect(login());
    REQUIRE(client.refresh_token().value() == "RT");

    auto data = client.measures().last(reference_patient());

    REQUIRE(data["ok"].get<bool>());
    REQUIRE(legacy->token().value() == "AT2");
}

TEST_CASE("disconnect sends an empty body and clears the store") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) { return json_resp(200, R"({"resultType":0,"data":null})"); };
    auto store = std::make_shared<InMemoryTokenStore>(std::string("AT"), std::string("RT"));
    Client client(credentialed(backend, store));

    client.auth().disconnect();

    REQUIRE(backend->requests.at(0).url == "http://localhost/general/disconnectApi");
    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer AT");
    // The device-cleanup fields are deliberately not sent: the server's `device`
    // mapping has no default branch.
    REQUIRE(backend->requests.at(0).body.value() == "{}");
    REQUIRE_FALSE(store->token().has_value());
    REQUIRE_FALSE(store->refresh_token().has_value());
}
