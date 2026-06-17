#include <bulutklinik/bulutklinik.hpp>

#include <cctype>

namespace bulutklinik {
namespace {

std::string base_url_for(Environment env) {
    switch (env) {
        case Environment::Production:
            return "https://api.bulutklinik.com/api/v3";
        case Environment::Test:
            return "https://apitest.bulutklinik.com/api/v3";
        case Environment::Local:
            return "https://api-bulutklinik.test/api/v3";
    }
    return "https://api.bulutklinik.com/api/v3";
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

enum class AuthMode { Public, Bearer, Partner };

class Transport {
public:
    Transport(std::shared_ptr<HttpBackend> backend, std::string base_url, std::string lang,
              std::optional<std::string> client_id, std::optional<std::string> client_secret,
              std::optional<std::string> partner_token, std::shared_ptr<TokenStore> token_store, long timeout_ms)
        : backend_(std::move(backend)),
          base_url_(std::move(base_url)),
          lang_(std::move(lang)),
          client_id_(std::move(client_id)),
          client_secret_(std::move(client_secret)),
          partner_token_(std::move(partner_token)),
          token_store_(std::move(token_store)),
          timeout_ms_(timeout_ms) {}

    TokenStore& token_store() { return *token_store_; }
    const std::optional<std::string>& client_id() const { return client_id_; }
    const std::optional<std::string>& client_secret() const { return client_secret_; }

    nlohmann::json send(const std::string& method, const std::string& path, AuthMode auth,
                        const nlohmann::json& body = nlohmann::json()) {
        return send_impl(method, path, auth, body, false);
    }

    void refresh() {
        if (!try_refresh(std::nullopt)) {
            throw AuthenticationError("token refresh failed", 401, std::nullopt, nlohmann::json(nullptr),
                                      nlohmann::json(nullptr), "POST", "/general/refreshApi", std::nullopt);
        }
    }

private:
    struct Dispatch {
        int status;
        nlohmann::json envelope;
        std::optional<std::string> retry_after;
    };

    nlohmann::json send_impl(const std::string& method, const std::string& path, AuthMode auth,
                             const nlohmann::json& body, bool is_retry) {
        std::optional<std::string> stale_access;
        if (auth == AuthMode::Bearer) {
            stale_access = token_store_->access_token();
        }

        Dispatch d = dispatch(method, path, auth, body);
        std::optional<int> result_type = result_type_of(d.envelope);

        if (d.status >= 200 && d.status < 300 && result_type && *result_type == 0) {
            return d.envelope.contains("data") ? d.envelope["data"] : nlohmann::json(nullptr);
        }

        const bool expired = d.status == 401 || (result_type && *result_type == 4);
        if (auth == AuthMode::Bearer && expired && !is_retry && try_refresh(stale_access)) {
            return send_impl(method, path, auth, body, true);
        }
        if (result_type && *result_type == 2) {
            token_store_->clear();
        }
        throw_api_error(method, path, d.status, d.envelope, d.retry_after);
    }

    Dispatch dispatch(const std::string& method, const std::string& path, AuthMode auth,
                      const nlohmann::json& body) {
        HttpRequest req;
        req.method = method;
        req.url = base_url_ + path;
        req.timeout_ms = timeout_ms_;
        req.headers["Accept"] = "application/json";
        req.headers["lang"] = lang_;
        if (!body.is_null() && method != "GET") {
            req.body = body.dump();
            req.headers["Content-Type"] = "application/json";
        }
        if (auth == AuthMode::Bearer) {
            auto token = token_store_->access_token();
            if (token && !token->empty()) {
                req.headers["Authorization"] = "Bearer " + *token;
            }
        } else if (auth == AuthMode::Partner) {
            if (partner_token_ && !partner_token_->empty()) {
                req.headers["Authorization"] = "Bearer " + *partner_token_;
            }
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

    bool try_refresh(const std::optional<std::string>& stale_access) {
        std::lock_guard<std::mutex> lock(refresh_mutex_);
        if (stale_access && token_store_->access_token() != stale_access) {
            return true;
        }
        auto refresh_token = token_store_->refresh_token();
        if (!refresh_token || refresh_token->empty() || !client_id_ || client_id_->empty() ||
            !client_secret_ || client_secret_->empty()) {
            return false;
        }

        nlohmann::json body = {
            {"refreshToken", *refresh_token},
            {"clientId", *client_id_},
            {"clientSecretKey", *client_secret_},
        };
        Dispatch d = dispatch("POST", "/general/refreshApi", AuthMode::Public, body);
        std::optional<int> result_type = result_type_of(d.envelope);
        if (d.status < 200 || d.status >= 300 || !result_type || *result_type != 0 ||
            !d.envelope.contains("data") || !d.envelope["data"].is_object() ||
            !d.envelope["data"].contains("access_token") || !d.envelope["data"]["access_token"].is_string()) {
            token_store_->clear();
            return false;
        }
        const auto& data = d.envelope["data"];
        std::optional<std::string> new_refresh = *refresh_token;
        if (data.contains("refresh_token") && data["refresh_token"].is_string()) {
            new_refresh = data["refresh_token"].get<std::string>();
        }
        token_store_->set_tokens(data["access_token"].get<std::string>(), new_refresh);
        return true;
    }

    std::shared_ptr<HttpBackend> backend_;
    std::string base_url_;
    std::string lang_;
    std::optional<std::string> client_id_;
    std::optional<std::string> client_secret_;
    std::optional<std::string> partner_token_;
    std::shared_ptr<TokenStore> token_store_;
    long timeout_ms_;
    std::mutex refresh_mutex_;
};

}  // namespace detail

namespace {

void store_tokens(detail::Transport* t, const nlohmann::json& data) {
    if (!data.is_object() || !data.contains("access_token") || !data["access_token"].is_string()) {
        throw BulutklinikError("Login response did not contain an access token");
    }
    std::optional<std::string> refresh;
    if (data.contains("refresh_token") && data["refresh_token"].is_string()) {
        refresh = data["refresh_token"].get<std::string>();
    }
    t->token_store().set_tokens(data["access_token"].get<std::string>(), refresh);
}

LoginResult finish_login(detail::Transport* t, const nlohmann::json& data) {
    if (data.is_object() && data.contains("access_token") && data["access_token"].is_string()) {
        store_tokens(t, data);
        return LoginResult{false, std::nullopt};
    }
    if (data.is_object() && data.contains("response") && data["response"].is_string()) {
        return LoginResult{true, data["response"].get<std::string>()};
    }
    return LoginResult{false, std::nullopt};
}

nlohmann::json card_to_json(const CardInfo& c) {
    return {
        {"cardHolder", c.card_holder},
        {"cardNumber", c.card_number},
        {"cardExpMonth", c.card_exp_month},
        {"cardExpYear", c.card_exp_year},
        {"cardCvv", c.card_cvv},
    };
}

}  // namespace

// ---------------- Client ----------------

Client::Client(ClientOptions options) {
    std::string base = options.base_url ? *options.base_url : base_url_for(options.environment);
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    auto store = options.token_store ? options.token_store : std::make_shared<InMemoryTokenStore>();
    auto backend = options.http_backend ? options.http_backend : std::make_shared<CprHttpBackend>();
    transport_ = std::make_shared<detail::Transport>(backend, base, options.lang, options.client_id,
                                                     options.client_secret, options.partner_token, store,
                                                     options.timeout_ms);
}

Client::~Client() = default;

AuthResource Client::auth() { return AuthResource(transport_.get()); }
DoctorsResource Client::doctors() { return DoctorsResource(transport_.get()); }
SlotsResource Client::slots() { return SlotsResource(transport_.get()); }
AppointmentsResource Client::appointments() { return AppointmentsResource(transport_.get()); }
PaymentsResource Client::payments() { return PaymentsResource(transport_.get()); }
MeasuresResource Client::measures() { return MeasuresResource(transport_.get()); }
TokenStore& Client::token_store() { return transport_->token_store(); }

// ---------------- AuthResource ----------------

LoginResult AuthResource::connect(const std::string& api_user_name,
                                  const std::optional<std::string>& api_user_password,
                                  const std::string& login_mode,
                                  const std::optional<std::string>& client_id,
                                  const std::optional<std::string>& client_secret,
                                  const std::optional<std::string>& with_phone_number) {
    nlohmann::json body;
    body["apiUserName"] = api_user_name;
    body["apiUserPassword"] = api_user_password ? nlohmann::json(*api_user_password) : nlohmann::json(nullptr);
    body["apiClientId"] = client_id ? *client_id : t_->client_id().value_or("");
    body["apiSecretKey"] = client_secret ? *client_secret : t_->client_secret().value_or("");
    body["loginMode"] = login_mode;
    if (with_phone_number) {
        body["withPhoneNumber"] = *with_phone_number;
    }
    nlohmann::json data = t_->send("POST", "/general/connectApi", detail::AuthMode::Public, body);
    return finish_login(t_, data);
}

void AuthResource::connect_with_two_factor(const std::string& sms_verification_code, const std::string& response) {
    nlohmann::json body = {{"smsVerificationCode", sms_verification_code}, {"response", response}};
    nlohmann::json data = t_->send("POST", "/general/connectApiWithTwoFactor", detail::AuthMode::Public, body);
    store_tokens(t_, data);
}

void AuthResource::register_patient(const RegisterInput& in) {
    nlohmann::json body;
    body["name"] = in.name;
    body["surname"] = in.surname;
    body["apiUserName"] = in.api_user_name;
    body["phoneNumber"] = in.phone_number;
    body["password"] = in.password;
    body["smsVerificationCode"] = in.sms_verification_code;
    body["response"] = in.response;
    body["acceptUserAgreement"] = in.accept_user_agreement == 0 ? 1 : in.accept_user_agreement;
    body["apiClientId"] = in.client_id ? *in.client_id : t_->client_id().value_or("");
    body["apiSecretKey"] = in.client_secret ? *in.client_secret : t_->client_secret().value_or("");
    nlohmann::json data = t_->send("POST", "/patients/addNewPatient", detail::AuthMode::Public, body);
    store_tokens(t_, data);
}

void AuthResource::refresh() { t_->refresh(); }

void AuthResource::disconnect() {
    struct ClearGuard {
        detail::Transport* t;
        ~ClearGuard() { t->token_store().clear(); }
    } guard{t_};
    t_->send("POST", "/general/disconnectApi", detail::AuthMode::Bearer, nlohmann::json::object());
}

// ---------------- DoctorsResource ----------------

nlohmann::json DoctorsResource::branches() {
    return t_->send("GET", "/patients/allBranches", detail::AuthMode::Bearer);
}

nlohmann::json DoctorsResource::locations() {
    return t_->send("GET", "/patients/allLocations", detail::AuthMode::Bearer);
}

nlohmann::json DoctorsResource::quick_search(const std::string& search_text,
                                             const std::optional<std::string>& list_type,
                                             const std::optional<std::string>& location) {
    nlohmann::json body;
    body["searchText"] = search_text;
    body["listType"] = list_type ? nlohmann::json(*list_type) : nlohmann::json(nullptr);
    body["location"] = location ? nlohmann::json(*location) : nlohmann::json(nullptr);
    return t_->send("POST", "/patients/quickSearch", detail::AuthMode::Bearer, body);
}

nlohmann::json DoctorsResource::search(const SearchInput& in) {
    nlohmann::json body;
    body["searchParams"] = in.search_params;
    body["orderParams"] = in.order_params;
    body["otherParams"] = in.other_params;
    body["currentPage"] = in.current_page > 0 ? in.current_page : 1;
    body["perPageLimit"] = in.per_page_limit > 0 ? in.per_page_limit : 20;
    return t_->send("POST", "/patients/filteredSearch", detail::AuthMode::Bearer, body);
}

nlohmann::json DoctorsResource::detail(const std::string& id, const std::optional<std::string>& corporate) {
    std::string path = "/patients/doctorDetail/" + id + (corporate ? "/" + *corporate : "");
    return t_->send("GET", path, bulutklinik::detail::AuthMode::Bearer);
}

// ---------------- SlotsResource ----------------

nlohmann::json SlotsResource::schedule(const std::string& doctor_id, const std::string& list_type,
                                       const std::optional<std::string>& schedule_date, int schedule_step,
                                       int schedule_page) {
    nlohmann::json body;
    body["doctorId"] = doctor_id;
    body["scheduleDate"] = schedule_date ? nlohmann::json(*schedule_date) : nlohmann::json(nullptr);
    body["scheduleStep"] = schedule_step;
    body["schedulePage"] = schedule_page;
    body["listType"] = list_type;
    return t_->send("POST", "/patients/doctorScheduler", detail::AuthMode::Bearer, body);
}

// ---------------- AppointmentsResource ----------------

nlohmann::json AppointmentsResource::reserve_interview(const std::string& doctor_id,
                                                       const std::string& appointment_date,
                                                       const std::string& appointment_type) {
    nlohmann::json body = {
        {"doctorId", doctor_id},
        {"appointmentDate", appointment_date},
        {"appointmentType", appointment_type},
    };
    return t_->send("POST", "/patients/addInterviewDateReservation", detail::AuthMode::Bearer, body);
}

nlohmann::json AppointmentsResource::add_physical(const std::string& doctor_id, const std::string& appointment_date) {
    nlohmann::json body = {{"doctorId", doctor_id}, {"appointmentDate", appointment_date}};
    return t_->send("POST", "/patients/addNewAppointment", detail::AuthMode::Bearer, body);
}

nlohmann::json AppointmentsResource::cancel(const std::string& event_id) {
    return t_->send("DELETE", "/patients/deleteUserAppointment/" + event_id, detail::AuthMode::Bearer);
}

// ---------------- PaymentsResource ----------------

nlohmann::json PaymentsResource::check_discount_code(const std::string& check_type, const std::string& discount_code,
                                                     const std::optional<std::string>& doctor_id,
                                                     const std::optional<std::string>& order_id,
                                                     const std::optional<std::string>& special_service_id,
                                                     const std::optional<std::string>& program_slug) {
    nlohmann::json body;
    body["checkType"] = check_type;
    body["discountCode"] = discount_code;
    if (doctor_id) {
        body["doctorId"] = *doctor_id;
    }
    if (order_id) {
        body["orderId"] = *order_id;
    }
    if (special_service_id) {
        body["specialServiceId"] = *special_service_id;
    }
    if (program_slug) {
        body["programSlug"] = *program_slug;
    }
    return t_->send("POST", "/patients/checkDiscountCode", detail::AuthMode::Bearer, body);
}

nlohmann::json PaymentsResource::get_cards() {
    return t_->send("GET", "/payments/getCards", detail::AuthMode::Bearer);
}

nlohmann::json PaymentsResource::save_card(const CardInfo& card) {
    return t_->send("POST", "/payments/saveCard", detail::AuthMode::Bearer, card_to_json(card));
}

nlohmann::json PaymentsResource::pay(const PaymentInput& in) {
    nlohmann::json body;
    body["doctorId"] = in.doctor_id;
    body["appointmentDate"] = in.appointment_date;
    body["appointmentType"] = in.appointment_type;
    body["is3D"] = in.is_3d;
    body["termsAccept"] = in.terms_accept;
    body["saveCard"] = in.save_card;
    body["discountCode"] = in.discount_code;
    if (in.card_id) {
        body["cardId"] = *in.card_id;
    }
    if (in.card_info) {
        body["cardInfo"] = card_to_json(*in.card_info);
    }
    if (in.case_detail) {
        body["caseDetail"] = *in.case_detail;
    }
    return t_->send("POST", "/payments/interviewPayment", detail::AuthMode::Bearer, body);
}

nlohmann::json PaymentsResource::delete_card(const std::string& card_id) {
    return t_->send("DELETE", "/payments/deleteCard/" + card_id, detail::AuthMode::Bearer);
}

// ---------------- MeasuresResource ----------------

nlohmann::json MeasuresResource::add_list(const std::vector<nlohmann::json>& records) {
    nlohmann::json body;
    body["data"] = records;
    return t_->send("POST", "/patients/addNewUserMeasures", detail::AuthMode::Bearer, body);
}

nlohmann::json MeasuresResource::add(const std::string& measure_type, const nlohmann::json& fields) {
    return t_->send("POST", "/patients/addNewUserMeasures/" + measure_type, detail::AuthMode::Bearer, fields);
}

nlohmann::json MeasuresResource::update(const std::string& measure_type, const nlohmann::json& fields) {
    return t_->send("PUT", "/patients/updateUserMeasures/" + measure_type, detail::AuthMode::Bearer, fields);
}

nlohmann::json MeasuresResource::delete_measure(const std::string& measure_type, const std::string& id) {
    nlohmann::json body = {{"id", id}};
    return t_->send("DELETE", "/patients/deleteUserMeasures/" + measure_type, detail::AuthMode::Bearer, body);
}

nlohmann::json MeasuresResource::last() {
    return t_->send("GET", "/patients/measuresList", detail::AuthMode::Bearer);
}

nlohmann::json MeasuresResource::list(const std::string& measure_type, const std::string& page,
                                      std::optional<int> glucose_type) {
    std::string path = "/patients/userMeasuresList/" + measure_type + "/" + page;
    if (glucose_type) {
        path += "/" + std::to_string(*glucose_type);
    }
    return t_->send("GET", path, detail::AuthMode::Bearer);
}

nlohmann::json MeasuresResource::graph(const std::string& measure_type, int period, const std::string& page,
                                       std::optional<int> glucose_type) {
    std::string path = "/patients/userMeasuresGraph/" + measure_type + "/" + std::to_string(period) + "/" + page;
    if (glucose_type) {
        path += "/" + std::to_string(*glucose_type);
    }
    return t_->send("GET", path, detail::AuthMode::Bearer);
}

nlohmann::json MeasuresResource::partner_health_information(const std::optional<std::string>& identity,
                                                           const std::optional<std::string>& phone_number,
                                                           const std::vector<nlohmann::json>& data) {
    nlohmann::json body;
    body["identity"] = identity ? nlohmann::json(*identity) : nlohmann::json(nullptr);
    body["phoneNumber"] = phone_number ? nlohmann::json(*phone_number) : nlohmann::json(nullptr);
    body["data"] = data;
    return t_->send("POST", "/outher/healthInformation", detail::AuthMode::Partner, body);
}

}  // namespace bulutklinik
