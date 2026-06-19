# Bulutklinik SDK — Canonical Design (SSOT)

> **This file is the single source of truth (SSOT) for every official Bulutklinik
> SDK.** All language packages (JavaScript/TypeScript, PHP, Python, Go, Java, C#,
> C++) are hand-written but MUST implement exactly the contract described here.
> The canonical copy lives at `dev-kits/DESIGN.md`; an identical copy is vendored
> into each language repository and re-synced whenever this file changes.
>
> Wire contract is derived from `dev-kits/Bulutklinik.postman_collection.json`
> ("Bulutklinik API — Randevu & Ödeme Akışı"), validated against the BulutklinikAPI
> source (Laravel 8.12, OAuth2/Passport).

- **Spec version:** 0.2.0 (adds the §7.2 escape hatch; validated against the TypeScript reference SDK, live against the `test` environment)
- **API:** BulutklinikAPI v3
- **Scope:** 6 services / 27 endpoints (patient persona). Designed to grow.

---

## 1. Scope

The SDKs cover the patient appointment-and-payment flow plus health measurements:

| Service        | Endpoints | Purpose                                              |
|----------------|:---------:|------------------------------------------------------|
| `auth`         | 5         | Login, 2FA, token refresh, registration, logout      |
| `doctors`      | 5         | Branches, locations, quick/filtered search, detail   |
| `slots`        | 1         | Doctor availability (materialized slots)             |
| `appointments` | 3         | Online reservation, physical appointment, cancel     |
| `payments`     | 5         | Discount check, saved cards, pay (3DS)               |
| `measures`     | 8         | Health measurements (CRUD, list, graph, partner)     |

Out of scope for this collection (may be added later): "Anlık randevu" (programs),
video-call (calls). The SDK surface is designed so new services slot in as new
resource groups without breaking existing ones.

---

## 2. Environments & transport

### 2.1 Base URLs

| Env          | Base URL                                       |
|--------------|------------------------------------------------|
| `production` | `https://api.bulutklinik.com/api/v3`           |
| `test`       | `https://apitest.bulutklinik.com/api/v3`       |
| `local`      | `https://api-bulutklinik.test/api/v3` (Herd)   |

The client accepts either a named environment preset or an explicit base URL.
Default: `production`.

### 2.2 Required headers

| Header         | Value                          | Notes                                       |
|----------------|--------------------------------|---------------------------------------------|
| `Accept`       | `application/json`             | Always.                                     |
| `Content-Type` | `application/json`             | On requests with a body.                    |
| `lang`         | `tr` (default), `en`, `de`, `az` | Configurable per-client and per-request.  |
| `Authorization`| `Bearer <accessToken>`         | Protected endpoints only. Omitted on public endpoints; partner endpoint uses the partner token. |

### 2.3 HTTP methods

Endpoints use `GET`, `POST`, `PUT`, `DELETE` as specified per endpoint in §6.
Path parameters (e.g. `{id}`, `{type}`, `{page}`) are URL segments, not query
string. Request bodies are JSON.

---

## 3. Response envelope

Every API response is a JSON envelope:

```jsonc
{
  "resultType": 0,            // integer state code (see §3.1)
  "errorType": "validation",  // optional; string label OR numeric code (see note)
  "errorMessage": "…",        // optional, human-readable (localized via `lang`)
  "successMessage": "…",      // optional
  "data": { /* payload */ }   // endpoint-specific; may be null, object, array, or string
}
```

> **`errorType` is polymorphic** (verified live): some endpoints return a string
> label (e.g. `"validation"`), others return a numeric code (e.g. `1`). SDKs must
> accept both — only treat `errorType` as a refinement hint when it is a string;
> never assume it is a string (e.g. don't call string methods on it unguarded).

A call is **successful** when the HTTP status is 2xx **and** `resultType == 0`.
SDKs unwrap and return `data` to the caller on success; otherwise they raise a
typed error (§4).

### 3.1 `resultType` state machine

| Value | Name     | SDK behavior                                                                 |
|:-----:|----------|------------------------------------------------------------------------------|
| `0`   | Success  | Return `data`.                                                               |
| `1`   | Error    | Raise `ApiError` (or a more specific subtype based on HTTP status / `errorType`). |
| `2`   | Logout   | Clear the token store, raise `AuthenticationError` (session revoked).        |
| `3`   | Update   | Raise `ApiError` with an "update required" marker (client/app too old).      |
| `4`   | Refresh  | Token expired. **Not** returned by `refreshApi`; returned by the global handler on any protected call that receives an expired/invalid token (HTTP 401). Triggers the auto-refresh+retry flow (§5.4). |

> Implementation note: `resultType 4` is the canonical refresh signal, but a bare
> HTTP `401` (without a parseable envelope) MUST be treated identically.

---

## 4. Error model

All SDKs expose one error hierarchy with a common base. Names follow each
language's convention (e.g. `BulutklinikError` / `ApiError` in TS, exceptions in
PHP/Python, `error` values implementing an interface in Go, exception classes in
Java/C#/C++).

```
BulutklinikError                  (base — all SDK errors derive from this)
├── TransportError                (network failure, timeout, DNS, TLS — no HTTP response)
└── ApiError                      (got an HTTP response that wasn't a success)
    ├── ValidationError           (422, or errorType=validation)
    ├── AuthenticationError       (401 / resultType 2 logout / failed refresh)
    ├── AuthorizationError        (403 — authenticated but not permitted/scoped)
    ├── NotFoundError             (404)
    └── RateLimitError            (429 — throttled; carries Retry-After if present)
```

Each `ApiError` carries: `httpStatus`, `resultType`, `errorType`, `errorMessage`,
the raw `data`, and the originating request (method + path) for debugging.
Mapping precedence: logout (`resultType == 2`) → string `errorType == "validation"`
→ HTTP status (401→Auth, 403→Authz, 404→NotFound, 422→Validation, 429→RateLimit)
→ otherwise (incl. numeric `errorType`, or success HTTP with `resultType != 0`) → `ApiError`.
Because `errorType` may be numeric (§3), guard before string-matching it.

---

## 5. Authentication & token lifecycle

OAuth2 via Laravel Passport. Access token lifetime ~30 days, refresh token ~130
days. The token grant happens server-side inside `connectApi` (no direct
`oauth/token` HTTP call from the SDK).

### 5.1 Login — `auth.connect`

`POST /general/connectApi` (also aliased at root `/connectApi`). **Public** (no Bearer).

Request body:

| Field             | Required | Notes                                                            |
|-------------------|:--------:|------------------------------------------------------------------|
| `apiUserName`     | ✓        | Identifier per `loginMode` (email / TC / phone / user_id).       |
| `apiUserPassword` | ✓*       | Required except `social` / `afterRegister` modes.                |
| `apiClientId`     | ✓        | OAuth client id.                                                 |
| `apiSecretKey`    | ✓        | OAuth client secret.                                             |
| `loginMode`       | ✓        | `email` \| `identity` \| `phone` \| `user_id` \| `social` \| `afterRegister`. |
| `withPhoneNumber` | —        | Some installs require it in `phone` mode.                        |

`loginMode` `social` / `afterRegister` skip password validation
(`validateForPassportPasswordGrant`).

Success → `data: { access_token, refresh_token, password_policy }`. The SDK
persists both tokens via the token store (§5.5).

**2FA branch:** if SMS 2FA is enabled (`sms_2fa_status=1`), `data.access_token` is
absent and `data.response` carries an encrypted blob. The SDK surfaces this as a
*two-factor challenge* (typed result, not an error) so the caller can collect the
SMS code and call `auth.connectWithTwoFactor`.

### 5.2 2FA verification — `auth.connectWithTwoFactor`

`POST /general/connectApiWithTwoFactor`. **Public** (middleware verifies the SMS code
inside the encrypted blob).

Request body:

| Field                  | Required | Notes                                          |
|------------------------|:--------:|------------------------------------------------|
| `smsVerificationCode`  | ✓        | The code the user received by SMS.             |
| `response`             | ✓        | The encrypted blob from `connect`'s `data.response`. |

(The collection also sends `tokenInfo`, but the server ignores it — the real token
is decrypted from `response`. SDKs send only `smsVerificationCode` + `response`.)

Success → `data: { access_token, refresh_token }`. Token is **not** re-minted here;
it was minted during `connect` and is returned now.

### 5.3 Token refresh — `auth.refresh`

`POST /general/refreshApi`. **Public.** Uses the Passport `refresh_token` grant.

Request body: `{ refreshToken, clientId, clientSecretKey }`.
Success → `data: { access_token, refresh_token }` (both rotated; persist both).

### 5.4 Silent auto-refresh + retry (mandatory in every SDK)

On any **protected** call:

1. Send the request with the current access token.
2. If the response is `401` **or** `resultType == 4`, and a refresh token exists,
   and this request has **not** already been retried:
   a. Call `auth.refresh` with the stored refresh token + client credentials.
   b. Persist the new tokens.
   c. Retry the original request **once**.
3. If the refresh call itself fails, or `resultType == 2` (logout), clear the
   token store and raise `AuthenticationError`.
4. Auto-refresh must be **concurrency-safe**: simultaneous 401s share a single
   in-flight refresh (no refresh stampede). Single-threaded SDKs (e.g. plain JS)
   gate on one shared promise; threaded SDKs (Java/C#/Go/C++) use a mutex.

The retry is bounded to one attempt to prevent loops.

### 5.5 Token store (pluggable)

A `TokenStore` abstraction holds the access + refresh tokens. Default
implementation is in-memory. Consumers may inject a custom store (file, DB,
secure storage). Required operations (named per language):

- get access token / get refresh token
- set tokens (access, refresh) — atomically
- clear (on logout / revoked session)

### 5.6 Registration — `auth.register`

`POST /patients/addNewPatient`. **Public** but guarded by SMS verification
(`checkPhoneVerificationSmsCode`) + throttle.

Request body: `name`, `surname`, `apiUserName`, `phoneNumber`, `password`,
`smsVerificationCode`, `response` (encrypted blob from the prior SMS-verify step),
`acceptUserAgreement` (1), `apiClientId`, `apiSecretKey`.

Rules (validated):
- `phoneNumber` must match `^[+]([0-9\s\(\)]*)$` — i.e. start with `+` and country
  code (e.g. `+90 555 111 22 33`). Bare digits are rejected.
- `apiUserName` is used as the `afterRegister` token username; send the **same**
  `+CC` value as `phoneNumber`, otherwise auto-login mints a wrong/empty token.
- Password is stored as `Hash::make(BULUT_API_ENC_KEY . password)` (bcrypt rounds=12).

Success → patient created + automatic `afterRegister` login → `data: { access_token, refresh_token }`.

> The prior SMS-verification step (`verifyAddingNewPatient`) that produces the
> `response` blob is **not in this collection**. SDKs expose `register` as-is and
> document that `response` + `smsVerificationCode` must be obtained beforehand.

### 5.7 Logout — `auth.disconnect`

`POST /general/disconnectApi`. **Bearer required** (`auth:patients,apiusers,doctors`).
Revokes the current access + refresh tokens server-side. The SDK then clears the
token store. Optional device-token fields (firebase/ios) may be added to the body.

---

## 6. Endpoint reference (27)

Notation: **Canonical name** = language-neutral concept → per-language naming
follows §7. `[public]` = no auth; `[bearer]` = access token; `[partner]` = partner
token; `[scope:…]` = required OAuth scope.

### 6.1 `auth`

| Canonical            | Method | Path                               | Auth     |
|----------------------|--------|------------------------------------|----------|
| `connect`            | POST   | `/general/connectApi`              | public   |
| `connectWithTwoFactor`| POST  | `/general/connectApiWithTwoFactor` | public   |
| `refresh`            | POST   | `/general/refreshApi`              | public   |
| `register`           | POST   | `/patients/addNewPatient`          | public*  |
| `disconnect`         | POST   | `/general/disconnectApi`           | bearer   |

(Bodies and responses in §5.)

### 6.2 `doctors`  `[bearer] [scope:patients,bulutweb]`

| Canonical      | Method | Path                                   | Body / params |
|----------------|--------|----------------------------------------|---------------|
| `branches`     | GET    | `/patients/allBranches`                | —             |
| `locations`    | GET    | `/patients/allLocations`               | —             |
| `quickSearch`  | POST   | `/patients/quickSearch`                | `searchText` (3–100, req), `listType` (`interview`\|`appointment`\|null), `location` (null) |
| `search`       | POST   | `/patients/filteredSearch`             | `searchParams{}`, `orderParams[]`, `otherParams[]`, `currentPage` (≥1, req), `perPageLimit` (10–100) |
| `detail`       | GET    | `/patients/doctorDetail/{id}/{corporate?}` | path `id` (req), optional `corporate` |

- `quickSearch` response: `{ searchedBranches, searchedDoctors, searchedCompanies, searchedGivenTreatments, searchedBlogs, queryText }`; each item `{ result_id, result_text, result_url, result_sub_text, result_type, result_image }`.
- `search.searchParams` keys: `withFreeText`, `withDoctorName`, `withBranchName`, `withBranchId` (`-1` excludes psychology/diet), `withLocationName`, `withLocationId`, `withCompanyName`, `withCompanyId`, `withGivenTreatments`, `withExpertyId`, `withInstitutionId`, `withNearestSlotDayRange`.
  `orderParams`: `name` | `point` | `slot` | `order`. `otherParams`: `isKizilay` | `isQuestionable` | `isInterviewable` | `isAppointmentable`.
  Response: `data: { foundDoctorsCount, foundDoctors: [ { doctor_id, name, surname, branch_name, star_rate, nearest_slot, isInterviewable, isAppointmentable, url, user_image, … } ] }`.
- `detail` returns `doctorGeneralInfo` (prices, session length, branch), education, languages, reviews, videos, special services, related clinics. The `doctor_id` here feeds later steps.

### 6.3 `slots`  `[bearer]`

| Canonical  | Method | Path                          | Body |
|------------|--------|-------------------------------|------|
| `schedule` | POST   | `/patients/doctorScheduler`   | `doctorId` (numeric, req); `scheduleDate` (`Y-m-d`, today..+21, optional); `scheduleStep` + `schedulePage` (window paging — both required when `scheduleDate` omitted); `listType` (req: `interview` → online slot_type 1,2; else physical slot_type 0,2) |

Response: `data` = date-keyed map → for each date `[ { slotId, slotStart "HH:mm:ss", slotEnd "HH:mm:ss", available: true } ]`. Empty days are `[]`.
Next step's `appointmentDate` = `"Y-m-d H:i"` (date key + `slotStart`, **drop seconds**).

### 6.4 `appointments`  `[bearer] [scope:patients,bulutweb]`

| Canonical          | Method | Path                                       | Body / params |
|--------------------|--------|--------------------------------------------|---------------|
| `reserveInterview` | POST   | `/patients/addInterviewDateReservation`    | `doctorId` (numeric, req), `appointmentDate` (`Y-m-d H:i`, today..+21, req), `appointmentType` (`interview`\|`appointment`, default `interview`) |
| `addPhysical`      | POST   | `/patients/addNewAppointment`              | `doctorId` (numeric, req), `appointmentDate` (`Y-m-d H:i`, req). No `appointmentType`. |
| `cancel`           | DELETE | `/patients/deleteUserAppointment/{eventId}`| path `eventId` (= `cln_events.id`) |

`reserveInterview` success → `{ resultType: 0, data: null }`; failure → 501.
`cancel` → 501 for insurance appointments, past cancel-window, or not found.
Slot is resolved server-side from `doctorId` + `appointmentDate` (no `slotId` in request).

### 6.5 `payments`

| Canonical          | Method | Path                              | Auth   | Notes |
|--------------------|--------|-----------------------------------|--------|-------|
| `checkDiscountCode`| POST   | `/patients/checkDiscountCode`     | bearer | **`patients` prefix, not `payments`.** |
| `getCards`         | GET    | `/payments/getCards`              | bearer | |
| `saveCard`         | POST   | `/payments/saveCard`              | bearer | Flat fields (not nested). |
| `pay`              | POST   | `/payments/interviewPayment`      | bearer | Throttle 20/h/IP. Returns `payment3DUrl`. |
| `deleteCard`       | DELETE | `/payments/deleteCard/{cardId}`   | bearer | path `cardId` |

- `checkDiscountCode` body: `checkType` (`question`\|`appointment`\|`lab`\|`special`\|`physicallyAppointment`\|`tmcLab`\|`program`), `doctorId` (required except lab/tmcLab/program), `discountCode` (req), plus `orderId`/`specialServiceId`/`programSlug` per type. Valid → `data: { discount_code, discount_title, discount_id, prices }`.
- `getCards` → `data.cards[]: { id, card_holder_name, card_number (masked), card_type, created_at }`. `id` → `cardId`.
- `saveCard` body (flat, `SavePatientCardRequest`): `cardHolder`, `cardNumber`, `cardExpMonth` (`m`), `cardExpYear` (`Y`), `cardCvv` — all required.
- `pay` body: `doctorId` (req), `appointmentDate` (`Y-m-d H:i`, req), `appointmentType` (`interview`→order_type 0 / `appointment`→3), `is3D` (bool, req), `termsAccept` (accepted, req), `saveCard` (1=tokenize), `discountCode` (opt), `caseDetail` (opt, encrypted), **and** either `cardInfo{ cardHolder, cardNumber, cardExpMonth, cardExpYear, cardCvv }` (all-or-none) **or** `cardId` (saved card). Amount is computed server-side (no `amount` in request).
- `pay` response: see §8.1 (`payment3DUrl` handling).

### 6.6 `measures`

Patient endpoints `[bearer] [scope:patients]`; partner endpoint `[partner] [scope:teusan]`.
Records are written to the authenticated patient (`bas_com_company_id` from token).

| Canonical                  | Method | Path                                                 | Body / params |
|----------------------------|--------|------------------------------------------------------|---------------|
| `addList`                  | POST   | `/patients/addNewUserMeasures`                       | `data[]` — each item: `type` + that type's fields + `date_time`. **Primary "submit health data" endpoint.** |
| `add`                      | POST   | `/patients/addNewUserMeasures/{type}`                | path `type`; body: `date_time` + type fields |
| `update`                   | PUT    | `/patients/updateUserMeasures/{type}`                | path `type`; body: `id` (req) + fields + `date_time` |
| `delete`                   | DELETE | `/patients/deleteUserMeasures/{type}`                | path `type`; body: `id` (req) |
| `last`                     | GET    | `/patients/measuresList`                             | Latest value per type. |
| `list`                     | GET    | `/patients/userMeasuresList/{type}/{page}/{glucoseType?}` | path; `glucoseType` 0/1 only for glucose |
| `graph`                    | GET    | `/patients/userMeasuresGraph/{type}/{period}/{page}/{glucoseType?}` | `period` 1=day,2=week,3=month,4=year |
| `partnerHealthInformation` | POST   | `/outher/healthInformation`                          | partner token; body: `identity`, `phoneNumber`, `data[]` |

`addList` runs in a DB transaction; submit multiple measurements in one call.
`last` returns the most-recent of each type (tension splits into hypertension/hypotension; glucose splits into `hunger_glucose`/`postprandial_glucose`), each with a `*Date`.

**Measure type schema** (every record also requires `date_time` = `"Y-m-d H:i"`):

| `type`    | Fields                                              |
|-----------|-----------------------------------------------------|
| `tension` | `hypertension` (systolic), `hypotension` (diastolic) |
| `glucose` | `glucose`, `glucose_type` (0=fasting, 1=postprandial) |
| `pulse`   | `pulse`                                             |
| `fever`   | `fever`                                             |
| `weight`  | `weight` (BMI auto-computed)                        |
| `length`  | `length` (BMI auto-computed)                        |
| `waist`   | `waist`                                             |
| `hip`     | `hip`                                               |
| `fat`     | `fat`                                               |
| `muscle`  | `muscle`                                            |
| `calorie` | `calorie`                                           |
| `step`    | `step`                                              |
| `sleep`   | `sleep` (hours; stored to `sleep_time`)             |

Value rules: numeric; `tension`/`pulse` digits 1–10; `glucose` 0–99999.99 + `glucose_type` 0\|1; `weight`/`length` 0–99999.99; etc.

> **Known API bug (document, don't replicate):** for the partner endpoint,
> `AddNewUserMeasuresListRequest::prepareForValidation` reads `identity` from
> `$this->message` instead of `$this->identity`, nulling it during validation; in
> practice matching falls back to `phoneNumber`. The SDK sends the correct
> contract (`identity` + `phoneNumber`) and notes this in the README.

---

## 7. Naming conventions & API shape

The client is a single root object exposing one accessor per service group; each
group exposes the canonical methods above.

```
client.auth.connect(...)            client.payments.pay(...)
client.doctors.search(...)          client.measures.addList(...)
client.slots.schedule(...)          client.appointments.reserveInterview(...)
```

Per-language casing & idioms:

| Language | Method case | Notes |
|----------|-------------|-------|
| JS/TS    | `camelCase` | `client.doctors.quickSearch()`. Promise-based. |
| PHP      | `camelCase` | `$client->doctors->quickSearch()`. Namespace `Bulutklinik\Sdk`. |
| Python   | `snake_case`| `client.doctors.quick_search()`. Sync **and** async (`AsyncClient`). |
| Go       | `PascalCase`| `client.Doctors.QuickSearch(ctx, …)`. Context-first, `(T, error)` returns. |
| Java     | `camelCase` | `client.doctors().quickSearch(…)`. Builder for config; checked vs unchecked TBD in Faz 3. |
| C#       | `PascalCase`+`Async` | `client.Doctors.QuickSearchAsync(…)`. `Task<T>`, `CancellationToken`. |
| C++      | `snake_case`| `client.doctors().quick_search(…)`. Namespace `bulutklinik`. cpr + nlohmann/json. |

Request inputs are typed structures (objects/records/structs) per language;
responses are typed where practical, otherwise a typed envelope + parsed `data`.

### 7.1 Client configuration

| Option        | Default        | Purpose                                            |
|---------------|----------------|----------------------------------------------------|
| `environment` / `baseUrl` | `production` | Named preset or explicit URL.            |
| `lang`        | `tr`           | Default `lang` header; overridable per request.    |
| `clientId` / `clientSecret` | —  | Needed for `refresh` (and passed by `connect`).    |
| `tokenStore`  | in-memory      | Pluggable persistence.                             |
| `timeout`     | sane default   | Request timeout.                                   |
| `httpClient`  | platform default | Injectable transport (PSR-18, http.Client, HttpClient, etc.). |

### 7.2 Escape hatch — arbitrary requests

Not every endpoint has a typed resource method, and the API grows faster than the
SDK surface. Every SDK therefore exposes **one generic request method on the root
client** for calling any Bulutklinik API endpoint directly. It is not a separate
HTTP client: it reuses the same transport, so default headers, the chosen auth
mode, silent token refresh + retry (§5.4), envelope unwrapping (§3) and the typed
error hierarchy (§4) all still apply.

Concept:

```
client.request(method, path, { auth, body, lang }) -> data
```

| Param    | Notes |
|----------|-------|
| `method` | `GET` \| `POST` \| `PUT` \| `DELETE`. |
| `path`   | Relative to the configured base URL, e.g. `/patients/allBranches`. Leading slash included. |
| `auth`   | `public` \| `bearer` (**default**) \| `partner`. Accepted as a string or an existing public enum/const per language. |
| `body`   | Optional JSON payload (object/map/dict). Omitted on `GET`. |
| `lang`   | Optional per-request `lang` override, where the SDK's transport supports one (JS, PHP, Go, C++). Python / Java / C# apply the client-level `lang`. |

Returns the unwrapped `data` payload as the language's raw JSON value (the same
type a future typed resource method would parse from), and raises the same typed
errors on failure. Representative per-language signatures (idiomatic, return the
raw `data`):

| Language | Signature |
|----------|-----------|
| JS/TS    | `client.request<T>({ method, path, auth?, body?, lang? }): Promise<T>` |
| Python   | `client.request(method, path, *, auth="bearer", body=None)` — plus the async client |
| PHP      | `$client->request(string $method, string $path, string $auth = 'bearer', ?array $body = null, ?string $lang = null): mixed` |
| Go       | `client.Do(ctx, method, path, *bk.RequestOptions) (json.RawMessage, error)` (nil options ⇒ bearer) |
| Java     | `client.request(String method, String path, String auth, Object body)` → `JsonNode` |
| C#       | `client.RequestAsync(HttpMethod method, string path, string auth = "bearer", object? body = null, CancellationToken = default)` → `JsonElement` |
| C++      | `client.request(method, path, bulutklinik::RequestOptions{})` → `nlohmann::json` |

This is the supported extension point for endpoints outside the 27 in §6. Prefer a
typed resource method when one exists; reach for `request` only for the gaps.

---

## 8. Special cases

### 8.1 `payment3DUrl` (3-D Secure) — passthrough

`pay` success response: `{ resultType: 0, data: { payment3DUrl: "<url>" } }`.
`payment3DUrl` is a **browser URL** the SDK returns verbatim — it is one of:
  (A) the bank's direct `URL_3DS`, or
  (B) `{APP_URL}/api/v3/payments/threeDUrl/<token>` (our endpoint serving the 3DS HTML form).

The SDK **does not** open, follow, or parse it. 3DS completion ("provizyon
kapatma" / capture) happens browser↔bank↔server via the
`POST /api/v3/threeD/appointmentPaymentComplete/{trxId}/{driver}` callback
(`trxId = "{orderId}.{transactionUuid}.{processId}"`) — outside SDK scope.
If `is3D = false`, `data` is the inline-completed order result (no `payment3DUrl`).

### 8.2 Encrypted blobs — passthrough

`connect`'s `data.response` (2FA), `register`'s `response`, and `caseDetail` are
opaque encrypted blobs. The SDK passes them through verbatim and never encrypts
or decrypts. The clinic/API encryption keys are never embedded in the SDK.

### 8.3 Public vs bearer vs partner

- Public (no `Authorization`): `connect`, `connectWithTwoFactor`, `refresh`, `register`.
- Bearer (access token): everything else.
- Partner: `partnerHealthInformation` uses a separately-configured partner token
  (`scope:teusan`), not the patient access token.

---

## 9. Cross-cutting requirements (every SDK)

1. **Idiomatic, hand-written** — no codegen; match each ecosystem's conventions.
2. **Minimal dependencies** — prefer the platform HTTP client; pin the documented
   stack per language (see §7 / PLAN.md).
3. **Typed** — public API and `data` payloads typed where the language supports it.
4. **Auto-refresh + retry** per §5.4, concurrency-safe.
5. **Pluggable** token store and HTTP client.
6. **Errors** per §4 with full context.
7. **Tested** — unit tests for envelope/error/refresh logic + at least one live
   smoke path against `test` env (Faz 1–2).
8. **Examples** — `examples/` with the end-to-end flow: login → search → slot →
   reserve → (pay) and a measures example.
9. **Self-contained repo** — README, LICENSE (MIT), DESIGN.md copy, CI.
10. **Versioning** — semver; tag `vX.Y.Z` per repo.

---

## 10. Live validation reference (test env)

- Base: `https://apitest.bulutklinik.com/api/v3`
- OAuth client: `Patients_Web_Mobile` — id `96b630b3-f62a-4e67-b33c-b58802dca5af` (secret in the collection / env file).
- Test patient: `hackathon@bulutklinik.test` (`loginMode: email`).
- Bookable `doctorId` examples: `8282` (interview + physical), `168896` (interview).
- Known env limits (request is correct, server/env is the cause):
  - `quickSearch` returns HTTP 404 / `resultType 1` on `test` — the search driver
    (Elasticsearch) is unavailable there; the controller catches only
    `QueryException` so other exceptions surface as a generic 404. `filteredSearch`
    (`doctors.search`) works and is the production search path.
  - `interviewPayment` may 404 if POS isn't configured for the company; 3DS capture
    can't run from a non-browser client. SDK validation asserts the request shape +
    `payment3DUrl` return, not the bank capture.
- TS reference live result (2026-06-17, `test`): 8/9 steps OK — `auth.connect`,
  `doctors.branches` (136), `doctors.locations` (81), `doctors.search`,
  `doctors.detail`, `slots.schedule`, `measures.last`, `auth.disconnect` all pass;
  only `quickSearch` fails for the env reason above.

---

## 11. Change control

This file is canonical. When it changes:
1. Bump the spec version (§ top).
2. Copy it into every language repo (`<repo>/DESIGN.md`).
3. Reconcile each SDK against the change; note breaking changes in repo CHANGELOGs.

If an SDK must diverge from this spec, fix the spec first (or record the
divergence here) — code and SSOT must never silently disagree.
