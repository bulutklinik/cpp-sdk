# Bulutklinik SDK — Canonical Design (SSOT)

> **This file is the single source of truth (SSOT) for every official Bulutklinik
> SDK.** All language packages (JavaScript/TypeScript, PHP, Python, Go, Java, C#,
> C++) are hand-written but MUST implement exactly the contract described here.
> The canonical copy lives at `dev-kits/DESIGN.md`; an identical copy is vendored
> into each language repository and re-synced whenever this file changes.
>
> Wire contract is derived from the BulutklinikAPI source (Laravel 8.12,
> OAuth2/Passport) — `app/Packages/Integration/Outher` and `routes/{v3,v4}/outher.php`.

- **Spec version:** 1.1.0 — restores the `auth` group. 1.0.x wrongly claimed the
  partner token could only be issued out of band; it is in fact obtained through
  the same `connectApi` password grant every other persona uses, and it is
  refreshable. See §5 and §12.1.
  1.0.0/1.0.1 made the SDKs single-persona: everything that required a *patient*
  login is gone, and every data method runs on the company-scoped `/outher`
  channel. That part is unchanged.
- **API:** BulutklinikAPI `v3` (default) or `v4` — selectable per client.
- **Scope:** 7 services / 31 endpoints (partner persona).

---

## 1. Scope

The SDKs expose the **partner** persona: a clinic-integration channel where the
caller is a company, authenticated by a pre-issued partner token, acting on the
patients of **its own company**.

| Service        | Endpoints | Purpose                                                     |
|----------------|:---------:|-------------------------------------------------------------|
| `auth`         | 3         | Obtain, refresh and revoke the partner access token         |
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
| Token lifecycle | minted by `auth.connect` from your client id/secret + service credentials, ~30 days, refreshable (§5) |

There is no patient login, no session, and no per-user access token. Two calls
for two different patients are indistinguishable to the transport — the patient
reference travels in the request body.

**Patient identity is carried in the body, never in the URL.** A TCKN in a path
segment would land in access logs, proxy logs and Sentry breadcrumbs. This is why
several read endpoints are `POST` although they are semantically reads.

### 1.2 Deliberately out of scope

Not exposed, because the API has no company-scoped equivalent:

- **Patient** authentication and registration — patient sign-up, social sign-up,
  password reset. (Partner authentication *is* covered — see §5.)
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
| `4`   | Refresh  | Access token expired. Triggers the silent refresh + single retry (§5.4). If no refresh token is held, or the refresh itself fails, raise `AuthenticationError`. |

> Implementation note: `/outher` returns `resultType 4` with HTTP `401` on an
> expired token. A bare HTTP `401` without a parseable envelope MUST be treated
> identically — both trigger the refresh path.

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
    ├── AuthenticationError       (401 after a failed/absent refresh, or resultType 2 — session revoked)
    ├── AuthorizationError        (403 — authenticated but the token lacks the scope, or carries no company)
    ├── NotFoundError             (404)
    └── RateLimitError            (429 — throttled; carries Retry-After if present)
```

Each `ApiError` carries: `httpStatus`, `resultType`, `errorType`, `errorMessage`,
the raw `data`, and the originating request (method + path) for debugging.
Mapping precedence: revoked session (`resultType == 2`) → string
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

### 5.1 Where the credentials come from

The Bulutklinik Developer Platform issues, per approved application, three things:

| Value | Used as |
|-------|---------|
| **Client ID** | `apiClientId` — the OAuth2 client |
| **Client Secret** | `apiSecretKey` — the OAuth2 client secret |
| **Service identity** | `apiUserName` — a project-specific login, *not* the developer's e-mail |

The password is the one set when registering on the portal. Nothing here is a
ready-made bearer token: the token is **minted by calling the API**.

> Spec 1.0.x got this wrong. It described the token as "issued out of band" with
> "no grant the SDK can drive", and therefore dropped `auth` entirely. The grant
> below has always existed and is what the portal's own quick-start shows.

### 5.2 Obtaining a token — `auth.connect`

`POST /general/connectApi`. **Public** (no `Authorization`), rate-limited.

| Field | Required | Notes |
|-------|:--------:|-------|
| `apiClientId` | ✓ | Client ID from the portal. |
| `apiSecretKey` | ✓ | Client Secret from the portal. |
| `apiUserName` | ✓ | The service identity. |
| `apiUserPassword` | ✓ | The portal account password. |
| `loginMode` | ✓ | `email` (what the portal documents) \| `identity` \| `phone` \| `user_id`. |

Success → `data: { access_token, refresh_token, password_policy }`. The SDK
persists both tokens (§5.3) and returns a login result.

Under the hood this is a Passport **password grant**; the granted scope is read
from the authenticated row's `client_scope`, which is what ties a partner
application to `apiouther`. A partner integration therefore never sends a scope —
it is a property of the credentials.

> **No CAPTCHA for partners.** The server only demands a CAPTCHA when the
> authenticating row is a patient, doctor or developer account. A partner
> (`user_group = api`) is exempt, which is what makes this callable from a
> headless SDK at all.

**Two-factor branch.** If the account has SMS 2FA enabled, `data` carries a
`response` blob and **no** `access_token`. SDKs surface this as a typed
*two-factor required* result rather than an error, so the caller can collect the
code and finish the login. Partner service identities do not normally have 2FA
on, but the branch exists and must not be mistaken for a malformed response.

### 5.3 Token store (pluggable)

Tokens are read through a `TokenStore` on **every** request, so a long-lived
process can rotate credentials without being rebuilt.

| Operation | Purpose |
|-----------|---------|
| get token | The current access token, or null/empty. |
| set token | Replace it (accepts null to unset). |
| clear | Drop everything. Called automatically on `resultType 2`. |

A store MAY additionally implement **refresh-token persistence** (`get refresh
token` / `set refresh token`). This is an *optional extension*, not part of the
base interface — a store written against spec 1.0.x keeps working unchanged. When
the injected store does not implement it, the SDK holds the refresh token in
memory for the client's lifetime; the only consequence is that a process restart
requires a fresh `connect` instead of a `refresh`. The built-in in-memory store
implements both.

The `partnerToken` option remains, for callers who already hold a token and do
not want the SDK to mint one:

```
new Client({ clientId, clientSecret })      ⇒ call auth.connect to obtain tokens
new Client({ partnerToken: "…" })           ⇒ in-memory store seeded with a token
new Client({ tokenStore: myVaultStore })    ⇒ tokens come from your store
new Client({ partnerToken: …, tokenStore: … })  ⇒ configuration error at construction
```

Passing both a literal and a store is rejected rather than silently resolved:
guessing which one the caller meant is how credential bugs get shipped.

If no token is available when a data request is dispatched, the SDK raises
`AuthenticationError` **before** touching the network.

### 5.4 Refresh — `auth.refresh`, and the silent retry

`POST /general/refreshApi`. **Public.** Body: `refreshToken`, `clientId`,
`clientSecretKey`. Success → `data: { access_token, refresh_token }` — both
rotate, so persist both.

On any partner-authenticated call:

1. Send it with the current access token.
2. If the response is `401` **or** `resultType == 4`, a refresh token is held, and
   this request has not already been retried:
   a. refresh, b. persist the new tokens, c. retry the original request **once**.
3. If the refresh fails, or `resultType == 2` comes back, clear the store and
   raise `AuthenticationError`.

The retry is bounded to one attempt. Refresh must be concurrency-safe:
simultaneous 401s share a single in-flight refresh rather than stampeding.

> A failed refresh answers `resultType 2` with HTTP 400 — which the envelope rules
> (§3.1) already map to "clear the store and raise". No special case needed.

### 5.5 Revoking — `auth.disconnect`

`POST /general/disconnectApi`. **Partner bearer.** Revokes the access token and
all of its refresh tokens, then the SDK clears the store.

> Send an **empty body**. The endpoint optionally accepts a device-token cleanup
> (`token` + `device`), but its `device` mapping has no default branch — an
> unexpected value raises server-side. There is no partner use for it.

### 5.6 Lifetime

Access tokens last ~30 days, refresh tokens ~130. Refreshing is the normal path;
a full `connect` is only needed on first use or after both have lapsed.

---

## 6. Endpoint reference (31)

Notation: **Canonical name** = language-neutral concept → per-language naming
follows §7. Every endpoint below requires the partner token except the two public
`auth` calls (§6.0); the scope column lists the OAuth scope the token must carry.

Two patient-reference shapes recur; both are defined in §8.1.

- **`patientRef`** — `{ identityNumber?, phoneNumber? }`, at least one. Used by
  **reads**. Never creates anything.
- **`bookingUser`** — `{ name, surname, phoneNumber, identityNumber?, email?, birthdate?, nationality?, price? }`.
  Used by **writes**. Creates the patient in your company if absent.

### 6.0 `auth`

Token lifecycle. Bodies and semantics in §5.

| Canonical    | Method | Path                        | Auth    |
|--------------|--------|-----------------------------|---------|
| `connect`    | POST   | `/general/connectApi`       | public  |
| `refresh`    | POST   | `/general/refreshApi`       | public  |
| `disconnect` | POST   | `/general/disconnectApi`    | partner |

`connect` and `refresh` are the only endpoints in this spec that are **not**
partner-authenticated — they are what produce the credential everything else
uses.

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
client.auth.connect(...)            client.measures.addList(...)
client.doctors.search(...)          client.laboratory.results(...)
client.slots.schedule(...)          client.diets.list(...)
client.appointments.reserve(...)
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
| `clientId` / `clientSecret` | — | OAuth client credentials from the portal. Required by `auth.connect` and `auth.refresh`. |
| `partnerToken`| —              | An already-minted access token. Seeds the default in-memory store. |
| `tokenStore`  | in-memory      | Pluggable token source (§5.3). Mutually exclusive with `partnerToken`. |
| `timeout`     | sane default   | Request timeout.                                   |
| `httpClient`  | platform default | Injectable transport (PSR-18, http.Client, HttpClient, etc.). |

`clientId` / `clientSecret` are back in 1.1.0: the partner token is minted by the
same password grant, so they are needed for `connect` and `refresh` (§5).

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
4. **Fail fast on a missing token** — raise before dispatching (§5.3), and
   auto-refresh + retry once per §5.4, concurrency-safe.
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
- Auth: an approved portal application for a test company whose credentials carry
  the `apiouther` scope. Credentials are per-integration; there is no shared test
  credential in the Postman collection.
- Smoke path that needs no patient data: `auth.connect` → `doctors.branches` →
  `doctors.locations` → `laboratory.catalog`. The three reads are `GET` and
  scope-gated only, so together with the login they prove the credentials, the
  granted scope and the base URL are all right.
- `auth.refresh` can be exercised directly rather than waiting ~30 days for a
  natural expiry.
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

## 12. Migration

### 12.0 From 1.0.x — the `auth` group is back

1.0.x removed `auth` on the mistaken premise that a partner token could only be
issued out of band. It cannot be minted by a *client-credentials* grant — there is
no `oauth/token` route — but it is minted by the **password** grant at
`connectApi`, using the client id/secret and service identity the portal hands
out. 1.1.0 restores `auth.connect` / `auth.refresh` / `auth.disconnect` and the
silent refresh + retry that goes with them.

This is **additive**. All 28 data methods keep their paths, bodies and
signatures. What changes:

| 1.0.x | 1.1.0 |
|-------|-------|
| `partnerToken` was the only way in | still supported; or call `auth.connect` with `clientId`/`clientSecret` |
| `resultType 4` → terminal `AuthenticationError` | → silent refresh + one retry; error only if that fails |
| `TokenStore` held one token | unchanged; refresh-token persistence is an **optional** extension a store may add (§5.3) |

A 1.0.x integration that seeds `partnerToken` and never hits an expiry keeps
working untouched.

### 12.1 From 0.6.x

0.6.x shipped two personas: a patient surface at the client root and a partner
surface under `client.partner.*`. 1.0.0 kept **only** the partner one and lifted
it to the root.

#### Mechanical rename

| 0.6.x | 1.0.0 |
|-------|-------|
| `client.partner.doctors.*` | `client.doctors.*` |
| `client.partner.slots.*` | `client.slots.*` |
| `client.partner.appointments.*` | `client.appointments.*` |
| `client.partner.measures.*` | `client.measures.*` |
| `client.partner.laboratory.*` | `client.laboratory.*` |
| `client.partner.diets.*` | `client.diets.*` |

Behaviour, paths and payloads of these 28 methods are unchanged.

#### Removed with no replacement

`auth` (all 11 methods) · `payments` (5) · `skin` · `meals` · `addresses` (4) ·
and the patient-persona `doctors`/`slots`/`appointments`/`measures`/`laboratory`/`diets`
that lived at the root in 0.6.x. §1.2 explains why each has no partner
equivalent. An application that needs a patient session must talk to the API
directly; the SDK no longer models it.

#### Configuration

| 0.6.x | 1.0.0 |
|-------|-------|
| `clientId`, `clientSecret` | removed in 1.0.x, **restored in 1.1.0** (§5) |
| `partnerToken` (optional, for 2 endpoints) | **required** credential for the whole client |
| token store held `accessToken` + `refreshToken` | holds the access token; refresh persistence is an optional store extension (§5.3) |
| silent refresh + retry on 401/`resultType 4` | removed in 1.0.x, **restored in 1.1.0** (§5.4) |
| base URL fixed at `/api/v3` | `/api/v3` or `/api/v4` via `apiVersion` |
| escape hatch `auth` default `bearer` | default `partner`; `bearer` no longer exists |
