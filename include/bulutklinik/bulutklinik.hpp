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

    TokenStore& token_store();

private:
    std::shared_ptr<detail::Transport> transport_;
};

}  // namespace bulutklinik

#endif  // BULUTKLINIK_BULUTKLINIK_HPP
