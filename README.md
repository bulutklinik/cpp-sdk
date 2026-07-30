# sdk-cpp — Bulutklinik partner API SDK for C++

Official Bulutklinik **partner** API SDK for C++ (C++17). Built on
[cpr](https://github.com/libcpr/cpr) (libcurl) + [nlohmann/json](https://github.com/nlohmann/json).

This is a single-persona SDK: every call runs on the company-scoped `/outher`
surface with the partner token issued for your integration. You act on the
patients of **your own company**, and the patient is named inline on each
request — there is no login and no session. See [`DESIGN.md`](./DESIGN.md) for
the full wire contract.

> **1.0.0 is a breaking release.** The patient persona (login, registration,
> payments, AI analysis, address book) has been removed and the former
> `client.partner()` namespace was lifted to the client root. See
> [CHANGELOG.md](./CHANGELOG.md) and DESIGN.md §12 for the migration.

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

int main() {
    bulutklinik::ClientOptions options;
    options.environment = bulutklinik::Environment::Production;  // Production | Test | Local
    options.api_version = bulutklinik::ApiVersion::V3;           // V3 (default) | V4
    if (const char* token = std::getenv("BK_PARTNER_TOKEN")) {
        options.partner_token = token;
    }
    bulutklinik::Client client(options);

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

28 endpoints across six groups.

| Group                    | Methods |
|--------------------------|---------|
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

The partner token is **issued out of band** through the Bulutklinik Developer
Platform. It behaves like an API key: there is no login method, and the SDK
cannot renew it.

The token is read from a `TokenStore` on **every** request, so a long-running
process can pick up a newly issued one without being rebuilt:

```cpp
class VaultTokenStore : public bulutklinik::TokenStore {
public:
    std::optional<std::string> token() const override { /* … */ }
    void set_token(const std::optional<std::string>& t) override { /* … */ }
    void clear() override { /* … */ }
};

options.token_store = std::make_shared<VaultTokenStore>();

// …or rotate the default in-memory store in place:
client.token_store().set_token(newly_issued_token);
```

Set `partner_token` **or** `token_store`, not both — the constructor throws
`std::invalid_argument` rather than guessing which one you meant.

### When the token expires

Tokens last about 30 days. An expired one comes back as `401` / `resultType 4`;
the SDK throws `AuthenticationError` and does **not** retry — there is nothing to
refresh. Recovery is operational: obtain a newly issued token and write it into
the store.

> This is the one behaviour that changed meaning in 1.0.0. On the patient SDK
> `resultType 4` meant "the SDK will fix this silently". Here it means the opposite.

An `AuthorizationError` (403) means the credential itself is wrong — either the
token lacks the `apiouther` scope, or it resolves to a user with no company. The
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
object, and writes into the shared consumer tenant. The API currently matches on
phone number only (a server-side bug nulls `identity` during validation); pass
both for forward compatibility. Prefer `add_list` for anything new.

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
