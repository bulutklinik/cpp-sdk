# sdk-cpp — Bulutklinik partner API SDK for C++

Official Bulutklinik **partner** API SDK for C++ (C++17). Built on
[cpr](https://github.com/libcpr/cpr) (libcurl) + [nlohmann/json](https://github.com/nlohmann/json).

This is a single-persona SDK: every call runs on the company-scoped `/outher`
surface with the partner token issued for your integration. You act on the
patients of **your own company**, and the patient is named inline on each
request — there is no patient session. See [`DESIGN.md`](./DESIGN.md) for the
full wire contract.

> **1.1.0 restores `client.auth()`.** 1.0.x wrongly assumed the partner token
> could only be issued out of band; it is in fact minted by `connectApi` from
> your portal credentials, and it is refreshable. Existing 1.0.x code that sets
> `partner_token` keeps working. See [CHANGELOG.md](./CHANGELOG.md).

## Install (CMake + vcpkg)

Dependencies are declared in [`vcpkg.json`](./vcpkg.json); a Conan recipe is in
[`conanfile.py`](./conanfile.py) (`conan create .`).

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
ctest --test-dir build --output-on-failure
```

Consume from your own CMake project:

```cmake
find_package(bulutklinik CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE bulutklinik::sdk)
```

## Quick start

```cpp
#include <bulutklinik/bulutklinik.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

std::string env(const char* key) {
    const char* value = std::getenv(key);
    return value ? value : "";
}

int main() {
    bulutklinik::ClientOptions options;
    options.environment = bulutklinik::Environment::Production;  // Production | Test | Local
    options.api_version = bulutklinik::ApiVersion::V3;           // V3 (default) | V4
    options.client_id = env("BK_CLIENT_ID");
    options.client_secret = env("BK_CLIENT_SECRET");
    bulutklinik::Client client(options);

    // 0) Log in. Tokens are stored and refreshed for you.
    bulutklinik::ConnectInput login;
    login.api_user_name = env("BK_SERVICE_IDENTITY");
    login.api_user_password = env("BK_PASSWORD");
    client.auth().connect(login);

    // 1) Find a doctor you can book — returns an nlohmann::json ("data" payload)
    auto result = client.doctors().search(
        nlohmann::json{{"withFreeText", "kardiyoloji"}}, 1, {"slot"});
    std::string doctor_id = std::to_string(result["foundDoctors"][0]["doctor_id"].get<int>());

    // 2) Free slots
    auto schedule = client.slots().schedule(doctor_id, std::string("2026-08-01"));

    // 3) Hold one for a patient — named inline, no session
    bulutklinik::Patient user;
    user.name = "Ada";
    user.surname = "Lovelace";
    user.phone_number = "+905551112233";
    auto held = client.appointments().reserve_without_agreement(slot_id, doctor_id, user);

    // 4) Confirm before held["reservationExpired"] passes
    client.appointments().create(held["hash"].get<std::string>(),
                                 held["outherProcessId"].get<std::string>());
}
```

## Services

31 endpoints across seven groups.

| Group                    | Methods |
|--------------------------|---------|
| `client.auth()`          | `connect`, `refresh`, `disconnect` |
| `client.doctors()`       | `search`, `branches`, `detail`, `locations` |
| `client.slots()`         | `schedule` |
| `client.appointments()`  | `reserve`, `reserve_without_agreement`, `instant_reserve`, `create`, `create_without_slot`, `cancel_without_slot`, `list`, `info`, `check_doctor` |
| `client.measures()`      | `last`, `list`, `graph`, `add_list`, `add`, `update`, `delete_measure`, `health_information` |
| `client.laboratory()`    | `catalog`, `catalog_detail`, `results`, `result_detail` |
| `client.diets()`         | `list`, `detail` |

`delete_measure` carries that name because `delete` is a reserved keyword.

## Naming a patient

There is no session, so every patient-scoped call carries the patient in its
body — never in the URL, since a TCKN in a path segment would land in access
logs, proxy logs and error breadcrumbs.

**Reads** need only the reference fields. The server looks solely inside your own
company and never creates anything:

```cpp
bulutklinik::Patient reference;
reference.identity_number = "12345678901";
client.measures().last(reference);
client.diets().list(reference);
```

`identity_number` is primary; `phone_number` is a fallback accepted only when it
matches exactly one patient (the column is not unique — family members share
numbers). A patient you have never treated resolves to "not found", with the same
message as "not yours" so the endpoint cannot be used to probe for TCKNs.

**Writes** need `name`, `surname` and `phone_number` too, because the patient is
created inside your company if absent.

## Booking

Two flows, depending on who collects the agreements and the payment:

```cpp
// (A) Hand off to the patient — returns a browser url for agreements + payment.
auto held = client.appointments().reserve(slot_id, doctor_id, user);

// (B) You already collected them — returns a hash to confirm yourself.
auto held = client.appointments().reserve_without_agreement(slot_id, doctor_id, user);
client.appointments().create(hash, outher_process_id);
```

**Payment is never taken through the API.** No partner endpoint produces a
financial record; the browser hand-off in (A) is where payment happens. The SDK
returns the url verbatim and never opens or follows it.

`create_without_slot` books a free-form range outside the slot grid, for
integrations running their own calendar; `cancel_without_slot` reverses it — and
only it.

## Authentication

Your portal application issues a **client ID**, a **client secret** and a
project-specific **service identity**; the password is the one you set when
registering on the portal. `auth().connect()` exchanges them for an access token
and a refresh token:

```cpp
bulutklinik::ClientOptions options;
options.client_id = client_id;
options.client_secret = client_secret;
bulutklinik::Client client(options);

bulutklinik::ConnectInput input;
input.api_user_name = "svc@your-app.bulutklinik";
input.api_user_password = "your-portal-password";
// input.login_mode defaults to "email".

bulutklinik::LoginResult result = client.auth().connect(input);
```

The granted scope comes from the credentials, not the request — a partner
application is provisioned with `apiouther`, which is what makes `/outher`
reachable. Already holding a token? Set `partner_token` and skip the login.

If the account has SMS 2FA enabled the API answers with a challenge instead of a
token pair; `result.two_factor_required` is then true and no token was stored.

### Refresh

Access tokens last ~30 days, refresh tokens ~130. You do not normally call
`refresh()` yourself: on a `401` / `resultType 4` the SDK refreshes once and
retries the original request, and concurrent calls share one in-flight refresh.

```cpp
client.auth().refresh();     // only useful to refresh ahead of time
client.auth().disconnect();  // revokes both tokens and clears the store
```

If the refresh fails — or there is no refresh token because you supplied a bare
`partner_token` — the call throws `AuthenticationError` and you should
`connect()` again.

### Token storage

Tokens are read from a `TokenStore` on **every** request, so a long-running
process can rotate them without being rebuilt. Derive from `RefreshTokenStore` to
persist both:

```cpp
class VaultTokenStore : public bulutklinik::RefreshTokenStore {
public:
    std::optional<std::string> token() const override { /* … */ }
    void set_token(const std::optional<std::string>& t) override { /* … */ }
    std::optional<std::string> refresh_token() const override { /* … */ }
    void set_refresh_token(const std::optional<std::string>& t) override { /* … */ }
    void clear() override { /* … */ }
};

options.token_store = std::make_shared<VaultTokenStore>();
```

The two refresh members are **optional**. A plain `TokenStore` — the 1.0.x shape,
access token only — still works; the SDK then keeps the refresh token in memory,
so a process restart needs `connect()` rather than a refresh.

Set `partner_token` **or** `token_store`, not both — the constructor throws
`std::invalid_argument` rather than guessing which one you meant.

An `AuthorizationError` (403) means the credential itself is wrong — either the
granted scope does not include `apiouther`, or the account has no company. The
company boundary comes from the token, never from request input, so retrying with
different body parameters will not help.

## Health measures

```cpp
bulutklinik::Patient reference;
reference.identity_number = "12345678901";

// Write several measurements at once (max 200 per call, one transaction)
std::vector<nlohmann::json> rows = {
    {{"type", "tension"}, {"date_time", "2026-06-17 09:30"}, {"hypertension", 120}, {"hypotension", 80}},
};
client.measures().add_list(patient, rows);

client.measures().last(reference);
client.measures().list(reference, "glucose", std::string("1"), 0); // 0=fasting, 1=postprandial
client.measures().graph(reference, "tension", 2);                  // period 2 = weekly
```

> Measurements are written to **your own company**. A value you write does not
> appear in the patient's Bulutklinik mobile app, and values they entered there
> are not visible to you. That is tenant isolation working as intended.

`measures().health_information` is the legacy `teusan` bulk endpoint, marked
`[[deprecated]]` and kept for existing integrations: it needs the `teusan` scope
instead of `apiouther`, takes a flat identity + phone number instead of a patient
object, and writes into the shared consumer tenant. Its patient matching is an **OR**, and it is loose: the lookup is
`identity OR phoneNumber` against the *global* user table and takes the first
row, so a phone number alone can resolve someone whose TCKN differs from the one
you sent. Send both, but do not assume they are checked as a pair — the
`apiouther` reads above do the opposite, scoping to your company and failing
closed on ambiguity. Prefer `add_list` for anything new.

## Escape hatch

Not every endpoint has a typed method. `client.request` reuses the same
transport, so headers, envelope unwrapping and typed exceptions all still apply:

```cpp
auto data = client.request("GET", "/outher/somethingNew");

// Auth::Public reaches unauthenticated endpoints outside the partner surface,
// e.g. the city/district catalogue that feeds address forms.
bulutklinik::RequestOptions options;
options.auth = bulutklinik::Auth::Public;
auto config = client.request("GET", "/general/getConfig", options);
```

## Errors

All exceptions derive from `bulutklinik::BulutklinikError`:

`TransportError` (network) · `ApiError` → `ValidationError` (422),
`AuthenticationError` (401 / revoked / expired), `AuthorizationError` (403),
`NotFoundError` (404), `RateLimitError` (429).
Every `ApiError` carries `http_status`, `result_type`, `error_type`, `data`,
`method`, `path` and `retry_after`.

```cpp
try {
    client.measures().last(reference);
} catch (const bulutklinik::RateLimitError& e) {
    std::cerr << "retry after " << e.retry_after.value_or(0) << "\n";
} catch (const bulutklinik::ValidationError& e) {
    std::cerr << e.data.dump() << "\n";
}
```

Note that `/outher` reports most business-rule failures as HTTP **`501`** with
`resultType 1` — "patient not found in your company", "slot no longer free",
"doctor not bookable through your integration". It is not a server crash; read
the message.

## Types

Every method returns `nlohmann::json` — the unwrapped `data` payload. Numeric
ids are passed as `std::string` so they survive round-tripping (a lab result id
may carry a `-lab` suffix). Optional parameters use `std::optional`.

## License

MIT
