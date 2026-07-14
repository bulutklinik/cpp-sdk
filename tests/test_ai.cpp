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

TEST_CASE("skin.analyze posts images to /patients/imageCheck with a bearer token") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(200, R"({"resultType":0,"data":{"status":[{"id":1,"label":"nevus","case_detail":"blob"}]}})");
    };
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    std::vector<nlohmann::json> images = {{{"image", "BASE64"}, {"branch_id", 42}}};
    auto data = client.skin().analyze(images);

    REQUIRE(data["status"][0]["label"] == "nevus");
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/imageCheck");
    REQUIRE(backend->requests.at(0).method == "POST");
    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer abc");

    auto body = nlohmann::json::parse(backend->requests.at(0).body.value());
    nlohmann::json expected = {{"images", {{{"image", "BASE64"}, {"branch_id", 42}}}}};
    REQUIRE(body == expected);
}

TEST_CASE("meals.analyze maps the input to the snake_case body with optional fields") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(200, R"({"resultType":0,"data":{"status":{"comment":"{}"}}})");
    };
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    MealInput input;
    input.image = "BASE64";
    input.portion_size = "custom";
    input.portion_grams = 300;
    input.meal_type = "lunch";
    input.note = "az yağlı";
    client.meals().analyze(input);

    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/imageAnalyzeMeal");
    REQUIRE(backend->requests.at(0).method == "POST");

    auto body = nlohmann::json::parse(backend->requests.at(0).body.value());
    nlohmann::json expected = {
        {"image", "BASE64"},
        {"portion_size", "custom"},
        {"meal_type", "lunch"},
        {"portion_grams", 300},
        {"note", "az yağlı"},
    };
    REQUIRE(body == expected);
}

TEST_CASE("meals.analyze omits optional fields when not set") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) {
        return json_resp(200, R"({"resultType":0,"data":{"status":{"comment":"{}"}}})");
    };
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    MealInput input;
    input.image = "BASE64";
    input.portion_size = "medium";
    input.meal_type = "snack";
    client.meals().analyze(input);

    auto body = nlohmann::json::parse(backend->requests.at(0).body.value());
    nlohmann::json expected = {
        {"image", "BASE64"},
        {"portion_size", "medium"},
        {"meal_type", "snack"},
    };
    REQUIRE(body == expected);
    REQUIRE_FALSE(body.contains("portion_grams"));
    REQUIRE_FALSE(body.contains("note"));
}
