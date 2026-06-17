# sdk-cpp — Bulutklinik API SDK for C++

Official Bulutklinik API SDK for C++ (C++17). Built on
[cpr](https://github.com/libcpr/cpr) (libcurl) + [nlohmann/json](https://github.com/nlohmann/json).

Covers the patient flow: **auth, doctor search, slots, appointments, payments,
and health measures**. See [`DESIGN.md`](./DESIGN.md) for the full wire contract.

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
#include <iostream>

int main() {
    bulutklinik::ClientOptions options;
    options.environment = bulutklinik::Environment::Production; // Production | Test | Local
    options.client_id = "clientId";
    options.client_secret = "clientSecret";
    bulutklinik::Client client(options);

    // 1) Log in (tokens are stored automatically)
    auto login = client.auth().connect("patient@example.com", std::string("•••••••"), "email");
    if (login.two_factor_required) {
        client.auth().connect_with_two_factor("123456", *login.two_factor_response);
    }

    // 2) Search — returns an nlohmann::json (the "data" payload)
    bulutklinik::SearchInput input;
    input.search_params = {{"withFreeText", "kardiyoloji"}};
    input.order_params = {"slot"};
    input.other_params = {"isInterviewable"};
    auto result = client.doctors().search(input);

    // 3) Slots, then 4) reserve ("YYYY-MM-DD HH:mm")
    std::string doctor_id = std::to_string(result["foundDoctors"][0]["doctor_id"].get<int>());
    auto slots = client.slots().schedule(doctor_id, "interview");
    client.appointments().reserve_interview(doctor_id, "2026-06-20 14:30");
}
```

## Services

| Accessor                  | Methods |
|---------------------------|---------|
| `client.auth()`           | `connect`, `connect_with_two_factor`, `register_patient`, `refresh`, `disconnect` |
| `client.doctors()`        | `branches`, `locations`, `quick_search`, `search`, `detail` |
| `client.slots()`          | `schedule` |
| `client.appointments()`   | `reserve_interview`, `add_physical`, `cancel` |
| `client.payments()`       | `check_discount_code`, `get_cards`, `save_card`, `pay`, `delete_card` |
| `client.measures()`       | `add_list`, `add`, `update`, `delete_measure`, `last`, `list`, `graph`, `partner_health_information` |

Data methods return `nlohmann::json`. (`register_patient` / `delete_measure` are
named to avoid the C++ keywords `register` / `delete`.)

## Authentication & tokens

- `connect` / `connect_with_two_factor` / `register_patient` store tokens automatically.
- On a `401` (or `resultType 4`), the SDK silently refreshes once and retries
  (thread-safe, single shared refresh).
- Inject a custom store via `ClientOptions::token_store` (subclass `TokenStore`).

## Errors

All derive from `bulutklinik::BulutklinikError`: `TransportError` and `ApiError`
→ `ValidationError` (422), `AuthenticationError` (401 / logout),
`AuthorizationError` (403), `NotFoundError` (404), `RateLimitError` (429).
`ApiError` carries `http_status`, `result_type`, `error_type`, `data`, `method`,
`path`, `retry_after`.

```cpp
try {
    client.payments().pay(input);
} catch (const bulutklinik::RateLimitError& e) {
    if (e.retry_after) std::cerr << "retry after " << *e.retry_after << "\n";
} catch (const bulutklinik::ApiError& e) {
    std::cerr << e.what() << "\n";
}
```

## Payments (3-D Secure)

`payments().pay` returns data containing `payment3DUrl` on a 3DS flow — a browser
URL to open. The bank → server callback completes the capture.

## License

MIT
