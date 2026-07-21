# Changelog

All notable changes to the Bulutklinik C++ SDK are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
