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

std::shared_ptr<MockBackend> make_backend() {
    auto backend = std::make_shared<MockBackend>();
    backend->responder = [](const HttpRequest&) { return ok_resp(); };
    return backend;
}

ClientOptions partner_options(const std::shared_ptr<MockBackend>& backend) {
    ClientOptions o;
    o.base_url = "http://localhost";
    o.http_backend = backend;
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

TEST_CASE("every call sends the partner token") {
    auto backend = make_backend();
    Client client(partner_options(backend));

    client.doctors().branches();
    client.measures().last(reference_patient());

    for (const auto& request : backend->requests) {
        REQUIRE(request.headers.at("Authorization") == "Bearer PT");
    }
}

TEST_CASE("discovery paths") {
    auto backend = make_backend();
    Client client(partner_options(backend));

    client.doctors().locations();
    client.doctors().detail("42");
    client.laboratory().catalog();
    client.laboratory().catalog_detail("18246");
    client.slots().schedule("7", std::string("2026-08-01"));

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

    client.diets().list(patient, std::string("2"));
    client.measures().list(patient, "glucose", std::string("1"), 0);
    client.laboratory().results(patient);

    // The identity number must never leak into a URL — it would land in access
    // logs, proxy logs and error breadcrumbs.
    for (const auto& request : backend->requests) {
        REQUIRE(request.url.find("12345678901") == std::string::npos);
    }

    auto body = nlohmann::json::parse(backend->requests[0].body.value());
    REQUIRE(body["patient"]["identityNumber"] == "12345678901");
    REQUIRE(body["currentPage"] == "2");

    REQUIRE(backend->requests[1].url.find("/outher/measuresList/glucose") != std::string::npos);
}

TEST_CASE("lab result id round-trips with its suffix") {
    auto backend = make_backend();
    Client client(partner_options(backend));
    const Patient patient = reference_patient();

    client.laboratory().result_detail(patient, "1234-lab");
    auto body = nlohmann::json::parse(backend->requests[0].body.value());
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
    client.measures().add_list(writer, rows);
    client.measures().add(
        writer, "tension",
        nlohmann::json{{"date_time", "2026-06-17 09:00"}, {"hypertension", 120}, {"hypotension", 80}});
    client.measures().update(
        reference, "tension", "9",
        nlohmann::json{{"date_time", "2026-06-17 10:00"}, {"hypertension", 125}, {"hypotension", 85}});
    client.measures().delete_measure(reference, "tension", "9");

    REQUIRE(backend->requests[0].method == "POST");
    REQUIRE(backend->requests[0].url.find("/outher/measures") != std::string::npos);
    REQUIRE(backend->requests[1].method == "POST");
    REQUIRE(backend->requests[1].url.find("/outher/measure/tension") != std::string::npos);
    REQUIRE(backend->requests[2].method == "PUT");
    REQUIRE(backend->requests[3].method == "DELETE");

    // Measure fields are flattened alongside `patient`, matching the server shape.
    auto add_body = nlohmann::json::parse(backend->requests[1].body.value());
    REQUIRE(add_body["hypertension"] == 120);
    REQUIRE(add_body["patient"]["name"] == "Ada");

    auto delete_body = nlohmann::json::parse(backend->requests[3].body.value());
    REQUIRE(delete_body["id"] == "9");
}

TEST_CASE("appointment lifecycle") {
    auto backend = make_backend();
    Client client(partner_options(backend));
    const Patient user = write_patient();

    client.appointments().reserve("1", "2", user);
    client.appointments().create("h", "5");
    client.appointments().list("+905551112233");

    AppointmentLookup lookup;
    lookup.hash = "h";
    lookup.outher_process_id = "5";
    client.appointments().cancel_without_slot(lookup);

    REQUIRE(backend->requests[0].url.find("/outher/reservation") != std::string::npos);
    REQUIRE(backend->requests[1].url.find("/outher/appointment") != std::string::npos);
    REQUIRE(backend->requests[2].url.find("/outher/appointments") != std::string::npos);
    REQUIRE(backend->requests[3].method == "DELETE");

    auto body = nlohmann::json::parse(backend->requests[0].body.value());
    REQUIRE(body["slotId"] == "1");
    REQUIRE(body["user"]["surname"] == "Lovelace");
}

TEST_CASE("remaining appointment endpoints") {
    auto backend = make_backend();
    Client client(partner_options(backend));
    const Patient user = write_patient();

    client.appointments().check_doctor("2", 0);
    client.appointments().reserve_without_agreement("1", "2", user);
    client.appointments().instant_reserve(user);
    client.appointments().create_without_slot("2", "2026-08-01 09:00", "2026-08-01 09:30", user);

    AppointmentLookup lookup;
    lookup.hash = "h";
    lookup.outher_process_id = "5";
    client.appointments().info(lookup);

    const std::vector<std::string> expected = {
        "/outher/checkDoctor",
        "/outher/reservationWithoutAgreement",
        "/outher/instantReservation",
        "/outher/appointmentWithoutSlot",
        "/outher/appointmentInfo",
    };
    for (size_t i = 0; i < expected.size(); ++i) {
        REQUIRE(backend->requests[i].url.find(expected[i]) != std::string::npos);
    }
}

TEST_CASE("measures graph path and the legacy teusan shape") {
    auto backend = make_backend();
    Client client(partner_options(backend));

    Patient by_phone;
    by_phone.phone_number = "+905551112233";
    client.measures().graph(by_phone, "weight", 3);
    REQUIRE(backend->requests[0].url.find("/outher/measuresGraph/weight/3") != std::string::npos);

    std::vector<nlohmann::json> rows = {
        {{"type", "pulse"}, {"date_time", "2026-06-17 09:00"}, {"pulse", 72}},
    };
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    // Deliberately exercising the deprecated legacy endpoint.
    client.measures().health_information(std::string("12345678901"),
                                         std::string("+905551112233"), rows);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

    REQUIRE(backend->requests[1].url.find("/outher/healthInformation") != std::string::npos);
    auto body = nlohmann::json::parse(backend->requests[1].body.value());
    // No `patient` wrapper here — this endpoint predates that contract.
    REQUIRE_FALSE(body.contains("patient"));
    REQUIRE(body["identity"] == "12345678901");
}

TEST_CASE("diet detail and catalog detail paths") {
    auto backend = make_backend();
    Client client(partner_options(backend));
    const Patient reference = reference_patient();

    client.diets().detail(reference, "77");
    client.laboratory().catalog_detail("18246");

    REQUIRE(backend->requests[0].url.find("/outher/diet") != std::string::npos);
    auto body = nlohmann::json::parse(backend->requests[0].body.value());
    REQUIRE(body["listId"] == "77");
    REQUIRE(backend->requests[1].url.find("/outher/laboratoryCatalog/18246") != std::string::npos);
}
