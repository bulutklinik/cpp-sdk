# Changelog

All notable changes to the Bulutklinik C++ SDK are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0]

The SDK becomes **partner-only**. Everything that required a patient login is
gone; the company-scoped `/outher` surface that shipped under `client.partner()`
in 0.6.0 is now the client root. See `DESIGN.md` §12 for the full migration.

### Changed — BREAKING

- **`client.partner().<group>()` → `client.<group>()`.** The six partner groups
  (`doctors`, `slots`, `appointments`, `measures`, `laboratory`, `diets`) moved to
  the root. Their paths, bodies and behaviour are unchanged — this is a rename.
  Resource classes lost the `Partner` prefix (`PartnerDoctorsResource` →
  `DoctorsResource`); `PartnerNamespace` is gone.
- **`TokenStore` now holds one partner token**: `token()` / `set_token()` /
  `clear()` replace `access_token()` / `refresh_token()` / `set_tokens()`.
  `InMemoryTokenStore` takes the token as its single constructor argument.
- **`ClientOptions::partner_token` is now the client's credential** and is
  required for every call. Setting both `partner_token` and `token_store` throws
  `std::invalid_argument` from the constructor rather than silently picking one.
- **No silent refresh.** A `401` / `resultType 4` throws `AuthenticationError`
  with no retry — a partner token is issued out of band and cannot be renewed
  from here. Install a newly issued token in the token store instead.
- **A missing token fails before dispatch** with `AuthenticationError`, rather
  than sending an anonymous request that returns an opaque `401`.
- **`Auth` is now `{ Public, Partner }`**; `Auth::Bearer` is gone, and
  `RequestOptions::auth` defaults to `Auth::Partner`.
- `MeasuresResource::partner_health_information` →
  `MeasuresResource::health_information`, now `[[deprecated]]`.
- `DoctorsResource::search` takes `(search_params, current_page, order_params)`
  instead of a `SearchInput` — the `/outher` search has no `other_params` or
  `per_page_limit`, and `order_params` excludes `point`.

### Added

- **`ApiVersion` (`V3` / `V4`) and `ClientOptions::api_version`.** Every path is
  version-agnostic, so targeting v4 is configuration, not a code change. Default
  stays `V3`.

### Fixed

- The test suite now compiles. The 0.6.0 partner tests parsed
  `HttpRequest::body` (a `std::optional<std::string>`) directly, which never
  built; they use `.value()` now. 0.6.0 shipped without a compile check.

### Removed

- `client.auth()` (all 11 methods), `client.payments()` (5), `client.skin()`,
  `client.meals()`, `client.addresses()` (4) — no company-scoped equivalent exists.
- The patient-persona `doctors` / `slots` / `appointments` / `measures` /
  `laboratory` / `diets` that lived at the root in 0.6.0.
- `ClientOptions::client_id` / `::client_secret`.
- The `LoginResult`, `CardInfo`, `RegisterInput`, `VerifyRegistrationInput`,
  `ConfirmRegistrationEmailInput`, `VerifyRegistrationSocialInput`,
  `RegisterSocialInput`, `ForgotPasswordInput`, `ResetPasswordInput`,
  `AddressInput`, `AddressUpdateInput`, `SearchInput`, `PaymentInput`,
  `MealInput` and `LabOrderInput` types.

## [0.6.0]

### Added

- `client.auth().confirm_registration_email(input)` — the **required** e-mail-branch middle
  step of registration (`POST /patients/emailConfirmationRegister`). A headerless SDK
  caller always gets `confirmationType "email"` from `verify_registration`; confirm the
  e-mailed code here to receive the SMS blob that `register_patient` consumes (without it,
  `register_patient` returns 501).
- Social sign-up: `client.auth().verify_registration_social(input)` +
  `client.auth().register_social(input)` (both public; `register_social` does not
  auto-login — call `connect` with login_mode `social` after).
- Password reset: `client.auth().forgot_password(input)` + `client.auth().reset_password(input)`.
- `client.appointments().list(page?)` (`GET /patients/userAppointments`) — the source of the
  `event_id` that `cancel` requires — and `client.appointments().reservations()`.
- New `client.addresses()` group (`list`/`add`/`update`/`delete_address`) over
  `/patients/userAddress`, required by `laboratory().order()` (which needs an `addressId`).
  (`delete_address` avoids the C++ keyword `delete`.)
- Types: `ConfirmRegistrationEmailInput`, `VerifyRegistrationSocialInput`,
  `RegisterSocialInput`, `ForgotPasswordInput`, `ResetPasswordInput`, `AddressInput`,
  `AddressUpdateInput`.

## [0.5.0]

### Added

- `client.auth().verify_registration(input)` — step 1 of registration
  (`POST /patients/verifyAddingNewPatient`): sends the verification code and returns
  the raw `nlohmann::json` holding the encrypted `response` blob to pass to
  `register_patient`. Uses the configured partner token (`auth:apiusers`, not
  public) and requires a browser-minted CAPTCHA token (`recaptcha_v2` or `captcha`).
- Type: `VerifyRegistrationInput`.

## [0.4.0]

### Added

- `client.laboratory()` — the patient's lab results, the orderable test catalog,
  and test pre-ordering (DESIGN.md §6.9): `results(page?)`
  (`GET /patients/userLabTestList/{page?}`), `result_detail(test_id)`
  (`GET /patients/userLabTestDetail/{testId}`; `testId` is a string, e.g.
  `"4821-lab"`), `catalog()` (`GET /patients/allLaboratoryTests`),
  `catalog_detail(id)` (`GET /patients/laboratoryTestDetail/{id}`), and
  `order(input)` (`POST /patients/addNewLaboratoryTest`).
- `client.diets()` — the patient's diet lists (DESIGN.md §6.10): `list(page?)`
  (`GET /patients/dietLists/{page?}`) and `detail(list_id)`
  (`GET /patients/diet/{listId}`).
- `LabOrderInput` struct for the `laboratory().order` input (maps to the body
  `testId`, `addressId`, `laboratoryId`).

## [0.3.0]

### Added

- `client.skin().analyze(images)` — "Cildimde Neyim Var" AI skin-lesion analysis
  (`POST /patients/imageCheck`). Returns per-image lesion `label`, a Turkish AI
  `comment`, `confidence`, `possible_icd` and an opaque `case_detail` blob (which
  can be forwarded as a payment's `caseDetail`).
- `client.meals().analyze(input)` — AI meal-photo calorie/nutrition estimation
  (`POST /patients/imageAnalyzeMeal`).
- `MealInput` struct for the meals input (maps to the snake_case body
  `image`, `portion_size`, `portion_grams`, `meal_type`, `note`).

## [0.2.0]

### Added

- `client.request(...)` escape hatch for calling any endpoint not yet covered by a
  typed resource method (DESIGN.md §7.2).

## [0.1.0]

### Added

- Initial release: `auth`, `doctors`, `slots`, `appointments`, `payments`,
  `measures` service groups over a shared transport with silent token refresh.
