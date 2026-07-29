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

HttpResponse ok_resp() {
    HttpResponse r;
    r.status = 200;
    r.body = R"({"resultType":0,"data":null})";
    return r;
}

/// A client with BOTH a patient access token and a partner token configured.
/// Partner calls must ignore the patient one.
std::shared_ptr<MockBackend> make_backend() {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) { return ok_resp(); };
    return backend;
}

ClientOptions partner_options(const std::shared_ptr<MockBackend>& backend) {
    ClientOptions o;
    o.base_url = "http://localhost";
    o.http_backend = backend;
    o.token_store = std::make_shared<InMemoryTokenStore>(std::string("PATIENT"), std::nullopt);
    o.partner_token = "PT";
    return o;
}

Patient reference_patient() {
    Patient p;
    p.identity_number = "12345678901";
    return p;
}

Patient write_patient() {
    Patient p;
    p.name = "Ada";
    p.surname = "Lovelace";
    p.phone_number = "+905551112233";
    return p;
}

}  // namespace

TEST_CASE("partner calls always send the partner token") {
    auto backend = make_backend();
    Client client(partner_options(backend));

    client.partner().doctors().branches();
    client.partner().measures().last(reference_patient());

    for (const auto& request : backend->requests) {
        REQUIRE(request.headers.at("Authorization") == "Bearer PT");
    }
}

TEST_CASE("patient surface keeps the patient token") {
    auto backend = make_backend();
    Client client(partner_options(backend));

    client.doctors().branches();

    REQUIRE(backend->requests[0].headers.at("Authorization") == "Bearer PATIENT");
    REQUIRE(backend->requests[0].url.find("/patients/allBranches") != std::string::npos);
}

TEST_CASE("partner discovery paths") {
    auto backend = make_backend();
    Client client(partner_options(backend));

    client.partner().doctors().locations();
    client.partner().doctors().detail("42");
    client.partner().laboratory().catalog();
    client.partner().laboratory().catalog_detail("18246");
    client.partner().slots().schedule("7", std::string("2026-08-01"));

    const std::vector<std::string> expected = {
        "/outher/locations",
        "/outher/doctorInfos/42",
        "/outher/laboratoryCatalog",
        "/outher/laboratoryCatalog/18246",
        "/outher/doctorSlots",
    };
    for (size_t i = 0; i < expected.size(); ++i) {
        REQUIRE(backend->requests[i].url.find(expected[i]) != std::string::npos);
    }
}

TEST_CASE("patient reference travels in the body, not the path") {
    auto backend = make_backend();
    Client client(partner_options(backend));
    const Patient patient = reference_patient();

    client.partner().diets().list(patient, std::string("2"));
    client.partner().measures().list(patient, "glucose", std::string("1"), 0);
    client.partner().laboratory().results(patient);

    // The identity number must never leak into a URL — it would land in access
    // logs, proxy logs and error breadcrumbs.
    for (const auto& request : backend->requests) {
        REQUIRE(request.url.find("12345678901") == std::string::npos);
    }

    auto body = nlohmann::json::parse(backend->requests[0].body);
    REQUIRE(body["patient"]["identityNumber"] == "12345678901");
    REQUIRE(body["currentPage"] == "2");

    REQUIRE(backend->requests[1].url.find("/outher/measuresList/glucose") != std::string::npos);
}

TEST_CASE("lab result id round-trips with its suffix") {
    auto backend = make_backend();
    Client client(partner_options(backend));
    const Patient patient = reference_patient();

    client.partner().laboratory().result_detail(patient, "1234-lab");
    auto body = nlohmann::json::parse(backend->requests[0].body);
    REQUIRE(body["testId"] == "1234-lab");
}

TEST_CASE("measure write verbs and paths") {
    auto backend = make_backend();
    Client client(partner_options(backend));
    const Patient writer = write_patient();
    const Patient reference = reference_patient();

    std::vector<nlohmann::json> rows = {
        {{"type", "pulse"}, {"date_time", "2026-06-17 09:00"}, {"pulse", 72}},
    };
    client.partner().measures().add_list(writer, rows);
    client.partner().measures().add(
        writer, "tension",
        nlohmann::json{{"date_time", "2026-06-17 09:00"}, {"hypertension", 120}, {"hypotension", 80}});
    client.partner().measures().update(
        reference, "tension", "9",
        nlohmann::json{{"date_time", "2026-06-17 10:00"}, {"hypertension", 125}, {"hypotension", 85}});
    client.partner().measures().delete_measure(reference, "tension", "9");

    REQUIRE(backend->requests[0].method == "POST");
    REQUIRE(backend->requests[0].url.find("/outher/measures") != std::string::npos);
    REQUIRE(backend->requests[1].method == "POST");
    REQUIRE(backend->requests[1].url.find("/outher/measure/tension") != std::string::npos);
    REQUIRE(backend->requests[2].method == "PUT");
    REQUIRE(backend->requests[3].method == "DELETE");

    // Measure fields are flattened alongside `patient`, matching the server shape.
    auto add_body = nlohmann::json::parse(backend->requests[1].body);
    REQUIRE(add_body["hypertension"] == 120);
    REQUIRE(add_body["patient"]["name"] == "Ada");

    auto delete_body = nlohmann::json::parse(backend->requests[3].body);
    REQUIRE(delete_body["id"] == "9");
}

TEST_CASE("partner appointment lifecycle") {
    auto backend = make_backend();
    Client client(partner_options(backend));
    const Patient user = write_patient();

    client.partner().appointments().reserve("1", "2", user);
    client.partner().appointments().create("h", "5");
    client.partner().appointments().list("+905551112233");

    AppointmentLookup lookup;
    lookup.hash = "h";
    lookup.outher_process_id = "5";
    client.partner().appointments().cancel_without_slot(lookup);

    REQUIRE(backend->requests[0].url.find("/outher/reservation") != std::string::npos);
    REQUIRE(backend->requests[1].url.find("/outher/appointment") != std::string::npos);
    REQUIRE(backend->requests[2].url.find("/outher/appointments") != std::string::npos);
    REQUIRE(backend->requests[3].method == "DELETE");

    auto body = nlohmann::json::parse(backend->requests[0].body);
    REQUIRE(body["slotId"] == "1");
    REQUIRE(body["user"]["surname"] == "Lovelace");
}
