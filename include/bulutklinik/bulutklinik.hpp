// Bulutklinik partner API SDK for C++ (C++17). Public API.
//
// Single-persona SDK: every call runs on the company-scoped `/outher` surface
// with the partner token issued for your integration. You act on the patients of
// your own company, and the patient is named inline on each request — there is
// no login and no session.
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

/// API version segment. The `/outher` surface is route-for-route identical on
/// both, so switching is configuration rather than a code change.
enum class ApiVersion { V3, V4 };

/// Authorization mode for a request. `Partner` sends the configured partner
/// token; `Public` sends no `Authorization` header. Every typed method is
/// `Partner` — `Public` is only reachable through `Client::request`.
enum class Auth { Public, Partner };

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
///
/// Note that `/outher` reports most business-rule failures as HTTP 501 with
/// `result_type` 1 — "patient not found in your company", "slot no longer free",
/// "doctor not bookable through your integration". It is not a server crash;
/// read the message.
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
/// 401, a revoked token (result_type 2), or an expired one (result_type 4).
class AuthenticationError : public ApiError {
public:
    using ApiError::ApiError;
};
/// 403 — the token authenticated but is not permitted. Either it lacks the
/// `apiouther` scope or it resolves to a user with no company. The company
/// boundary comes from the token, never from request input, so retrying with
/// different body parameters will not help.
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

/// Pluggable source for the partner token.
///
/// The token is read on every request, so pointing this at a file, cache,
/// database or secret manager lets a long-running process pick up a newly issued
/// token without being rebuilt. An empty optional means "no token"; the transport
/// then fails before dispatching rather than sending an anonymous request.
///
/// Implementations must be thread-safe.
class TokenStore {
public:
    virtual ~TokenStore() = default;
    virtual std::optional<std::string> token() const = 0;
    virtual void set_token(const std::optional<std::string>& token) = 0;
    virtual void clear() = 0;
};

/// Default, thread-safe in-memory token store.
class InMemoryTokenStore : public TokenStore {
public:
    InMemoryTokenStore() = default;
    explicit InMemoryTokenStore(std::optional<std::string> token) : token_(std::move(token)) {}

    std::optional<std::string> token() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return token_;
    }
    void set_token(const std::optional<std::string>& token) override {
        std::lock_guard<std::mutex> lock(mutex_);
        token_ = token;
    }
    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        token_.reset();
    }

private:
    mutable std::mutex mutex_;
    std::optional<std::string> token_;
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

// ---------------- configuration ----------------

/// Client configuration.
///
/// Set `partner_token` **or** `token_store`, not both — either the literal or the
/// store is the source of truth for the credential, and guessing which one the
/// caller meant is how credential bugs get shipped. Passing both throws
/// std::invalid_argument from the Client constructor.
struct ClientOptions {
    Environment environment = Environment::Production;
    /// Ignored when `base_url` is set.
    ApiVersion api_version = ApiVersion::V3;
    /// Explicit base URL; overrides `environment` + `api_version`.
    std::optional<std::string> base_url;
    std::string lang = "tr";
    /// The partner token issued for your integration. Seeds the default
    /// in-memory token store.
    std::optional<std::string> partner_token;
    std::shared_ptr<TokenStore> token_store;
    std::shared_ptr<HttpBackend> http_backend;
    long timeout_ms = 30000;
};

/// Options for the generic `Client::request` escape hatch. `body` is a plain
/// `nlohmann::json` (a null value means "no body"); a per-request `lang`
/// overrides the client default when set.
struct RequestOptions {
    Auth auth = Auth::Partner;
    nlohmann::json body = nlohmann::json(nullptr);
    std::optional<std::string> lang;
};

namespace detail {
class Transport;
}

// ---------------- patient references ----------------

/// Identifies a patient.
///
/// Reads need only `identity_number` (primary) or `phone_number` (accepted solely
/// when it matches exactly one patient in your company — the column is not unique,
/// and the server fails closed rather than guessing). The server looks only inside
/// your own company on this path and never creates anything, so a patient you have
/// never treated resolves to "not found" — with the same message as "not yours",
/// so the endpoint cannot be used to probe for TCKNs.
///
/// Writes need `name`, `surname` and `phone_number` as well: if no matching
/// patient exists in your company the server creates one.
struct Patient {
    std::optional<std::string> name;
    std::optional<std::string> surname;
    std::optional<std::string> phone_number;
    std::optional<std::string> identity_number;
    std::optional<std::string> email;
    /// `Y-m-d`.
    std::optional<std::string> birthdate;
    /// ISO country code present in `bas_com_countries.code`.
    std::optional<std::string> nationality;
    std::optional<double> price;

    /// Serialises only the fields that were set, matching the server contract.
    nlohmann::json to_json() const;
};

/// Addresses one appointment either by its process (`hash` + `outher_process_id`)
/// or by its coordinates (`doctor_id` + `appointment_date` + `is_outher_doctor`).
/// Supply one pair or the other.
struct AppointmentLookup {
    std::optional<std::string> hash;
    std::optional<std::string> outher_process_id;
    std::optional<std::string> doctor_id;
    /// `Y-m-d H:i`.
    std::optional<std::string> appointment_date;
    std::optional<int> is_outher_doctor;

    nlohmann::json to_json() const;
};

// ---------------- resources ----------------

/// Doctor discovery. Results are scoped to the doctors enabled for your
/// integration, so a doctor returned here is one you can book. `locations` is the
/// exception — a global city catalogue, not company-scoped.
class DoctorsResource {
public:
    explicit DoctorsResource(detail::Transport* transport) : t_(transport) {}

    /// `order_params` accepts "name", "order" and "slot".
    ///
    /// `search_params` must carry at least one key: the server rule is
    /// `required|array` and PHP's `required` rejects an empty array, so `{}` is a
    /// validation error rather than an unfiltered search.
    nlohmann::json search(const nlohmann::json& search_params, int current_page = 1,
                          const std::vector<std::string>& order_params = {});
    nlohmann::json branches();
    /// The `doctor_id` here feeds `SlotsResource::schedule`.
    nlohmann::json detail(const std::string& doctor_id);
    /// City list. Global catalogue — not scoped to your company.
    nlohmann::json locations();

private:
    detail::Transport* t_;
};

/// Doctor availability.
class SlotsResource {
public:
    explicit SlotsResource(detail::Transport* transport) : t_(transport) {}

    /// Either pass `schedule_date` (`Y-m-d`), or page with `schedule_step` +
    /// `schedule_page`; the server requires one of the two forms.
    ///
    /// Returns a date-keyed map; `slotId` feeds `AppointmentsResource::reserve`.
    nlohmann::json schedule(const std::string& doctor_id,
                            const std::optional<std::string>& schedule_date = std::nullopt,
                            std::optional<int> schedule_step = std::nullopt,
                            std::optional<int> schedule_page = std::nullopt);

private:
    detail::Transport* t_;
};

/// The appointment lifecycle. The patient is supplied inline as `user`; the
/// server materialises it inside your company on write.
///
/// Two booking flows:
///   - hand off to the patient: `reserve` returns a `url` the patient opens in a
///     browser to accept the agreements and pay;
///   - you collected the agreements: `reserve_without_agreement` returns a `hash`
///     to feed, with `outherProcessId`, into `create`.
///
/// Payment is never taken through the API. No partner endpoint produces a
/// financial record; that is what the browser hand-off is for.
class AppointmentsResource {
public:
    explicit AppointmentsResource(detail::Transport* transport) : t_(transport) {}

    /// Hold an online slot and get back a `url` for the patient to complete
    /// agreements and payment in a browser.
    nlohmann::json reserve(const std::string& slot_id, const std::string& doctor_id,
                           const Patient& user);
    /// Same hold, for integrations that collect the agreements themselves.
    /// Returns a `hash` plus `reservationExpired` — confirm before it passes.
    nlohmann::json reserve_without_agreement(const std::string& slot_id, const std::string& doctor_id,
                                             const Patient& user);
    /// Instant reservation — no slot; the server picks an available doctor.
    nlohmann::json instant_reserve(const Patient& user);
    /// Turn a reservation into a confirmed appointment.
    nlohmann::json create(const std::string& hash, const std::string& outher_process_id);
    /// Book a free-form time range outside the slot grid.
    nlohmann::json create_without_slot(const std::string& doctor_id, const std::string& start_date,
                                       const std::string& finish_date, const Patient& user,
                                       std::optional<int> is_outher_doctor = std::nullopt);
    /// Cancel an appointment made with `create_without_slot` — and only those;
    /// ones confirmed through `create` are not cancellable here.
    nlohmann::json cancel_without_slot(const AppointmentLookup& lookup);
    /// The appointments you created for that phone number, not the patient's
    /// history across the platform.
    nlohmann::json list(const std::string& phone_number,
                        const std::optional<std::string>& page = std::nullopt,
                        const std::optional<std::string>& type = std::nullopt);
    nlohmann::json info(const AppointmentLookup& lookup);
    /// Whether a doctor is bookable through your integration. Fails with 501 when
    /// they are not — call it before offering a doctor.
    nlohmann::json check_doctor(const std::string& doctor_id, int is_outher_doctor);

private:
    detail::Transport* t_;
};

/// Diet lists recorded for a patient inside your own company. Lists written by
/// other clinics are not visible here.
class DietsResource {
public:
    explicit DietsResource(detail::Transport* transport) : t_(transport) {}

    /// Page size is fixed to 20 server-side.
    nlohmann::json list(const Patient& patient,
                        const std::optional<std::string>& page = std::nullopt);
    /// `list_id` comes from `list`.
    nlohmann::json detail(const Patient& patient, const std::string& list_id);

private:
    detail::Transport* t_;
};

/// Laboratory catalogue (global, static) and results (your company only, merging
/// the clinic's HBYS lab requests and TmcLab order groups).
///
/// Ordering a test is not available here — it creates a financial record.
class LaboratoryResource {
public:
    explicit LaboratoryResource(detail::Transport* transport) : t_(transport) {}

    nlohmann::json catalog();
    /// Prices are the plain list prices — the patient-side discount pass does not
    /// apply here.
    nlohmann::json catalog_detail(const std::string& test_id);
    /// Each item's `id` is accepted verbatim by `result_detail`; a `-lab` suffix
    /// marks a TmcLab order group.
    nlohmann::json results(const Patient& patient,
                           const std::optional<std::string>& page = std::nullopt);
    nlohmann::json result_detail(const Patient& patient, const std::string& test_id);

private:
    detail::Transport* t_;
};

/// Health measurements.
///
/// Scope: written into and read from your own company. Values the patient entered
/// in the Bulutklinik mobile app are not visible here, and a value you write does
/// not appear in their app — a consequence of tenant isolation, not a bug.
class MeasuresResource {
public:
    explicit MeasuresResource(detail::Transport* transport) : t_(transport) {}

    nlohmann::json last(const Patient& patient);
    /// `glucose_type` applies to "glucose" only (0=fasting, 1=postprandial).
    nlohmann::json list(const Patient& patient, const std::string& measure_type,
                        const std::optional<std::string>& page = std::nullopt,
                        std::optional<int> glucose_type = std::nullopt);
    /// `period`: 1=day, 2=week, 3=month, 4=year.
    nlohmann::json graph(const Patient& patient, const std::string& measure_type, int period,
                         const std::optional<std::string>& page = std::nullopt,
                         std::optional<int> glucose_type = std::nullopt);
    /// Write several measurements of mixed types in one transaction. The server
    /// caps a single call at 200 rows.
    nlohmann::json add_list(const Patient& patient, const std::vector<nlohmann::json>& data);
    nlohmann::json add(const Patient& patient, const std::string& measure_type,
                       const nlohmann::json& fields);
    nlohmann::json update(const Patient& patient, const std::string& measure_type,
                          const std::string& id, const nlohmann::json& fields);
    /// Named delete_measure because `delete` is a reserved keyword in C++.
    nlohmann::json delete_measure(const Patient& patient, const std::string& measure_type,
                                  const std::string& id);
    /// Legacy bulk submission for `teusan` integrations.
    ///
    /// Deprecated: requires the `teusan` scope instead of `apiouther`, takes a flat
    /// identity + phone number instead of a patient object, and writes into the
    /// shared consumer tenant rather than your own company — so the values are not
    /// readable through `last` or `list`. Prefer `add_list`.
    [[deprecated("Requires the teusan scope and writes into the shared consumer tenant; prefer add_list.")]]
    nlohmann::json health_information(const std::optional<std::string>& identity,
                                      const std::optional<std::string>& phone_number,
                                      const std::vector<nlohmann::json>& data);

private:
    detail::Transport* t_;
};

// ---------------- client ----------------

/// The Bulutklinik partner API client. Construct once and reuse; resources are
/// obtained via accessor methods (e.g. client.doctors().branches()).
class Client {
public:
    /// @throws std::invalid_argument when both `partner_token` and `token_store`
    /// are set.
    explicit Client(ClientOptions options = {});
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    DoctorsResource doctors();
    SlotsResource slots();
    AppointmentsResource appointments();
    MeasuresResource measures();
    LaboratoryResource laboratory();
    DietsResource diets();

    /// Escape hatch: call any Bulutklinik API endpoint that does not yet have a
    /// typed resource method. The request still goes through the shared transport,
    /// so default headers, the chosen `auth` mode (`Auth::Partner` by default),
    /// envelope unwrapping and the typed error hierarchy all apply. Returns the
    /// unwrapped `data` payload. Prefer a typed resource method when one exists.
    ///
    /// @example
    /// ```cpp
    /// auto branches = client.request("GET", "/outher/branches");
    /// // Auth::Public reaches unauthenticated endpoints outside the partner surface
    /// auto config = client.request("GET", "/general/getConfig",
    ///                              {bulutklinik::Auth::Public});
    /// ```
    nlohmann::json request(const std::string& method, const std::string& path,
                           const RequestOptions& options = {});

    /// The active token store. Write a newly issued partner token here to rotate
    /// the credential without rebuilding the client.
    TokenStore& token_store();

private:
    std::shared_ptr<detail::Transport> transport_;
};

}  // namespace bulutklinik

#endif  // BULUTKLINIK_BULUTKLINIK_HPP
