// Bulutklinik API SDK for C++ (C++17). Public API.
#ifndef BULUTKLINIK_BULUTKLINIK_HPP
#define BULUTKLINIK_BULUTKLINIK_HPP

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bulutklinik {

/// Base URL presets.
enum class Environment { Production, Test, Local };

/// Authorization mode for a request. `Bearer` uses the stored access token,
/// `Partner` the configured partner token, `Public` sends no `Authorization`.
enum class Auth { Public, Bearer, Partner };

// ---------------- errors ----------------

/// Base class for every exception thrown by the SDK.
class BulutklinikError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Network failure, timeout, DNS or TLS error — no usable HTTP response.
class TransportError : public BulutklinikError {
public:
    using BulutklinikError::BulutklinikError;
};

/// An HTTP response was received but the call was not successful.
class ApiError : public BulutklinikError {
public:
    ApiError(const std::string& message, int http_status, std::optional<int> result_type,
             nlohmann::json error_type, nlohmann::json data, std::string method,
             std::string path, std::optional<int> retry_after)
        : BulutklinikError(message),
          http_status(http_status),
          result_type(result_type),
          error_type(std::move(error_type)),
          data(std::move(data)),
          method(std::move(method)),
          path(std::move(path)),
          retry_after(retry_after) {}

    int http_status;
    std::optional<int> result_type;
    /// String label or numeric code (JSON value).
    nlohmann::json error_type;
    nlohmann::json data;
    std::string method;
    std::string path;
    std::optional<int> retry_after;
};

class ValidationError : public ApiError {
public:
    using ApiError::ApiError;
};
class AuthenticationError : public ApiError {
public:
    using ApiError::ApiError;
};
class AuthorizationError : public ApiError {
public:
    using ApiError::ApiError;
};
class NotFoundError : public ApiError {
public:
    using ApiError::ApiError;
};
class RateLimitError : public ApiError {
public:
    using ApiError::ApiError;
};

// ---------------- token store ----------------

/// Pluggable token persistence. The default is in-memory.
class TokenStore {
public:
    virtual ~TokenStore() = default;
    virtual std::optional<std::string> access_token() const = 0;
    virtual std::optional<std::string> refresh_token() const = 0;
    virtual void set_tokens(const std::string& access, const std::optional<std::string>& refresh) = 0;
    virtual void clear() = 0;
};

/// Default, thread-safe in-memory token store.
class InMemoryTokenStore : public TokenStore {
public:
    InMemoryTokenStore() = default;
    InMemoryTokenStore(std::optional<std::string> access, std::optional<std::string> refresh)
        : access_(std::move(access)), refresh_(std::move(refresh)) {}

    std::optional<std::string> access_token() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return access_;
    }
    std::optional<std::string> refresh_token() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return refresh_;
    }
    void set_tokens(const std::string& access, const std::optional<std::string>& refresh) override {
        std::lock_guard<std::mutex> lock(mutex_);
        access_ = access;
        refresh_ = refresh;
    }
    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        access_.reset();
        refresh_.reset();
    }

private:
    mutable std::mutex mutex_;
    std::optional<std::string> access_;
    std::optional<std::string> refresh_;
};

// ---------------- HTTP backend ----------------

struct HttpRequest {
    std::string method;
    std::string url;
    std::map<std::string, std::string> headers;
    std::optional<std::string> body;
    long timeout_ms = 30000;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    /// Response headers with lower-cased keys.
    std::map<std::string, std::string> headers;
    bool transport_error = false;
    std::string error_message;
};

/// HTTP transport abstraction. The default implementation uses cpr; inject a
/// custom one (e.g. for testing) via ClientOptions::http_backend.
class HttpBackend {
public:
    virtual ~HttpBackend() = default;
    virtual HttpResponse send(const HttpRequest& request) = 0;
};

/// Default backend, built on cpr (libcurl).
class CprHttpBackend : public HttpBackend {
public:
    HttpResponse send(const HttpRequest& request) override;
};

// ---------------- models ----------------

struct LoginResult {
    bool two_factor_required = false;
    std::optional<std::string> two_factor_response;
};

struct CardInfo {
    std::string card_holder;
    std::string card_number;
    std::string card_exp_month;
    std::string card_exp_year;
    std::string card_cvv;
};

struct RegisterInput {
    std::string name;
    std::string surname;
    std::string api_user_name;
    std::string phone_number;
    std::string password;
    std::string sms_verification_code;
    std::string response;
    int accept_user_agreement = 1;
    std::optional<std::string> client_id;
    std::optional<std::string> client_secret;
};

/// Input for the registration verify step (AuthResource::verify_registration).
/// The endpoint requires a CAPTCHA token (recaptcha_v2 or captcha) minted by a
/// browser/human, and is authorized with the configured partner token.
struct VerifyRegistrationInput {
    std::string name;
    std::string surname;
    std::string phone_number;
    /// Country dial code only, e.g. "+90" (matches ^\+\d{1,3}$).
    std::string phone_code;
    std::string email;
    std::string password;
    int accept_user_agreement = 1;
    /// Sent as "g-recaptcha-response-v2". Provide this or captcha.
    std::optional<std::string> recaptcha_v2;
    /// Sent as "captcha". Provide this or recaptcha_v2.
    std::optional<std::string> captcha;
    /// Optional structured agreement approvals, passed through verbatim.
    std::optional<nlohmann::json> user_agreements;
};

struct SearchInput {
    nlohmann::json search_params = nlohmann::json::object();
    std::vector<std::string> order_params;
    std::vector<std::string> other_params;
    int current_page = 1;
    int per_page_limit = 20;
};

struct PaymentInput {
    std::string doctor_id;
    std::string appointment_date;
    bool is_3d = false;
    bool terms_accept = false;
    std::string appointment_type = "interview";
    std::optional<CardInfo> card_info;
    std::optional<std::string> card_id;
    int save_card = 0;
    std::string discount_code;
    std::optional<std::string> case_detail;
};

/// Input for `meals().analyze`. `image` is base64 (a `data:…;base64,` prefix is
/// accepted). `portion_size` is one of `small | medium | large | custom` and
/// `meal_type` one of `breakfast | lunch | dinner | snack`. `portion_grams` is
/// required when `portion_size == "custom"`. `note` is optional free text.
struct MealInput {
    std::string image;
    std::string portion_size;
    std::string meal_type;
    std::optional<int> portion_grams;
    std::optional<std::string> note;
};

/// Input for `laboratory().order`. All three ids are required and map to the
/// API body `{ testId, addressId, laboratoryId }`.
struct LabOrderInput {
    std::string test_id;
    std::string address_id;
    std::string laboratory_id;
};

struct ClientOptions {
    Environment environment = Environment::Production;
    std::optional<std::string> base_url;
    std::string lang = "tr";
    std::optional<std::string> client_id;
    std::optional<std::string> client_secret;
    std::optional<std::string> partner_token;
    std::shared_ptr<TokenStore> token_store;
    std::shared_ptr<HttpBackend> http_backend;
    long timeout_ms = 30000;
};

/// Options for the generic `Client::request` escape hatch. `body` is a plain
/// `nlohmann::json` (a null value means "no body"); a per-request `lang`
/// overrides the client default when set.
struct RequestOptions {
    Auth auth = Auth::Bearer;
    nlohmann::json body = nlohmann::json(nullptr);
    std::optional<std::string> lang;
};

namespace detail {
class Transport;
}

// ---------------- resources ----------------

class AuthResource {
public:
    explicit AuthResource(detail::Transport* transport) : t_(transport) {}

    LoginResult connect(const std::string& api_user_name,
                        const std::optional<std::string>& api_user_password,
                        const std::string& login_mode,
                        const std::optional<std::string>& client_id = std::nullopt,
                        const std::optional<std::string>& client_secret = std::nullopt,
                        const std::optional<std::string>& with_phone_number = std::nullopt);
    void connect_with_two_factor(const std::string& sms_verification_code, const std::string& response);
    /// Registration step 1: send the verification code and return the raw data
    /// holding the encrypted `response` blob. Uses the configured partner token
    /// (the endpoint is behind `auth:apiusers`, not public); a CAPTCHA token
    /// (recaptcha_v2 or captcha), minted by a browser/human, is required. Feed the
    /// returned `response` (and the code the user receives) into register_patient.
    nlohmann::json verify_registration(const VerifyRegistrationInput& input);
    /// Named register_patient because `register` is a reserved keyword in C++.
    void register_patient(const RegisterInput& input);
    void refresh();
    void disconnect();

private:
    detail::Transport* t_;
};

class DoctorsResource {
public:
    explicit DoctorsResource(detail::Transport* transport) : t_(transport) {}

    nlohmann::json branches();
    nlohmann::json locations();
    nlohmann::json quick_search(const std::string& search_text,
                               const std::optional<std::string>& list_type = std::nullopt,
                               const std::optional<std::string>& location = std::nullopt);
    nlohmann::json search(const SearchInput& input);
    nlohmann::json detail(const std::string& id, const std::optional<std::string>& corporate = std::nullopt);

private:
    detail::Transport* t_;
};

class SlotsResource {
public:
    explicit SlotsResource(detail::Transport* transport) : t_(transport) {}

    nlohmann::json schedule(const std::string& doctor_id, const std::string& list_type,
                           const std::optional<std::string>& schedule_date = std::nullopt,
                           int schedule_step = 7, int schedule_page = 1);

private:
    detail::Transport* t_;
};

class AppointmentsResource {
public:
    explicit AppointmentsResource(detail::Transport* transport) : t_(transport) {}

    nlohmann::json reserve_interview(const std::string& doctor_id, const std::string& appointment_date,
                                    const std::string& appointment_type = "interview");
    nlohmann::json add_physical(const std::string& doctor_id, const std::string& appointment_date);
    nlohmann::json cancel(const std::string& event_id);

private:
    detail::Transport* t_;
};

class PaymentsResource {
public:
    explicit PaymentsResource(detail::Transport* transport) : t_(transport) {}

    nlohmann::json check_discount_code(const std::string& check_type, const std::string& discount_code,
                                      const std::optional<std::string>& doctor_id = std::nullopt,
                                      const std::optional<std::string>& order_id = std::nullopt,
                                      const std::optional<std::string>& special_service_id = std::nullopt,
                                      const std::optional<std::string>& program_slug = std::nullopt);
    nlohmann::json get_cards();
    nlohmann::json save_card(const CardInfo& card);
    nlohmann::json pay(const PaymentInput& input);
    nlohmann::json delete_card(const std::string& card_id);

private:
    detail::Transport* t_;
};

class MeasuresResource {
public:
    explicit MeasuresResource(detail::Transport* transport) : t_(transport) {}

    nlohmann::json add_list(const std::vector<nlohmann::json>& records);
    nlohmann::json add(const std::string& measure_type, const nlohmann::json& fields);
    nlohmann::json update(const std::string& measure_type, const nlohmann::json& fields);
    /// Named delete_measure because `delete` is a reserved keyword in C++.
    nlohmann::json delete_measure(const std::string& measure_type, const std::string& id);
    nlohmann::json last();
    nlohmann::json list(const std::string& measure_type, const std::string& page,
                       std::optional<int> glucose_type = std::nullopt);
    nlohmann::json graph(const std::string& measure_type, int period, const std::string& page,
                        std::optional<int> glucose_type = std::nullopt);
    nlohmann::json partner_health_information(const std::optional<std::string>& identity,
                                             const std::optional<std::string>& phone_number,
                                             const std::vector<nlohmann::json>& data);

private:
    detail::Transport* t_;
};

/// "Cildimde Neyim Var" — AI skin-lesion analysis.
class SkinResource {
public:
    explicit SkinResource(detail::Transport* transport) : t_(transport) {}

    /// Analyze one or more skin photos. Each image is a loose record like
    /// `{"image": "<base64>", "branch_id": 42}` (`branch_id` optional). Returns
    /// the `data` payload verbatim (per-image lesion label, Turkish comment,
    /// confidence, possible ICD hints and an opaque `case_detail` blob).
    nlohmann::json analyze(const std::vector<nlohmann::json>& images);

private:
    detail::Transport* t_;
};

/// AI meal-photo calorie/nutrition estimation (sibling of `skin`).
class MealsResource {
public:
    explicit MealsResource(detail::Transport* transport) : t_(transport) {}

    /// Estimate calories and nutrition from a meal photo. Input names map to the
    /// API's snake_case body (`portion_size`, `portion_grams`, `meal_type`).
    nlohmann::json analyze(const MealInput& input);

private:
    detail::Transport* t_;
};

/// The patient's laboratory results, the orderable test catalog, and pre-ordering.
class LaboratoryResource {
public:
    explicit LaboratoryResource(detail::Transport* transport) : t_(transport) {}

    /// The patient's completed/in-progress lab results. `page` defaults to 1
    /// server-side when the segment is omitted.
    nlohmann::json results(std::optional<std::string> page = std::nullopt);
    /// One result's detail. `test_id` is a string (`"123"` or `"123-lab"`),
    /// interpolated verbatim.
    nlohmann::json result_detail(const std::string& test_id);
    /// The orderable test-group catalog.
    nlohmann::json catalog();
    /// One catalog group by id.
    nlohmann::json catalog_detail(const std::string& id);
    /// Pre-order a lab test. All three ids are required.
    nlohmann::json order(const LabOrderInput& input);

private:
    detail::Transport* t_;
};

/// The patient's diet lists (a dietitian's "Diyet Listesi"). JSON only.
class DietsResource {
public:
    explicit DietsResource(detail::Transport* transport) : t_(transport) {}

    /// The patient's diet lists. `page` defaults to 1 server-side when omitted.
    nlohmann::json list(std::optional<std::string> page = std::nullopt);
    /// One diet list's detail by `list_id`.
    nlohmann::json detail(const std::string& list_id);

private:
    detail::Transport* t_;
};

// ---------------- client ----------------

/// The Bulutklinik API client. Construct once and reuse; resources are obtained
/// via accessor methods (e.g. client.doctors().quick_search(...)).
class Client {
public:
    explicit Client(ClientOptions options = {});
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    AuthResource auth();
    DoctorsResource doctors();
    SlotsResource slots();
    AppointmentsResource appointments();
    PaymentsResource payments();
    MeasuresResource measures();
    SkinResource skin();
    MealsResource meals();
    LaboratoryResource laboratory();
    DietsResource diets();

    /// Escape hatch: call any Bulutklinik API endpoint that does not yet have a
    /// typed resource method. The request still goes through the shared transport,
    /// so default headers, the chosen `auth` mode (`Auth::Bearer` by default),
    /// silent token refresh + retry, envelope unwrapping and the typed error
    /// hierarchy all apply. Returns the unwrapped `data` payload. Prefer a typed
    /// resource method when one exists; reach for this only for the gaps.
    ///
    /// @example
    /// ```cpp
    /// auto branches = client.request("GET", "/patients/allBranches");
    /// auto created = client.request("POST", "/patients/someNewEndpoint",
    ///                               {bulutklinik::Auth::Bearer, {{"foo", "bar"}}});
    /// ```
    nlohmann::json request(const std::string& method, const std::string& path,
                           const RequestOptions& options = {});

    TokenStore& token_store();

private:
    std::shared_ptr<detail::Transport> transport_;
};

}  // namespace bulutklinik

#endif  // BULUTKLINIK_BULUTKLINIK_HPP
