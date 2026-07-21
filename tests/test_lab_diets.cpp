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

std::shared_ptr<MockBackend> ok_backend(const std::string& data = R"({"resultType":0,"data":{}})") {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [data](const HttpRequest&) { return json_resp(200, data); };
    return backend;
}

}  // namespace

TEST_CASE("laboratory.results GETs the list with a page segment and a bearer token") {
    auto backend = ok_backend(R"({"resultType":0,"data":{"foundTestsCount":0,"foundTests":[]}})");
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    client.laboratory().results(std::string("2"));

    REQUIRE(backend->requests.at(0).method == "GET");
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/userLabTestList/2");
    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer abc");
    REQUIRE_FALSE(backend->requests.at(0).body.has_value());
}

TEST_CASE("laboratory.results omits the page segment when page is not set") {
    auto backend = ok_backend(R"({"resultType":0,"data":{"foundTestsCount":0,"foundTests":[]}})");
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    client.laboratory().results();

    REQUIRE(backend->requests.at(0).method == "GET");
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/userLabTestList");
}

TEST_CASE("laboratory.result_detail interpolates a string test id verbatim (incl. -lab suffix)") {
    auto backend = ok_backend();
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    client.laboratory().result_detail("4821-lab");

    REQUIRE(backend->requests.at(0).method == "GET");
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/userLabTestDetail/4821-lab");
    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer abc");
}

TEST_CASE("laboratory.catalog GETs the orderable test catalog") {
    auto backend = ok_backend();
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    client.laboratory().catalog();

    REQUIRE(backend->requests.at(0).method == "GET");
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/allLaboratoryTests");
}

TEST_CASE("laboratory.catalog_detail GETs a single catalog group by id") {
    auto backend = ok_backend();
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    client.laboratory().catalog_detail("17");

    REQUIRE(backend->requests.at(0).method == "GET");
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/laboratoryTestDetail/17");
}

TEST_CASE("laboratory.order POSTs the three ids to addNewLaboratoryTest") {
    auto backend = ok_backend(R"({"resultType":0,"data":{"preOrderId":99}})");
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    LabOrderInput input;
    input.test_id = "12";
    input.address_id = "34";
    input.laboratory_id = "56";
    auto data = client.laboratory().order(input);

    REQUIRE(data["preOrderId"] == 99);
    REQUIRE(backend->requests.at(0).method == "POST");
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/addNewLaboratoryTest");
    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer abc");

    auto body = nlohmann::json::parse(backend->requests.at(0).body.value());
    nlohmann::json expected = {
        {"testId", "12"},
        {"addressId", "34"},
        {"laboratoryId", "56"},
    };
    REQUIRE(body == expected);
}

TEST_CASE("diets.list GETs the diet lists with a page segment and a bearer token") {
    auto backend = ok_backend(R"({"resultType":0,"data":{"foundDietsCount":0,"foundDiets":[]}})");
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    client.diets().list(std::string("3"));

    REQUIRE(backend->requests.at(0).method == "GET");
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/dietLists/3");
    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer abc");
}

TEST_CASE("diets.list omits the page segment when page is not set") {
    auto backend = ok_backend(R"({"resultType":0,"data":{"foundDietsCount":0,"foundDiets":[]}})");
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    client.diets().list();

    REQUIRE(backend->requests.at(0).method == "GET");
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/dietLists");
}

TEST_CASE("diets.detail GETs one diet list by list id") {
    auto backend = ok_backend();
    Client client(base_options(backend, std::make_shared<InMemoryTokenStore>(std::string("abc"), std::nullopt)));

    client.diets().detail("501");

    REQUIRE(backend->requests.at(0).method == "GET");
    REQUIRE(backend->requests.at(0).url == "http://localhost/patients/diet/501");
    REQUIRE(backend->requests.at(0).headers.at("Authorization") == "Bearer abc");
}
