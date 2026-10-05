# SuPCIS-L8 Example

An example codebase written to understand enterprise C software architecture before working on embedded/automotive and warehouse management systems. Models the structure of a real WMS (Warehouse Management System) using Domain-Driven Design in pure C.

> This is a study project — not production code. The goal was to understand how large C codebases are structured: layered architecture, dependency inversion, repository pattern, unit testing with mocks, and CI/CD pipeline design.

## What This Demonstrates

| Concept | Where |
|---|---|
| Domain-Driven Design in C | `src/domain/` — entities with invariants, no DB dependency |
| Repository pattern | `src/domain/*/include/*_repository.h` — interfaces (function pointer structs) |
| Dependency inversion | Domain never includes infrastructure headers |
| Unit testing with cmocka | `tests/unit/` — mock repositories, no DB or network required |
| Static analysis | `make check` — cppcheck on entire `src/` |
| Layered architecture | domain → application → infrastructure (one-way dependency) |

## Architecture

```
src/
├── domain/              ← business rules — no DB, no HTTP, no OS calls
│   ├── inventory/       ← StockItem entity: reserve, deduct, release
│   ├── order/           ← Order entity: state machine (NEW→RELEASED→PICKING→SHIPPED)
│   ├── picking/         ← PickTask and PickWave entities
│   └── robot/           ← AutoStore port interface (stub implementation)
│
├── application/         ← orchestrates domain entities (use cases)
│   └── order_app_service.c
│
└── infrastructure/      ← DB, HTTP, config — depends on domain interfaces
    ├── api/             ← REST handlers (maps HTTP ↔ domain results)
    ├── database/        ← Oracle OCI repository implementations
    ├── configuration/   ← customer-specific config loader
    └── external_integration/  ← ERP adapter (SAP integration stub)
```

## Building

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt-get install -y gcc make libcurl4-openssl-dev

# Build the server binary
make

# Run unit tests (no database required)
make test

# Static analysis
make check
```

## Running Unit Tests

Tests use [cmocka](https://cmocka.org/) and mock all external dependencies — no Oracle database or AutoStore hardware needed.

```bash
sudo apt-get install -y libcmocka-dev
bash scripts/testing/run_unit_tests.sh
```

25 tests across 5 test suites:
- `test_inventory` — StockItem reserve/deduct/release logic
- `test_order` — Order state machine transitions
- `test_picking` — PickTask confirmation and PickWave closing
- `test_autostore` — Robot command struct and port stub
- `test_rest` — REST handler HTTP status code mapping

## Deep Dive

See [`docs/CODEBASE_GUIDE.md`](docs/CODEBASE_GUIDE.md) for a detailed walkthrough of every layer, design decision, and interview-prep notes.
