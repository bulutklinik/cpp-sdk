#include <bulutklinik/bulutklinik.hpp>

#include <cctype>

namespace bulutklinik {
namespace {

std::string api_root_for(Environment env) {
    switch (env) {
        case Environment::Production:
            return "https://api.bulutklinik.com/api";
        case Environment::Test:
            return "https://apitest.bulutklinik.com/api";
        case Environment::Local:
            return "https://api-bulutklinik.test/api";
    }
    return "https://api.bulutklinik.com/api";
}

std::string version_segment(ApiVersion version) {
    return version == ApiVersion::V4 ? "v4" : "v3";
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::optional<int> result_type_of(const nlohmann::json& env) {
    auto it = env.find("resultType");
    if (it != env.end() && it->is_number_integer()) {
        return it->get<int>();
    }
    return std::nullopt;
}

nlohmann::json parse_envelope(const std::string& text) {
    if (text.empty()) {
        return nlohmann::json::object();
    }
    nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        nlohmann::json obj = nlohmann::json::object();
        obj["errorMessage"] = text;
        return obj;
    }
    if (!parsed.is_object()) {
        nlohmann::json obj = nlohmann::json::object();
        obj["data"] = parsed;
        return obj;
    }
    return parsed;
}

[[noreturn]] void throw_api_error(const std::string& method, const std::string& path, int status,
                                  const nlohmann::json& env, const std::optional<std::string>& retry_after) {
    std::string message;
    if (env.contains("errorMessage") && env["errorMessage"].is_string()) {
        message = env["errorMessage"].get<std::string>();
    }
    if (message.empty()) {
        message = "Bulutklinik API request failed: " + method + " " + path + " (HTTP " + std::to_string(status) + ")";
    }

    std::optional<int> result_type = result_type_of(env);
    nlohmann::json error_type = env.contains("errorType") ? env["errorType"] : nlohmann::json(nullptr);
    nlohmann::json data = env.contains("data") ? env["data"] : nlohmann::json(nullptr);
    std::optional<int> retry;
    if (retry_after) {
        try {
            retry = std::stoi(*retry_after);
        } catch (...) {
            retry = std::nullopt;
        }
    }

    const bool is_validation =
        (error_type.is_string() && iequals(error_type.get<std::string>(), "validation")) || status == 422;

    if (result_type && *result_type == 2) {
        throw AuthenticationError(message, status, result_type, error_type, data, method, path, retry);
    }
    // resultType 4 reaches here only when the silent refresh could not run or
    // failed — the transport retries first (DESIGN.md 5.4).
    if (result_type && *result_type == 4) {
        throw AuthenticationError(message + " The access token is expired and could not be refreshed -"
                                            " call auth().connect() again.",
                                  status, result_type, error_type, data, method, path, retry);
    }
    if (is_validation) {
        throw ValidationError(message, status, result_type, error_type, data, method, path, retry);
    }
    switch (status) {
        case 401:
            throw AuthenticationError(message, status, result_type, error_type, data, method, path, retry);
        case 403:
            throw AuthorizationError(message, status, result_type, error_type, data, method, path, retry);
        case 404:
            throw NotFoundError(message, status, result_type, error_type, data, method, path, retry);
        case 429:
            throw RateLimitError(message, status, result_type, error_type, data, method, path, retry);
        default:
            throw ApiError(message, status, result_type, error_type, data, method, path, retry);
    }
}

}  // namespace

namespace detail {

enum class AuthMode { Public, Partner };

/// Builds requests, unwraps the response envelope and maps failures to typed
/// exceptions.
///
/// On a 401 / resultType 4 it refreshes once and retries the original request;
/// the error surfaces only when there is no refresh token or the refresh itself
/// fails. Concurrent refreshes are serialised, and a caller that finds the token
/// already rotated skips straight to the retry.
class Transport {
public:
    Transport(std::shared_ptr<HttpBackend> backend, std::string base_url, std::string lang,
              std::shared_ptr<TokenStore> token_store, std::optional<std::string> client_id,
              std::optional<std::string> client_secret, long timeout_ms)
        : backend_(std::move(backend)),
          base_url_(std::move(base_url)),
          lang_(std::move(lang)),
          token_store_(std::move(token_store)),
          client_id_(std::move(client_id)),
          client_secret_(std::move(client_secret)),
          timeout_ms_(timeout_ms) {}

    TokenStore& token_store() { return *token_store_; }
    const std::optional<std::string>& client_id() const { return client_id_; }
    const std::optional<std::string>& client_secret() const { return client_secret_; }

    /// Persist a freshly minted token pair.
    void set_tokens(const std::string& access, const std::optional<std::string>& refresh) {
        token_store_->set_token(access);
        if (auto* refreshable = dynamic_cast<RefreshTokenStore*>(token_store_.get())) {
            refreshable->set_refresh_token(refresh);
        } else {
            std::lock_guard<std::mutex> lock(fallback_mutex_);
            fallback_refresh_token_ = refresh;
        }
    }

    std::optional<std::string> refresh_token() const {
        if (auto* refreshable = dynamic_cast<RefreshTokenStore*>(token_store_.get())) {
            return refreshable->refresh_token();
        }
        std::lock_guard<std::mutex> lock(fallback_mutex_);
        return fallback_refresh_token_;
    }

    void clear_tokens() {
        {
            std::lock_guard<std::mutex> lock(fallback_mutex_);
            fallback_refresh_token_.reset();
        }
        token_store_->clear();
    }

    /// Force a refresh using the stored refresh token. Throws on failure.
    void refresh() {
        if (!try_refresh(std::nullopt)) {
            throw AuthenticationError("bulutklinik: token refresh failed", 401, std::nullopt,
                                      nlohmann::json(nullptr), nlohmann::json(nullptr), "POST",
                                      "/general/refreshApi", std::nullopt);
        }
    }

    nlohmann::json send(const std::string& method, const std::string& path, AuthMode auth,
                        const nlohmann::json& body = nlohmann::json(),
                        const std::optional<std::string>& lang = std::nullopt,
                        bool is_retry = false) {
        std::optional<std::string> stale_access;
        if (auth == AuthMode::Partner) {
            stale_access = token_store_->token();
        }

        Dispatch d = dispatch(method, path, auth, body, lang);
        std::optional<int> result_type = result_type_of(d.envelope);

        if (d.status >= 200 && d.status < 300 && result_type && *result_type == 0) {
            return d.envelope.contains("data") ? d.envelope["data"] : nlohmann::json(nullptr);
        }

        const bool expired = d.status == 401 || (result_type && *result_type == 4);
        if (auth == AuthMode::Partner && expired && !is_retry && try_refresh(stale_access)) {
            return send(method, path, auth, body, lang, true);
        }

        // A revoked session is worth forgetting; a merely expired access token is
        // not, since the caller may want to inspect it.
        if (result_type && *result_type == 2) {
            clear_tokens();
        }
        throw_api_error(method, path, d.status, d.envelope, d.retry_after);
    }

private:
    /// Returns true when a usable access token is in place afterwards.
    ///
    /// `stale_access` is the token the failing request actually used: if the store
    /// no longer holds it, another thread already refreshed and the caller should
    /// simply retry.
    bool try_refresh(const std::optional<std::string>& stale_access) {
        std::lock_guard<std::mutex> lock(refresh_mutex_);
        if (stale_access && token_store_->token() != stale_access) {
            return true;
        }

        std::optional<std::string> refresh = refresh_token();
        if (!refresh || refresh->empty() || !client_id_ || client_id_->empty() || !client_secret_ ||
            client_secret_->empty()) {
            return false;
        }

        nlohmann::json body = {
            {"refreshToken", *refresh},
            {"clientId", *client_id_},
            {"clientSecretKey", *client_secret_},
        };
        Dispatch d = dispatch("POST", "/general/refreshApi", AuthMode::Public, body, std::nullopt);
        std::optional<int> result_type = result_type_of(d.envelope);
        nlohmann::json data = d.envelope.contains("data") ? d.envelope["data"] : nlohmann::json(nullptr);

        if (d.status < 200 || d.status >= 300 || !result_type || *result_type != 0 || !data.is_object() ||
            !data.contains("access_token") || !data["access_token"].is_string()) {
            clear_tokens();
            return false;
        }

        std::optional<std::string> rotated = refresh;
        if (data.contains("refresh_token") && data["refresh_token"].is_string()) {
            rotated = data["refresh_token"].get<std::string>();
        }
        set_tokens(data["access_token"].get<std::string>(), rotated);
        return true;
    }

    struct Dispatch {
        int status;
        nlohmann::json envelope;
        std::optional<std::string> retry_after;
    };

    Dispatch dispatch(const std::string& method, const std::string& path, AuthMode auth,
                      const nlohmann::json& body, const std::optional<std::string>& lang = std::nullopt) {
        std::optional<std::string> token;
        if (auth == AuthMode::Partner) {
            token = token_store_->token();
            if (!token || token->empty()) {
                // Dispatching anyway would only come back as an opaque 401.
                throw AuthenticationError("No access token available. Call auth().connect(), or set"
                                          " partner_token.",
                                          0, std::nullopt,
                                          nlohmann::json(nullptr), nlohmann::json(nullptr), method, path,
                                          std::nullopt);
            }
        }

        HttpRequest req;
        req.method = method;
        req.url = base_url_ + path;
        req.timeout_ms = timeout_ms_;
        req.headers["Accept"] = "application/json";
        req.headers["lang"] = lang ? *lang : lang_;
        if (!body.is_null() && method != "GET") {
            req.body = body.dump();
            req.headers["Content-Type"] = "application/json";
        }
        if (token) {
            req.headers["Authorization"] = "Bearer " + *token;
        }

        HttpResponse resp = backend_->send(req);
        if (resp.transport_error) {
            throw TransportError("bulutklinik: " + method + " " + path + ": " + resp.error_message);
        }

        std::optional<std::string> retry_after;
        auto it = resp.headers.find("retry-after");
        if (it != resp.headers.end()) {
            retry_after = it->second;
        }
        return Dispatch{resp.status, parse_envelope(resp.body), retry_after};
    }

    std::shared_ptr<HttpBackend> backend_;
    std::string base_url_;
    std::string lang_;
    std::shared_ptr<TokenStore> token_store_;
    std::optional<std::string> client_id_;
    std::optional<std::string> client_secret_;
    long timeout_ms_;
    std::mutex refresh_mutex_;
    /// Used only when the injected store cannot persist the refresh token.
    mutable std::mutex fallback_mutex_;
    std::optional<std::string> fallback_refresh_token_;
};

}  // namespace detail

// ---------------- Client ----------------

Client::Client(ClientOptions options) {
    // Either the literal or the store is the source of truth for the credential.
    // Guessing which one the caller meant is how credential bugs get shipped.
    if (options.partner_token && options.token_store) {
        throw std::invalid_argument(
            "bulutklinik: set either partner_token or token_store, not both. "
            "Seed your own store with the token if you need custom persistence.");
    }

    std::string base = options.base_url
                           ? *options.base_url
                           : api_root_for(options.environment) + "/" + version_segment(options.api_version);
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    auto store = options.token_store ? options.token_store
                                     : std::make_shared<InMemoryTokenStore>(options.partner_token);
    auto backend = options.http_backend ? options.http_backend : std::make_shared<CprHttpBackend>();
    transport_ = std::make_shared<detail::Transport>(backend, base, options.lang, store, options.client_id,
                                                    options.client_secret, options.timeout_ms);
}

Client::~Client() = default;

AuthResource Client::auth() { return AuthResource(transport_.get()); }
DoctorsResource Client::doctors() { return DoctorsResource(transport_.get()); }
SlotsResource Client::slots() { return SlotsResource(transport_.get()); }
AppointmentsResource Client::appointments() { return AppointmentsResource(transport_.get()); }
MeasuresResource Client::measures() { return MeasuresResource(transport_.get()); }
LaboratoryResource Client::laboratory() { return LaboratoryResource(transport_.get()); }
DietsResource Client::diets() { return DietsResource(transport_.get()); }

nlohmann::json Client::request(const std::string& method, const std::string& path,
                               const RequestOptions& options) {
    detail::AuthMode auth =
        options.auth == Auth::Public ? detail::AuthMode::Public : detail::AuthMode::Partner;
    return transport_->send(method, path, auth, options.body, options.lang);
}

TokenStore& Client::token_store() { return transport_->token_store(); }

std::optional<std::string> Client::refresh_token() const { return transport_->refresh_token(); }

// ---------------- resources ----------------

namespace {

/// Adds an optional string only when it was set, so unset fields never reach the
/// wire as nulls the server would have to interpret.
void put_opt(nlohmann::json& target, const char* key, const std::optional<std::string>& value) {
    if (value) target[key] = *value;
}

/// Body shared by every patient-scoped read: `{"patient": {...}, ...}`.
nlohmann::json patient_body(const Patient& patient) {
    nlohmann::json body;
    body["patient"] = patient.to_json();
    return body;
}

/// Flattens measure fields next to the patient reference, optionally adding an
/// id. The server expects the columns at the top level, not nested.
nlohmann::json measure_body(const Patient& patient, const nlohmann::json* fields,
                            const std::optional<std::string>& id) {
    nlohmann::json body = patient_body(patient);
    if (id) body["id"] = *id;
    if (fields && fields->is_object()) {
        for (auto it = fields->begin(); it != fields->end(); ++it) body[it.key()] = it.value();
    }
    return body;
}

}  // namespace

nlohmann::json Patient::to_json() const {
    nlohmann::json out = nlohmann::json::object();
    put_opt(out, "name", name);
    put_opt(out, "surname", surname);
    put_opt(out, "phoneNumber", phone_number);
    put_opt(out, "identityNumber", identity_number);
    put_opt(out, "email", email);
    put_opt(out, "birthdate", birthdate);
    put_opt(out, "nationality", nationality);
    if (price) out["price"] = *price;
    return out;
}

nlohmann::json AppointmentLookup::to_json() const {
    nlohmann::json out = nlohmann::json::object();
    put_opt(out, "hash", hash);
    put_opt(out, "outherProcessId", outher_process_id);
    put_opt(out, "doctorId", doctor_id);
    put_opt(out, "appointmentDate", appointment_date);
    if (is_outher_doctor) out["isOutherDoctor"] = *is_outher_doctor;
    return out;
}

// ---------------- AuthResource ----------------

LoginResult AuthResource::connect(const ConnectInput& input) {
    std::optional<std::string> id = input.client_id ? input.client_id : t_->client_id();
    std::optional<std::string> secret = input.client_secret ? input.client_secret : t_->client_secret();
    if (!id || id->empty() || !secret || secret->empty()) {
        throw std::invalid_argument(
            "bulutklinik: client_id and client_secret are required - pass them to connect() or set them "
            "on ClientOptions.");
    }

    nlohmann::json body = {
        {"apiClientId", *id},
        {"apiSecretKey", *secret},
        {"apiUserName", input.api_user_name},
        {"apiUserPassword", input.api_user_password},
        {"loginMode", input.login_mode.empty() ? std::string("email") : input.login_mode},
    };
    nlohmann::json data = t_->send("POST", "/general/connectApi", detail::AuthMode::Public, body);

    if (data.is_object() && data.contains("access_token") && data["access_token"].is_string()) {
        std::optional<std::string> refresh;
        if (data.contains("refresh_token") && data["refresh_token"].is_string()) {
            refresh = data["refresh_token"].get<std::string>();
        }
        t_->set_tokens(data["access_token"].get<std::string>(), refresh);

        LoginResult result;
        if (data.contains("password_policy")) {
            result.password_policy = data["password_policy"];
        }
        return result;
    }

    LoginResult result;
    result.two_factor_required = true;
    if (data.is_object() && data.contains("response") && data["response"].is_string()) {
        result.two_factor_response = data["response"].get<std::string>();
    }
    return result;
}

void AuthResource::refresh() { t_->refresh(); }

void AuthResource::disconnect() {
    t_->send("POST", "/general/disconnectApi", detail::AuthMode::Partner, nlohmann::json::object());
    t_->clear_tokens();
}

// ---------------- DoctorsResource ----------------

nlohmann::json DoctorsResource::search(const nlohmann::json& search_params, int current_page,
                                              const std::vector<std::string>& order_params) {
    nlohmann::json body = {
        {"searchParams", search_params},
        {"orderParams", order_params},
        {"currentPage", current_page},
    };
    return t_->send("POST", "/outher/search", detail::AuthMode::Partner, body);
}

nlohmann::json DoctorsResource::branches() {
    return t_->send("GET", "/outher/branches", detail::AuthMode::Partner);
}

nlohmann::json DoctorsResource::detail(const std::string& doctor_id) {
    return t_->send("GET", "/outher/doctorInfos/" + doctor_id, detail::AuthMode::Partner);
}

nlohmann::json DoctorsResource::locations() {
    return t_->send("GET", "/outher/locations", detail::AuthMode::Partner);
}

// ---------------- SlotsResource ----------------

nlohmann::json SlotsResource::schedule(const std::string& doctor_id,
                                              const std::optional<std::string>& schedule_date,
                                              std::optional<int> schedule_step,
                                              std::optional<int> schedule_page) {
    nlohmann::json body;
    body["doctorId"] = doctor_id;
    put_opt(body, "scheduleDate", schedule_date);
    if (schedule_step) body["scheduleStep"] = *schedule_step;
    if (schedule_page) body["schedulePage"] = *schedule_page;
    return t_->send("POST", "/outher/doctorSlots", detail::AuthMode::Partner, body);
}

// ---------------- AppointmentsResource ----------------

nlohmann::json AppointmentsResource::reserve(const std::string& slot_id,
                                                    const std::string& doctor_id,
                                                    const Patient& user) {
    nlohmann::json body = {{"slotId", slot_id}, {"doctorId", doctor_id}, {"user", user.to_json()}};
    return t_->send("POST", "/outher/reservation", detail::AuthMode::Partner, body);
}

nlohmann::json AppointmentsResource::reserve_without_agreement(const std::string& slot_id,
                                                                      const std::string& doctor_id,
                                                                      const Patient& user) {
    nlohmann::json body = {{"slotId", slot_id}, {"doctorId", doctor_id}, {"user", user.to_json()}};
    return t_->send("POST", "/outher/reservationWithoutAgreement", detail::AuthMode::Partner, body);
}

nlohmann::json AppointmentsResource::instant_reserve(const Patient& user) {
    nlohmann::json body = {{"user", user.to_json()}};
    return t_->send("POST", "/outher/instantReservation", detail::AuthMode::Partner, body);
}

nlohmann::json AppointmentsResource::create(const std::string& hash,
                                                   const std::string& outher_process_id) {
    nlohmann::json body = {{"hash", hash}, {"outherProcessId", outher_process_id}};
    return t_->send("POST", "/outher/appointment", detail::AuthMode::Partner, body);
}

nlohmann::json AppointmentsResource::create_without_slot(const std::string& doctor_id,
                                                                const std::string& start_date,
                                                                const std::string& finish_date,
                                                                const Patient& user,
                                                                std::optional<int> is_outher_doctor) {
    nlohmann::json body = {
        {"doctorId", doctor_id},
        {"startDate", start_date},
        {"finishDate", finish_date},
        {"user", user.to_json()},
    };
    if (is_outher_doctor) body["isOutherDoctor"] = *is_outher_doctor;
    return t_->send("POST", "/outher/appointmentWithoutSlot", detail::AuthMode::Partner, body);
}

nlohmann::json AppointmentsResource::cancel_without_slot(const AppointmentLookup& lookup) {
    return t_->send("DELETE", "/outher/appointmentWithoutSlot", detail::AuthMode::Partner,
                    lookup.to_json());
}

nlohmann::json AppointmentsResource::list(const std::string& phone_number,
                                                 const std::optional<std::string>& page,
                                                 const std::optional<std::string>& type) {
    nlohmann::json body;
    body["phoneNumber"] = phone_number;
    put_opt(body, "page", page);
    put_opt(body, "type", type);
    return t_->send("POST", "/outher/appointments", detail::AuthMode::Partner, body);
}

nlohmann::json AppointmentsResource::info(const AppointmentLookup& lookup) {
    return t_->send("POST", "/outher/appointmentInfo", detail::AuthMode::Partner, lookup.to_json());
}

nlohmann::json AppointmentsResource::check_doctor(const std::string& doctor_id,
                                                         int is_outher_doctor) {
    nlohmann::json body = {{"doctorId", doctor_id}, {"isOutherDoctor", is_outher_doctor}};
    return t_->send("POST", "/outher/checkDoctor", detail::AuthMode::Partner, body);
}

// ---------------- DietsResource ----------------

nlohmann::json DietsResource::list(const Patient& patient,
                                          const std::optional<std::string>& page) {
    nlohmann::json body = patient_body(patient);
    put_opt(body, "currentPage", page);
    return t_->send("POST", "/outher/dietLists", detail::AuthMode::Partner, body);
}

nlohmann::json DietsResource::detail(const Patient& patient, const std::string& list_id) {
    nlohmann::json body = patient_body(patient);
    body["listId"] = list_id;
    return t_->send("POST", "/outher/diet", detail::AuthMode::Partner, body);
}

// ---------------- LaboratoryResource ----------------

nlohmann::json LaboratoryResource::catalog() {
    return t_->send("GET", "/outher/laboratoryCatalog", detail::AuthMode::Partner);
}

nlohmann::json LaboratoryResource::catalog_detail(const std::string& test_id) {
    return t_->send("GET", "/outher/laboratoryCatalog/" + test_id, detail::AuthMode::Partner);
}

nlohmann::json LaboratoryResource::results(const Patient& patient,
                                                  const std::optional<std::string>& page) {
    nlohmann::json body = patient_body(patient);
    put_opt(body, "currentPage", page);
    return t_->send("POST", "/outher/laboratoryResults", detail::AuthMode::Partner, body);
}

nlohmann::json LaboratoryResource::result_detail(const Patient& patient,
                                                        const std::string& test_id) {
    // Kept as a string so a "-lab" suffix (TmcLab order group) survives the round trip.
    nlohmann::json body = patient_body(patient);
    body["testId"] = test_id;
    return t_->send("POST", "/outher/laboratoryResult", detail::AuthMode::Partner, body);
}

// ---------------- MeasuresResource ----------------

nlohmann::json MeasuresResource::last(const Patient& patient) {
    return t_->send("POST", "/outher/lastMeasures", detail::AuthMode::Partner, patient_body(patient));
}

nlohmann::json MeasuresResource::list(const Patient& patient, const std::string& measure_type,
                                             const std::optional<std::string>& page,
                                             std::optional<int> glucose_type) {
    nlohmann::json body = patient_body(patient);
    put_opt(body, "currentPage", page);
    if (glucose_type) body["glucoseType"] = *glucose_type;
    return t_->send("POST", "/outher/measuresList/" + measure_type, detail::AuthMode::Partner, body);
}

nlohmann::json MeasuresResource::graph(const Patient& patient, const std::string& measure_type,
                                              int period, const std::optional<std::string>& page,
                                              std::optional<int> glucose_type) {
    nlohmann::json body = patient_body(patient);
    put_opt(body, "currentPage", page);
    if (glucose_type) body["glucoseType"] = *glucose_type;
    std::string path = "/outher/measuresGraph/" + measure_type + "/" + std::to_string(period);
    return t_->send("POST", path, detail::AuthMode::Partner, body);
}

nlohmann::json MeasuresResource::add_list(const Patient& patient,
                                                 const std::vector<nlohmann::json>& data) {
    nlohmann::json body = patient_body(patient);
    body["data"] = data;
    return t_->send("POST", "/outher/measures", detail::AuthMode::Partner, body);
}

nlohmann::json MeasuresResource::add(const Patient& patient, const std::string& measure_type,
                                            const nlohmann::json& fields) {
    return t_->send("POST", "/outher/measure/" + measure_type, detail::AuthMode::Partner,
                    measure_body(patient, &fields, std::nullopt));
}

nlohmann::json MeasuresResource::update(const Patient& patient, const std::string& measure_type,
                                               const std::string& id, const nlohmann::json& fields) {
    return t_->send("PUT", "/outher/measure/" + measure_type, detail::AuthMode::Partner,
                    measure_body(patient, &fields, id));
}

nlohmann::json MeasuresResource::delete_measure(const Patient& patient,
                                                       const std::string& measure_type,
                                                       const std::string& id) {
    return t_->send("DELETE", "/outher/measure/" + measure_type, detail::AuthMode::Partner,
                    measure_body(patient, nullptr, id));
}

nlohmann::json MeasuresResource::health_information(const std::optional<std::string>& identity,
                                                    const std::optional<std::string>& phone_number,
                                                    const std::vector<nlohmann::json>& data) {
    // Legacy `teusan` contract: flat, no `patient` wrapper. Kept verbatim.
    nlohmann::json body;
    body["identity"] = identity ? nlohmann::json(*identity) : nlohmann::json(nullptr);
    body["phoneNumber"] = phone_number ? nlohmann::json(*phone_number) : nlohmann::json(nullptr);
    body["data"] = data;
    return t_->send("POST", "/outher/healthInformation", detail::AuthMode::Partner, body);
}

}  // namespace bulutklinik
