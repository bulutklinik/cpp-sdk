# Bulutklinik SDK — Canonical Design (SSOT)

> **This file is the single source of truth (SSOT) for every official Bulutklinik
> SDK.** All language packages (JavaScript/TypeScript, PHP, Python, Go, Java, C#,
> C++) are hand-written but MUST implement exactly the contract described here.
> The canonical copy lives at `dev-kits/DESIGN.md`; an identical copy is vendored
> into each language repository and re-synced whenever this file changes.
>
> Wire contract is derived from the BulutklinikAPI source (Laravel 8.12,
> OAuth2/Passport) — `app/Packages/Integration/Outher` and `routes/{v3,v4}/outher.php`.

- **Spec version:** 1.0.1 — **breaking** (from 0.6.x). The SDKs become a single-persona,
  partner-only surface. Everything that required a patient login is gone; every
  method now runs on the company-scoped `/outher` channel with a pre-issued
  partner token. See §12 for what was removed and why.
- **API:** BulutklinikAPI `v3` (default) or `v4` — selectable per client.
- **Scope:** 6 services / 28 endpoints (partner persona).

---

## 1. Scope

The SDKs expose the **partner** persona: a clinic-integration channel where the
caller is a company, authenticated by a pre-issued partner token, acting on the
patients of **its own company**.

| Service        | Endpoints | Purpose                                                     |
|----------------|:---------:|-------------------------------------------------------------|
| `doctors`      | 4         | Doctor discovery: search, branches, detail, city list       |
| `slots`        | 1         | Doctor availability (materialized slots)                    |
| `appointments` | 9         | Reserve, confirm, free-form booking, cancel, list, lookup   |
| `measures`     | 8         | Health measurements for a named patient (read + write)      |
| `laboratory`   | 4         | Lab results for a named patient + orderable test catalog    |
| `diets`        | 2         | Diet lists written by a dietitian, for a named patient      |

### 1.1 What "partner persona" means

| | partner (this SDK) |
|---|---|
| Who authenticates | your **company**, via a pre-issued partner token |
| Whose data you see | patients of **your own company only** |
| How a patient is named | **inline on every call** (`patient` / `user` object) — there is no session |
| Token lifecycle | issued out of band, ~30 days; **the SDK cannot refresh it** (§5.3) |

There is no patient login, no session, and no per-user access token. Two calls
for two different patients are indistinguishable to the transport — the patient
reference travels in the request body.

**Patient identity is carried in the body, never in the URL.** A TCKN in a path
segment would land in access logs, proxy logs and Sentry breadcrumbs. This is why
several read endpoints are `POST` although they are semantically reads.

### 1.2 Deliberately out of scope

Not exposed, because the API has no company-scoped equivalent:

- **Patient authentication and registration** — login, 2FA, token refresh, sign-up,
  social sign-up, password reset, logout.
- **Payments** — discount codes, the saved-card vault, 3-D Secure. Partner booking
  hands payment off to a browser `url` (§6.3); no partner endpoint produces a
  financial record.
- **Self-service AI** — skin-lesion analysis, meal photo analysis.
- **Patient address book** — it exists only to feed the patient-side lab order,
  which is itself unavailable to partners.
- Other partner scopes that exist server-side but are separate integrations:
  `apilab` (laboratory result write-back), `apidevice` (medical devices), and the
  plain-`apiusers` doctor-calendar endpoints. These may be added later as their
  own resource groups.

---

## 2. Environments & transport

### 2.1 Base URLs

The base URL is `<api root>/<apiVersion>`.

| Env          | API root                                 |
|--------------|------------------------------------------|
| `production` | `https://api.bulutklinik.com/api`        |
| `test`       | `https://apitest.bulutklinik.com/api`    |
| `local`      | `https://api-bulutklinik.test/api` (Herd)|

| `apiVersion` | Segment | Notes                                                    |
|--------------|---------|----------------------------------------------------------|
| `v3`         | `/v3`   | **Default.** The long-standing surface.                  |
| `v4`         | `/v4`   | The consolidated architecture. Route-for-route identical for `/outher` — the API's `outher:audit-routes` command enforces v3/v4 parity. |

The client accepts a named environment preset **plus** an `apiVersion`, or an
explicit `baseUrl` that overrides both. Defaults: `production` + `v3`.

> Every path in §6 is version-agnostic (`/outher/...`); only the base URL differs.
> Switching `apiVersion` is a configuration change, not a code change.

### 2.2 Required headers

| Header         | Value                          | Notes                                       |
|----------------|--------------------------------|---------------------------------------------|
| `Accept`       | `application/json`             | Always.                                     |
| `Content-Type` | `application/json`             | On requests with a body.                    |
| `lang`         | `tr` (default), `en`, `de`, `az` | Configurable per-client and per-request.  |
| `Authorization`| `Bearer <partnerToken>`        | On every endpoint in §6.                    |

### 2.3 HTTP methods

Endpoints use `GET`, `POST`, `PUT`, `DELETE` as specified per endpoint in §6.
Path parameters (e.g. `{type}`, `{period}`, `{doctorId}`) are URL segments, not
query string. Request bodies are JSON.

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
| `2`   | Logout   | Clear the token store, raise `AuthenticationError` (token revoked).          |
| `3`   | Update   | Raise `ApiError` with an "update required" marker.                           |
| `4`   | Refresh  | The partner token is expired or invalid. **There is nothing to refresh** (§5.3) — raise `AuthenticationError` telling the caller to install a newly issued token. |

> Implementation note: `/outher` returns `resultType 4` with HTTP `401` on an
> expired token. A bare HTTP `401` without a parseable envelope MUST be treated
> identically. Neither triggers a retry.

### 3.2 The `501` convention

`/outher` reports most business-rule failures as HTTP **`501`** with
`resultType 1` — "patient not found in your company", "diet list is not yours",
"slot no longer free", "doctor not bookable through your integration". It is not
a server crash. Callers should read `errorMessage`, not the status code alone.

The read endpoints deliberately return the **same** message for "this patient is
not in your company" and "this patient does not exist". Distinguishing them would
turn the endpoint into a TCKN-probing oracle.

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
    ├── AuthenticationError       (401 / resultType 2 / resultType 4 — token invalid, expired or revoked)
    ├── AuthorizationError        (403 — authenticated but the token lacks the scope, or carries no company)
    ├── NotFoundError             (404)
    └── RateLimitError            (429 — throttled; carries Retry-After if present)
```

Each `ApiError` carries: `httpStatus`, `resultType`, `errorType`, `errorMessage`,
the raw `data`, and the originating request (method + path) for debugging.
Mapping precedence: logout/expiry (`resultType == 2` or `4`) → string
`errorType == "validation"` → HTTP status (401→Auth, 403→Authz, 404→NotFound,
422→Validation, 429→RateLimit) → otherwise (incl. numeric `errorType`, or success
HTTP with `resultType != 0`) → `ApiError`.
Because `errorType` may be numeric (§3), guard before string-matching it.

> `403` deserves a note: it is raised not only for a missing `apiouther` scope but
> also when the token resolves to a user with no company. The company boundary is
> derived from the authenticated principal, never from request input — so a `403`
> here means the credential itself is wrong, and retrying with different body
> parameters will never help.

---

## 5. Authentication

### 5.1 The partner token

OAuth2 via Laravel Passport, guard `apiusers`, scope `apiouther` (plus `teusan`
for `measures.healthInformation`). The token is **issued out of band** — through
the Bulutklinik Developer Platform, not by any SDK call. There is no
client-credentials grant the SDK can drive, and no `oauth/token` request.

Consequences the SDKs must honour:

1. The token is a **configuration input**, like an API key.
2. There is **no login method** on the client.
3. There is **no auto-refresh** and no retry-after-refresh (contrast: spec 0.x §5.4).
4. The company the token belongs to is fixed at issue time. It cannot be
   overridden per request.

### 5.2 Token store (pluggable)

The token is read through a `TokenStore` on **every** request, so a long-lived
process can rotate the credential without being rebuilt — point the store at a
file, a database, or a secret manager and the next call picks up the new value.

Required operations (named per language):

| Operation  | Purpose                                                          |
|------------|------------------------------------------------------------------|
| get token  | Return the current partner token, or null/empty if none.         |
| set token  | Replace the stored token (accepts null to unset).                |
| clear      | Drop the stored token. Called automatically on `resultType 2`.    |

The default implementation is in-memory. The `partnerToken` client option is a
convenience that seeds one:

```
new Client({ partnerToken: "…" })           ⇒ in-memory store seeded with the token
new Client({ tokenStore: myVaultStore })    ⇒ the token comes from your store
new Client({ partnerToken: …, tokenStore: … })  ⇒ configuration error, raised at construction
```

Passing both is rejected rather than silently resolved: either the literal or the
store is the source of truth, and guessing which one the caller meant is how
credential bugs get shipped.

If no token is available when a request is dispatched, the SDK raises
`AuthenticationError` **before** touching the network — an unauthenticated call to
`/outher` would only come back as a confusing `401`/`resultType 4`.

### 5.3 Expiry

Passport issues these tokens with a ~30 day lifetime. When one expires the API
answers `401` + `resultType 4`; the SDK raises `AuthenticationError` and does
**not** retry. Recovery is operational: obtain a newly issued token and write it
into the token store (or rebuild the client). SDK READMEs must say this plainly —
`resultType 4` used to mean "the SDK will fix this silently" and now means the
opposite.

---

## 6. Endpoint reference (28)

Notation: **Canonical name** = language-neutral concept → per-language naming
follows §7. Every endpoint below requires the partner token; the scope column
lists the OAuth scope the token must carry.

Two patient-reference shapes recur; both are defined in §8.1.

- **`patientRef`** — `{ identityNumber?, phoneNumber? }`, at least one. Used by
  **reads**. Never creates anything.
- **`bookingUser`** — `{ name, surname, phoneNumber, identityNumber?, email?, birthdate?, nationality?, price? }`.
  Used by **writes**. Creates the patient in your company if absent.

### 6.1 `doctors`  `[scope:apiouther]`

| Canonical   | Method | Path                          | Body / params |
|-------------|--------|-------------------------------|---------------|
| `search`    | POST   | `/outher/search`              | `searchParams{}` (req), `orderParams[]` (`name`\|`order`\|`slot`), `currentPage` (≥1, req) |
| `branches`  | GET    | `/outher/branches`            | — |
| `detail`    | GET    | `/outher/doctorInfos/{doctorId}` | path `doctorId` (req, numeric) |
| `locations` | GET    | `/outher/locations`           | — |

- Results are filtered to the doctors enabled for your integration (the server
  applies your partner slug), so anything returned here is bookable by you.
  `locations` is the exception: a global city catalogue, not company-scoped.
- `search.searchParams` accepts the same keys as the patient-side filtered search
  (`withFreeText`, `withDoctorName`, `withBranchName`, `withBranchId`,
  `withLocationName`, `withLocationId`, `withCompanyName`, `withCompanyId`,
  `withGivenTreatments`, `withExpertyId`, `withInstitutionId`,
  `withNearestSlotDayRange`). Response: `{ foundDoctorsCount, foundDoctors: [ { doctor_id, name, surname, branch_name, … } ] }`.
  Note `orderParams` here is narrower than the patient surface — `point` is not accepted.
- **`searchParams` must contain at least one key.** Its rule is `required|array`,
  and PHP's `required` rejects an empty array — so `{}` is a guaranteed `422`,
  not an unfiltered search. SDKs must therefore make `searchParams` a required
  argument and must not default it to an empty map.
- `detail` `doctor_id` feeds `slots.schedule` and the booking calls.

### 6.2 `slots`  `[scope:apiouther]`

| Canonical  | Method | Path                    | Body |
|------------|--------|-------------------------|------|
| `schedule` | POST   | `/outher/doctorSlots`   | `doctorId` (numeric, req); `scheduleDate` (`Y-m-d`, today..+21, optional); `scheduleStep` + `schedulePage` (window paging — both required when `scheduleDate` omitted) |

Response: `data` = date-keyed map → for each date
`[ { slotId, slotStart "HH:mm:ss", slotEnd "HH:mm:ss", available: true } ]`.
Empty days are `[]`. `slotId` feeds `appointments.reserve`; an
`appointmentDate` elsewhere is `"Y-m-d H:i"` (date key + `slotStart`, **seconds dropped**).

Unlike the patient surface there is no `listType` — the partner channel is online
interviews.

### 6.3 `appointments`  `[scope:apiouther]`

| Canonical                | Method | Path                              | Body / params |
|--------------------------|--------|-----------------------------------|---------------|
| `reserve`                | POST   | `/outher/reservation`             | `slotId` (req), `doctorId` (req), `user{}` = bookingUser |
| `reserveWithoutAgreement`| POST   | `/outher/reservationWithoutAgreement` | same as `reserve` |
| `instantReserve`         | POST   | `/outher/instantReservation`      | `user{}` = bookingUser |
| `create`                 | POST   | `/outher/appointment`             | `hash` (req), `outherProcessId` (req, numeric) |
| `createWithoutSlot`      | POST   | `/outher/appointmentWithoutSlot`  | `doctorId` (req), `startDate` (`Y-m-d H:i`, ≥ today, req), `finishDate` (`Y-m-d H:i`, after `startDate`, req), `isOutherDoctor` (0\|1), `user{}` = bookingUser |
| `cancelWithoutSlot`      | DELETE | `/outher/appointmentWithoutSlot`  | appointment lookup (below) |
| `list`                   | POST   | `/outher/appointments`            | `phoneNumber` (req), `page` (≥1), `type` (`normal`\|`instant`) |
| `info`                   | POST   | `/outher/appointmentInfo`         | appointment lookup (below) |
| `checkDoctor`            | POST   | `/outher/checkDoctor`             | `doctorId` (req, numeric), `isOutherDoctor` (req, 0\|1) |

**Appointment lookup** (`info`, `cancelWithoutSlot`) addresses one appointment
either **by process** — `hash` + `outherProcessId` — or **by coordinates** —
`doctorId` + `appointmentDate` (`Y-m-d H:i`) + `isOutherDoctor`. Send one pair or
the other; the server validates them as mutually `required_without`.

**The two booking flows:**

```
(A) hand off to the patient      reserve  ──▶ data.url  ──▶ patient opens it in a
                                                            browser: agreements + payment
(B) you collected the agreements  reserveWithoutAgreement ──▶ data.hash
                                          └─▶ create(hash, outherProcessId) ──▶ appointment
```

- `reserve` → `{ url, hash }`. `url` is a short link to the Bulutklinik agreement
  and payment page; hand it to the patient. The SDK returns it verbatim and never
  opens or follows it.
- `reserveWithoutAgreement` → `{ hash, doctorId, slotId, phoneNumber, reservationExpired }`.
  `reservationExpired` (`Y-m-d H:i:s`) is the hold deadline — `create` after it
  passes fails with `501`.
- `instantReserve` → `{ url }`. No slot: the server picks an available doctor.
- `create` → the appointment record plus `last_delete_time` (the cancellation
  deadline).
- `createWithoutSlot` books a free-form range outside the slot grid, for
  integrations running their own calendar. `cancelWithoutSlot` reverses it — and
  **only** it; appointments created through `create` are not cancellable here.
- `list` returns the appointments **you** created for that phone number, not the
  patient's full history across the platform.
- `checkDoctor` → `{ title, name, surname, branch_name, state: "1" }` when the
  doctor is bookable through your integration; `501` when not. Call it before
  showing a doctor as reservable.

> Completing a payment (`outherProcess`) is **not** on the partner surface: those
> routes require a `patients`/`bulutweb` scope. Flow (A) exists precisely because
> the browser hand-off is where payment happens.

### 6.4 `measures`  `[scope:apiouther]` (`healthInformation`: `[scope:teusan]`)

Reads resolve the patient inside your company and never create one. Writes create
the patient if needed. **Measurements are written to your own company** — a value
you write does not appear in the patient's Bulutklinik mobile app. That is the
intended consequence of tenant isolation, not a bug.

| Canonical           | Method | Path                                          | Body / params |
|---------------------|--------|-----------------------------------------------|---------------|
| `last`              | POST   | `/outher/lastMeasures`                        | `patient{}` = patientRef |
| `list`              | POST   | `/outher/measuresList/{type}`                 | path `type`; `patient{}` = patientRef, `currentPage` (≥1), `glucoseType` (0\|1, glucose only) |
| `graph`             | POST   | `/outher/measuresGraph/{type}/{period}`       | path `type`, `period` (1=day,2=week,3=month,4=year); `patient{}` = patientRef, `currentPage`, `glucoseType` |
| `addList`           | POST   | `/outher/measures`                            | `patient{}` = bookingUser, `data[]` (1–200 items) — each item `type` + that type's fields + `date_time` |
| `add`               | POST   | `/outher/measure/{type}`                      | path `type`; `patient{}` = bookingUser, `date_time` + type fields |
| `update`            | PUT    | `/outher/measure/{type}`                      | path `type`; `patient{}` = patientRef, `id` (req) + fields + `date_time` |
| `delete`            | DELETE | `/outher/measure/{type}`                      | path `type`; `patient{}` = patientRef, `id` (req) |
| `healthInformation` | POST   | `/outher/healthInformation`                   | `identity`, `phoneNumber`, `data[]` — legacy flat contract, **not** `patient{}` |

- `addList` writes every row in **one transaction**, capped at **200 items** —
  without a cap a single request would hold a transaction open across thousands of
  rows and block on the `med_monitor_*` tables.
- `update`/`delete` take the read-side `patient{}` (if there is a row to change,
  the patient already exists) and bound the write to `id` + patient + company.
- `id` for `update`/`delete` comes from `list`.

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

Value rules: numeric; `tension`/`pulse` digits 1–10; `glucose` 0–99999.99 +
`glucose_type` 0\|1; `weight`/`length` 0–99999.99; etc.

`last` returns the most recent of each type (tension splits into
hypertension/hypotension; glucose into `hunger_glucose`/`postprandial_glucose`),
each with a `*Date`.

> **`healthInformation` is the odd one out.** It predates the `patient{}`
> contract, needs the `teusan` scope instead of `apiouther`, and takes a flat
> `identity` + `phoneNumber`. Prefer `addList` for new integrations.
>
> **Its patient matching is an OR, and it is loose.** The lookup is
> `WHERE identity = … OR phone_number = …` against the **global** user table,
> taking the first row (`PatientUsersModel::patientUserFindWithOr`). A phone
> number alone can therefore resolve a person whose TCKN differs from the one you
> sent. Send both fields, but do not assume they are checked as a pair. This is
> the exact opposite of the `apiouther` reads in this group, which scope to your
> own company and fail closed on ambiguity (§8.1) — another reason to prefer
> `addList`.
>
> (Spec 0.x documented a defect here that nulled `identity` outright before it
> reached the lookup. That was fixed API-side on 2026-07-21; the OR remains.)

### 6.5 `laboratory`  `[scope:apiouther]`

| Canonical       | Method | Path                                  | Body / params |
|-----------------|--------|---------------------------------------|---------------|
| `catalog`       | GET    | `/outher/laboratoryCatalog`           | — |
| `catalogDetail` | GET    | `/outher/laboratoryCatalog/{testId}`  | path `testId` (req, numeric) |
| `results`       | POST   | `/outher/laboratoryResults`           | `patient{}` = patientRef, `currentPage` (≥1) |
| `resultDetail`  | POST   | `/outher/laboratoryResult`            | `patient{}` = patientRef, `testId` (req) |

- `catalog` / `catalogDetail` are the **global orderable-test catalogue** — static
  data, no patient and no company scoping.
- `resultDetail.testId` must be passed back **exactly** as `results` returned it:
  a plain number is an HBYS lab request, a `-lab` suffix marks a TmcLab order
  group (server pattern: `/^\d+(-lab)?$/`). The SDK does not parse or normalise it.
- Ordering a test is **not** available to partners (it creates a financial record).

### 6.6 `diets`  `[scope:apiouther]`

| Canonical | Method | Path                 | Body |
|-----------|--------|----------------------|------|
| `list`    | POST   | `/outher/dietLists`  | `patient{}` = patientRef, `currentPage` (≥1) |
| `detail`  | POST   | `/outher/diet`       | `patient{}` = patientRef, `listId` (req, numeric) |

- `list` → `{ foundDietsCount, foundDiets: [ { list_id, diet_date, protocol_no, patient_*, doctor_* } ] }`.
  Page size is fixed to 20 server-side. `list_id` feeds `detail`.
- `detail` → an **array of meal-time groups**
  `[ { time, meals: [ { meal_time, total_calories, …, meal_details: [ { quantity, explanation, meal_name, kcal, unit } ] } ] } ]`.
  A `listId` that is not this patient's returns `501` with the generic message.

---

## 7. Naming conventions & API shape

The client is a single root object exposing one accessor per service group; each
group exposes the canonical methods above. **There is no namespace prefix** — the
partner surface *is* the surface.

```
client.doctors.search(...)          client.measures.addList(...)
client.slots.schedule(...)          client.laboratory.results(...)
client.appointments.reserve(...)    client.diets.list(...)
```

Per-language casing & idioms:

| Language | Method case | Notes |
|----------|-------------|-------|
| JS/TS    | `camelCase` | `client.doctors.search()`. Promise-based. |
| PHP      | `camelCase` | `$client->doctors->search()`. Namespace `Bulutklinik\Sdk`. |
| Python   | `snake_case`| `client.doctors.search()`. Sync **and** async (`AsyncClient`). |
| Go       | `PascalCase`| `client.Doctors.Search(ctx, …)`. Context-first, `(T, error)` returns. |
| Java     | `camelCase` | `client.doctors().search(…)`. Builder for config. |
| C#       | `PascalCase`+`Async` | `client.Doctors.SearchAsync(…)`. `Task<T>`, `CancellationToken`. |
| C++      | `snake_case`| `client.doctors().search(…)`. Namespace `bulutklinik`. cpr + nlohmann/json. |

Request inputs are typed structures (objects/records/structs) per language;
responses are typed where practical, otherwise a typed envelope + parsed `data`.

Where a method name would collide with a language keyword, the language's own
escape applies — e.g. C++ `measures.delete_measure(...)`, since `delete` is
reserved.

### 7.1 Client configuration

| Option        | Default        | Purpose                                            |
|---------------|----------------|----------------------------------------------------|
| `environment` | `production`   | Named preset (`production` \| `test` \| `local`).   |
| `apiVersion`  | `v3`           | `v3` \| `v4`. Combined with `environment` to build the base URL. |
| `baseUrl`     | —              | Explicit URL; overrides `environment` + `apiVersion`. |
| `lang`        | `tr`           | Default `lang` header; overridable per request.    |
| `partnerToken`| —              | The partner token. Seeds the default in-memory store. |
| `tokenStore`  | in-memory      | Pluggable token source (§5.2). Mutually exclusive with `partnerToken`. |
| `timeout`     | sane default   | Request timeout.                                   |
| `httpClient`  | platform default | Injectable transport (PSR-18, http.Client, HttpClient, etc.). |

`clientId` / `clientSecret` are **gone** — they existed only for the patient
password and refresh grants.

### 7.2 Escape hatch — arbitrary requests

Not every endpoint has a typed resource method, and the API grows faster than the
SDK surface. Every SDK therefore exposes **one generic request method on the root
client** for calling any Bulutklinik API endpoint directly. It is not a separate
HTTP client: it reuses the same transport, so default headers, the chosen auth
mode, envelope unwrapping (§3) and the typed error hierarchy (§4) all still apply.

Concept:

```
client.request(method, path, { auth, body, lang }) -> data
```

| Param    | Notes |
|----------|-------|
| `method` | `GET` \| `POST` \| `PUT` \| `DELETE`. |
| `path`   | Relative to the configured base URL, e.g. `/outher/branches`. Leading slash included. |
| `auth`   | `partner` (**default**) \| `public`. Accepted as a string or an existing public enum/const per language. |
| `body`   | Optional JSON payload (object/map/dict). Omitted on `GET`. |
| `lang`   | Optional per-request `lang` override, where the SDK's transport supports one (JS, PHP, Go, C++). Python / Java / C# apply the client-level `lang`. |

Returns the unwrapped `data` payload as the language's raw JSON value, and raises
the same typed errors on failure. Representative per-language signatures:

| Language | Signature |
|----------|-----------|
| JS/TS    | `client.request<T>({ method, path, auth?, body?, lang? }): Promise<T>` |
| Python   | `client.request(method, path, *, auth="partner", body=None)` — plus the async client |
| PHP      | `$client->request(string $method, string $path, string $auth = 'partner', ?array $body = null, ?string $lang = null): mixed` |
| Go       | `client.Do(ctx, method, path, *bk.RequestOptions) (json.RawMessage, error)` (nil options ⇒ partner) |
| Java     | `client.request(String method, String path, String auth, Object body)` → `JsonNode` |
| C#       | `client.RequestAsync(HttpMethod method, string path, string auth = "partner", object? body = null, CancellationToken = default)` → `JsonElement` |
| C++      | `client.request(method, path, bulutklinik::RequestOptions{})` → `nlohmann::json` |

`auth: "public"` exists for the handful of unauthenticated endpoints outside §6
that an integration may still need — e.g. `GET /general/getConfig` for the
`cities[].districts[]` list. Prefer a typed resource method when one exists.

---

## 8. Special cases

### 8.1 How a patient reference resolves

This is the single most important behaviour on the partner surface, and the two
paths are deliberately asymmetric.

**Read path** (`patientRef` — `measures.last`/`list`/`graph`/`update`/`delete`,
`laboratory.results`/`resultDetail`, `diets.list`/`detail`):

1. Searches **only** `pat_patients` rows belonging to your company. It never
   consults the global user table and **never creates** anything.
2. `identityNumber` (TCKN) is the primary selector.
3. `phoneNumber` is the fallback — used when no TCKN was sent, **or** when the
   TCKN missed (a patient you created without a TCKN is findable once you learn
   it). It is accepted only when it matches **exactly one** row; the column is not
   unique, since family members share numbers. Two matches fail closed.
4. When the fallback fires and the matched row has a *different* non-empty
   `identity_number`, the request is rejected — that is a different person.
5. Not found → `501` with the same generic message as "not yours" (§3.2).

**Write path** (`bookingUser` — every booking call, `measures.addList`/`add`):

1. Looks the person up globally by TCKN/phone and creates a password-less shadow
   user if absent.
2. Find-or-creates the `pat_patients` row **in your company**.
3. This is why the descriptive fields (`name`, `surname`, `phoneNumber`) are
   required here and absent from the read shape.

The company boundary always comes from the authenticated token, never from
request input. There is no parameter that lets a partner read another company's
data — including no `companyId` field to send.

### 8.2 Opaque values — passthrough

`reserve`'s `hash`, the `url` it returns, and `outherProcessId` are server-issued
values. The SDK passes them through verbatim: it never decodes, re-encodes,
shortens or follows them. The clinic/API encryption keys are never embedded in
the SDK.

---

## 9. Cross-cutting requirements (every SDK)

1. **Idiomatic, hand-written** — no codegen; match each ecosystem's conventions.
2. **Minimal dependencies** — prefer the platform HTTP client; pin the documented
   stack per language (see §7 / PLAN.md).
3. **Typed** — public API and `data` payloads typed where the language supports it.
4. **Fail fast on a missing token** — raise before dispatching (§5.2).
5. **Pluggable** token store and HTTP client.
6. **Errors** per §4 with full context.
7. **Tested** — unit tests for envelope/error/auth/config logic + at least one live
   smoke path against `test`.
8. **Examples** — `examples/` with the end-to-end partner flow: check doctor →
   slots → reserve → create, and a measures read/write example.
9. **Self-contained repo** — README, LICENSE (MIT), DESIGN.md copy, CI.
10. **Versioning** — semver; tag `vX.Y.Z` per repo.

---

## 10. Live validation reference (test env)

- Base: `https://apitest.bulutklinik.com/api/v3` (or `/v4`).
- Auth: a partner token issued for a test company with the `apiouther` scope.
  Unlike the patient surface there is no shared test credential in the Postman
  collection — the token is per-integration.
- Smoke path that needs no patient data: `doctors.branches` → `doctors.locations`
  → `laboratory.catalog`. All three are `GET`, scope-gated only, and prove the
  token and base URL are right.
- Patient-scoped reads need a patient that exists **in the token's company**;
  a TCKN that works on the patient surface will not necessarily resolve here.

---

## 11. Change control

This file is canonical. When it changes:
1. Bump the spec version (§ top).
2. Copy it into every language repo (`<repo>/DESIGN.md`).
3. Reconcile each SDK against the change; note breaking changes in repo CHANGELOGs.

If an SDK must diverge from this spec, fix the spec first (or record the
divergence here) — code and SSOT must never silently disagree.

---

## 12. Migration from 0.6.x

0.6.x shipped two personas: a patient surface at the client root and a partner
surface under `client.partner.*`. 1.0.0 keeps **only** the partner one and lifts
it to the root.

### 12.1 Mechanical rename

| 0.6.x | 1.0.0 |
|-------|-------|
| `client.partner.doctors.*` | `client.doctors.*` |
| `client.partner.slots.*` | `client.slots.*` |
| `client.partner.appointments.*` | `client.appointments.*` |
| `client.partner.measures.*` | `client.measures.*` |
| `client.partner.laboratory.*` | `client.laboratory.*` |
| `client.partner.diets.*` | `client.diets.*` |

Behaviour, paths and payloads of these 28 methods are unchanged.

### 12.2 Removed with no replacement

`auth` (all 11 methods) · `payments` (5) · `skin` · `meals` · `addresses` (4) ·
and the patient-persona `doctors`/`slots`/`appointments`/`measures`/`laboratory`/`diets`
that lived at the root in 0.6.x. §1.2 explains why each has no partner
equivalent. An application that needs a patient session must talk to the API
directly; the SDK no longer models it.

### 12.3 Configuration

| 0.6.x | 1.0.0 |
|-------|-------|
| `clientId`, `clientSecret` | removed |
| `partnerToken` (optional, for 2 endpoints) | **required** credential for the whole client |
| token store held `accessToken` + `refreshToken` | holds one partner token |
| silent refresh + retry on 401/`resultType 4` | removed — `AuthenticationError`, no retry (§5.3) |
| base URL fixed at `/api/v3` | `/api/v3` or `/api/v4` via `apiVersion` |
| escape hatch `auth` default `bearer` | default `partner`; `bearer` no longer exists |
