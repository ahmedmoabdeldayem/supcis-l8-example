# SuPCIS-L8 — Complete Codebase Guide

This document explains everything about this example codebase:
what each file does, how the pieces connect, and why it was built this way.
Written specifically to help an embedded/automotive engineer understand
enterprise C software before walking into an interview at Element Logic.

---

## Table of Contents

1. [What SuPCIS-L8 Is](#1-what-supcis-l8-is)
2. [Architecture Overview](#2-architecture-overview)
3. [Folder Structure](#3-folder-structure)
4. [Layer-by-Layer Explanation](#4-layer-by-layer-explanation)
   - 4.1 Common
   - 4.2 Domain Layer
   - 4.3 Infrastructure Layer
   - 4.4 Application Layer
5. [Domain Modules](#5-domain-modules)
   - 5.1 Inventory
   - 5.2 Order
   - 5.3 Picking
   - 5.4 Robot / AutoStore
6. [Infrastructure Modules](#6-infrastructure-modules)
   - 6.1 Database (Oracle OCI)
   - 6.2 REST API handlers
   - 6.3 Configuration loader
   - 6.4 ERP adapter (SAP)
7. [Application Layer — Workflows](#7-application-layer--workflows)
8. [Database Design](#8-database-design)
9. [REST API Reference](#9-rest-api-reference)
10. [Customer Configuration](#10-customer-configuration)
11. [Testing Strategy](#11-testing-strategy)
12. [Scripts Reference](#12-scripts-reference)
13. [Build System](#13-build-system)
14. [CI/CD Pipeline](#14-cicd-pipeline)
15. [Key Concepts Glossary](#15-key-concepts-glossary)
16. [How to Read Unfamiliar Code](#16-how-to-read-unfamiliar-code)
17. [End-to-End API Data Flow](#17-end-to-end-api-data-flow)
18. [Oracle OCI — Real Implementation Deep Dive](#18-oracle-oci--real-implementation-deep-dive)
19. [AutoStore Robot Navigation — How It Really Works](#19-autostore-robot-navigation--how-it-really-works)

---

## 1. What SuPCIS-L8 Is

SuPCIS-L8 is a **Warehouse Management Software (WMS)** made by S&P Computersysteme GmbH,
which is part of Element Logic — the world's largest AutoStore partner.

**What it does:**
- Tracks every item stored in a warehouse (where it is, how many)
- Receives orders from customer ERP systems (SAP, etc.)
- Generates pick instructions for warehouse workers
- Coordinates AutoStore robots to bring bins to picking ports
- Manages the packing and shipping process
- Generates reports and syncs back to ERP

**Who uses it:**
Each customer (warehouse operator) gets a customized installation.
The software is deployed on-site or in a hybrid cloud setup.
S&P developers configure it per customer using YAML config files.

**Why it's written in C:**
The software predates modern languages and needs to run reliably on
long-lifecycle industrial hardware with RedHat Linux. C gives deterministic
performance and is familiar to systems engineers who maintain it.

---

## 2. Architecture Overview

The codebase follows **Domain-Driven Design (DDD)** organized into layers:

```
┌─────────────────────────────────────────────────────┐
│                  REST API (HTTP)                     │  ← External clients
│           src/infrastructure/api/                   │     (ERP, mobile apps)
├─────────────────────────────────────────────────────┤
│              Application Layer                       │  ← Orchestrates workflows
│           src/application/                          │
├─────────────────────────────────────────────────────┤
│               Domain Layer                           │  ← Business rules (THE CORE)
│    src/domain/{inventory,order,picking,robot}/       │
├─────────────────────────────────────────────────────┤
│            Infrastructure Layer                      │  ← Oracle DB, HTTP client,
│    src/infrastructure/{database,                    │     config loader, ERP adapter
│                         api,configuration,          │
│                         external_integration}/       │
└─────────────────────────────────────────────────────┘
```

**The rule: dependencies only point downward.**
- The REST API depends on the application layer
- The application layer depends on the domain
- The domain depends on NOTHING except common types
- Infrastructure implements domain interfaces (repositories)

This means you can test the domain with zero database, zero network,
zero HTTP server. That is the entire point of DDD layering.

---

## 3. Folder Structure

```
supcis-l8-example/
│
├── src/
│   ├── common/
│   │   ├── include/
│   │   │   ├── types.h          Shared primitive types (result codes, UUID, qty, timestamp)
│   │   │   └── logger.h         Structured logging macros (LOG_INFO, LOG_ERROR, etc.)
│   │   └── src/
│   │       └── logger.c         Logger implementation (file open, timestamp, level filter)
│   │
│   ├── domain/                  ← Business rules — no HTTP, no SQL, no YAML
│   │   ├── inventory/
│   │   │   ├── include/
│   │   │   │   ├── inventory_entity.h      StockItem struct + reserve/deduct/release
│   │   │   │   └── inventory_repository.h  Abstract interface (function pointers)
│   │   │   └── src/
│   │   │       └── inventory_entity.c      Business logic for stock quantity changes
│   │   │
│   │   ├── order/
│   │   │   ├── include/
│   │   │   │   └── order_entity.h          Order + OrderLine structs + state machine
│   │   │   └── src/
│   │   │       └── order_entity.c          release / cancel / is_fully_picked
│   │   │
│   │   ├── picking/
│   │   │   ├── include/
│   │   │   │   ├── picking_entity.h        PickTask + PickWave structs
│   │   │   │   └── picking_service.h       Domain service: creates waves from orders
│   │   │   └── src/
│   │   │       ├── picking_entity.c        confirm task / close wave logic
│   │   │       └── picking_service.c       Wave creation algorithm (reserve + build tasks)
│   │   │
│   │   └── robot/
│   │       ├── include/
│   │       │   └── autostore_port.h        Robot command + config structs
│   │       └── src/
│   │           └── autostore_port.c        HTTP adapter to AutoStore controller (libcurl)
│   │
│   ├── application/             ← Workflow orchestration (calls domain in sequence)
│   │   ├── include/
│   │   │   └── application_service.h       release_order / confirm_task / cancel_order
│   │   └── src/
│   │       └── order_app_service.c         Stubbed workflow steps with explanatory comments
│   │
│   ├── infrastructure/          ← "How" things are stored/sent (domain doesn't know)
│   │   ├── api/
│   │   │   ├── include/
│   │   │   │   └── rest_handler.h          http_request_t / http_response_t structs + dispatch
│   │   │   └── src/
│   │   │       └── inventory_rest_handlers.c  GET /api/v1/inventory handler
│   │   │
│   │   ├── database/
│   │   │   ├── include/
│   │   │   │   └── db_connection.h         Oracle OCI interface (connect/execute/query/commit)
│   │   │   └── src/
│   │   │       └── db_connection.c         OCI stub with full sequence explained in comments
│   │   │
│   │   ├── configuration/
│   │   │   ├── include/
│   │   │   │   └── customer_config.h       customer_config_t struct + config_load/validate
│   │   │   └── src/
│   │   │       └── customer_config.c       YAML loader stub + env var substitution
│   │   │
│   │   └── external_integration/
│   │       ├── include/
│   │       │   └── erp_adapter.h           SAP RFC interface (connect/notify/confirm)
│   │       └── src/
│   │           └── erp_adapter.c           SAP NW RFC SDK stub with call sequence explained
│   │
│   └── main.c                   Entry point: signals → config → logger → DB → event loop
│
├── database/
│   ├── migrations/
│   │   ├── V001__init_schema.sql        schema_version tracking table
│   │   ├── V002__inventory_tables.sql   warehouse_location + stock_item
│   │   └── V003__order_tables.sql       order_header + order_line
│   ├── stored_procedures/
│   │   └── sp_create_pick_wave.sql      PL/SQL: creates wave + reserves stock atomically
│   ├── views/
│   │   ├── v_inventory_on_hand.sql      Current available stock per bin
│   │   ├── v_open_orders.sql            All non-shipped orders with completion %
│   │   └── v_wave_progress.sql          Active waves with pending/confirmed counts
│   └── seed_data/
│       ├── warehouse_locations.sql      Generates a 3-aisle bin grid + ports + staging
│       └── default_configurations.sql  System config key/value defaults
│
├── config/
│   ├── default/
│   │   ├── application.conf     Server port, picking strategy, reservation expiry
│   │   ├── database.conf        Oracle TNS alias, pool size, timeouts
│   │   ├── logging.conf         Log rotation size, file count, timestamp format
│   │   └── api.conf             HTTP bind address, body size limit, auth header name
│   ├── customers/
│   │   └── customer_template/
│   │       ├── warehouse.yaml   Warehouse ID, AutoStore config, picking strategy
│   │       └── erp_mappings.yaml  SAP host, client, warehouse/order type mappings
│   └── environments/
│       ├── development.env      Local DB, debug log level, fake API keys
│       ├── staging.env          Staging server — values injected by CI pipeline
│       └── production.env       Template only — real values never stored in git
│
├── tests/
│   ├── unit/
│   │   ├── domain/
│   │   │   ├── inventory/
│   │   │   │   └── test_inventory_entity.c   5 tests: reserve/deduct/release/available
│   │   │   ├── order/
│   │   │   │   └── test_order_entity.c       7 tests: state machine transitions
│   │   │   ├── picking/
│   │   │   │   └── test_picking_entity.c     8 tests: confirm/short-pick/wave-close
│   │   │   └── robot/
│   │   │       └── test_autostore_port.c     3 tests: config, command fields, poll stub
│   │   └── infrastructure/
│   │       └── api/
│   │           └── test_rest_handler.c       2 tests: 404 mapping, 200 JSON fields
│   ├── integration/
│   │   └── test_order_to_picking_flow.c      Full order → wave creation with mock repo
│   ├── fixtures/
│   │   ├── test_helper.h        Shared factory function declarations
│   │   └── test_helper.c        test_make_stock_item / test_make_order factories
│   └── mocks/
│       └── mock_repository.h    Reusable mock inventory_repository_t with reset/add/get
│
├── scripts/
│   ├── build/
│   │   ├── compile.sh           Wraps make with debug/release mode selection
│   │   └── clean.sh             Deletes build/ directory
│   ├── database/
│   │   ├── migrate.sh           Applies pending V*.sql migrations via sqlplus
│   │   └── rollback.sh          Rolls back last migration using R*.sql scripts
│   ├── testing/
│   │   ├── run_unit_tests.sh    Compiles and runs unit tests (no DB needed)
│   │   ├── run_integration_tests.sh  Waits for Oracle, migrates, runs integration tests
│   │   └── valgrind_check.sh    Runs tests under Valgrind for memory leak detection
│   └── deployment/
│       ├── deploy.sh            rsync binary + config → remote, migrate, restart service
│       └── health_check.sh      Hits /api/v1/health and verifies HTTP 200
│
├── build/                       Generated by make — never committed to git
│   ├── bin/                     Final supcis-l8 binary goes here
│   ├── obj/                     Compiled .o object files (mirrors src/ structure)
│   └── reports/test-results/    JUnit XML test results (read by CI)
│
├── .gitlab-ci.yml               GitLab CI pipeline (build → test → analysis → deploy)
├── .jenkins/
│   └── Jenkinsfile              Jenkins pipeline (same stages, Groovy syntax)
├── docs/
│   └── CODEBASE_GUIDE.md        This file
└── Makefile                     Build system (make / make test / make check / make clean)
```

---

## 4. Layer-by-Layer Explanation

### 4.1 Common (`src/common/`)

Everything else in the codebase depends on this layer.

**`types.h`** — shared primitive types:

| Type | Underlying type | Purpose |
|---|---|---|
| `supcis_result_t` | enum (int) | Return code for every fallible function |
| `supcis_id_t` | char[37] | UUID string primary key |
| `quantity_t` | int32_t | Whole-number stock counts (never float) |
| `timestamp_t` | int64_t | Unix epoch seconds (int64 avoids Year 2038) |

**`logger.h` / `logger.c`** — structured logging:
- `logger_init(path, min_level)` — opens the log file at startup
- `LOG_INFO("module", "fmt", ...)` — writes `[timestamp] [INFO ] [module] message`
- Flushes immediately on WARN/ERROR so messages appear before a crash
- Falls back to stderr if the log file can't be opened (useful in development)

```c
LOG_INFO("inventory", "Reserved %d of SKU %s at %s", qty, sku_id, location);
LOG_ERROR("db",       "Oracle connection failed: %d", oci_error_code);
```

### 4.2 Domain Layer (`src/domain/`)

**The most important layer.** Contains ALL business rules.
Has NO knowledge of HTTP, Oracle, YAML, or any external system.

Each module follows the same pattern:
- **Entity** — a struct with identity + functions enforcing its rules
- **Repository interface** — function pointers (C's way of doing interfaces)
- **Service** — logic that spans multiple entities (where needed)

The domain layer can be compiled and tested entirely standalone.
No database, no network, no file system required.

### 4.3 Infrastructure Layer (`src/infrastructure/`)

Implements the "how" that the domain deliberately ignores:

| Sub-module | What it does |
|---|---|
| `database/` | Oracle OCI connection, execute SQL, map rows to structs |
| `api/` | HTTP server, parse REST requests, serialize JSON responses |
| `configuration/` | Parse YAML files, substitute env vars, validate config |
| `external_integration/` | Speak to SAP via RFC, AutoStore via HTTP |

Infrastructure modules know about the domain (they implement repository interfaces)
but the domain knows nothing about infrastructure.

### 4.4 Application Layer (`src/application/`)

Sits between the REST API and the domain.
Contains no business rules — only workflow steps.

**What it does:**
1. Accept a request (order ID, task ID, etc.)
2. Load entities via their repositories
3. Call domain functions in the correct sequence
4. Persist results via repositories
5. Trigger external notifications (ERP, AutoStore)
6. Return a result to the REST handler

**What it does NOT do:**
- Invent new rules (those belong in the domain)
- Format JSON (that belongs in the REST handler)
- Write SQL (that belongs in the repository implementation)

---

## 5. Domain Modules

### 5.1 Inventory

**Files:** `inventory_entity.h/.c`, `inventory_repository.h`

**Core concept:** One `stock_item_t` = one SKU at one bin location.

```
stock_item_t:
  stock_id          — UUID primary key
  sku_id            — product identifier ("SKU-BEARING-001")
  location_code     — bin address ("BIN-A-01-01")
  quantity_on_hand  — total physically in the bin
  quantity_reserved — locked for open pick tasks (still physically present)
```

**The three quantity operations:**

| Function | When called | What changes |
|---|---|---|
| `stock_item_reserve(item, qty)` | Pick task created | `reserved += qty` |
| `stock_item_release(item, qty)` | Pick task cancelled | `reserved -= qty` |
| `stock_item_deduct(item, qty)` | Picker confirms pick | `on_hand -= qty`, `reserved -= qty` |

**Why separate reserve from deduct?**
Reserve is a soft lock — the item is still physically in the bin.
Deduct is the final removal after physical picking confirms it left the bin.
If a task is cancelled before picking, `release()` undoes the lock cleanly
without losing track of the units.

**`inventory_repository.h` — the repository interface:**
Defined as a struct of function pointers (C's equivalent of an interface/abstract class).
The `self` pointer is C's equivalent of `this` in C++/Java.
Two implementations exist: Oracle (production) and mock array (tests).

### 5.2 Order

**Files:** `order_entity.h/.c`

**State machine:**
```
NEW ──── order_release() ──→ RELEASED ──→ PICKING ──→ PACKING ──→ SHIPPED
 │                               │            │           │
 └───────── order_cancel() ──────┴────────────┴───────────┘
                                          CANCELLED
                                  (blocked from SHIPPED)
```

**Rules enforced:**
- `order_release()` — only transitions from NEW (prevents double-release and duplicate waves)
- `order_cancel()` — blocked when SHIPPED (can't cancel what already left the building)
- `order_is_fully_picked()` — returns true only when every line has `qty_picked == qty_ordered`

**PICKING → PACKING → SHIPPED transitions** are set by the application layer
when external events arrive (wave closed, packing complete, shipment scanned).

### 5.3 Picking

**Files:** `picking_entity.h/.c`, `picking_service.h/.c`

**How wave picking works end-to-end:**
```
Released orders
    ↓
picking_service_create_wave()     [domain service]
    ↓ finds stock, reserves it, builds pick_task_t per line
pick_wave_t with N tasks
    ↓ persisted to Oracle, sent to picker scanners
Picker scans each task
    ↓
pick_task_confirm(task, qty)      [domain entity function]
    ↓
All tasks CONFIRMED or CANCELLED
    ↓
pick_wave_close(wave)             [domain entity function]
    ↓
Application layer advances order → PACKING
```

**Task lifecycle:**
```
PENDING → ASSIGNED → CONFIRMED
                   → CANCELLED
```

**Short picks:** if `qty_ordered=10` but only 7 available, `qty_to_pick=7`.
The wave still proceeds with 7. The missing 3 are handled by backorder logic
in the application layer (not in the domain — that would be scope creep).

**`picking_service_create_wave()` algorithm:**
1. Validate order is RELEASED
2. For each order line: call `repo->find_by_sku_location()` with `NULL` location (any bin)
3. Calculate `to_pick = min(qty_ordered, qty_available)`
4. Call `stock_item_reserve()` then `repo->save()` to persist reservation
5. Build a `pick_task_t` pointing to that bin
6. Return `SUPCIS_ERR_NOT_FOUND` if zero tasks were created (all stock out)

### 5.4 Robot / AutoStore

**Files:** `autostore_port.h/.c`

**How AutoStore works physically:**
- Robots drive on rails on top of a dense grid of stacked storage bins
- When a bin is needed, a robot digs down through the stack, grabs it, brings it to a port
- A human picker takes items from the bin at the port, then the robot returns it

**Integration pattern — Command + Poll:**
```
SuPCIS sends:  POST /api/v1/commands  {"type":1, "binId":"BIN-42", "targetPort":"PORT-01"}
               ↓
AutoStore controller dispatches robot
               ↓
SuPCIS polls:  GET /api/v1/commands/{id}/status
               ↓  (repeat until complete)
               {"status": "COMPLETE"}
```

**Library:** `libcurl` handles HTTP, TLS, timeouts, headers.
The JSON payload is built with `snprintf` (small fixed-structure, no library needed).

---

## 6. Infrastructure Modules

### 6.1 Database (Oracle OCI)

**Files:** `db_connection.h`, `db_connection.c`

Oracle Call Interface (OCI) is Oracle's native C library.
The `db_handle_t` struct is opaque — callers never see OCI internals.

**Real OCI call sequence (shown in comments in db_connection.c):**
```
db_connect()   → OCIEnvCreate → OCIHandleAlloc → OCILogon2
db_begin()     → transaction starts implicitly with first DML
db_execute()   → OCIStmtPrepare2 → OCIBindByPos → OCIStmtExecute
db_query_one() → OCIStmtPrepare2 → OCIDefineByPos → OCIStmtExecute → OCIStmtFetch2
db_commit()    → OCITransCommit
db_rollback()  → OCITransRollback
db_disconnect()→ OCILogoff → OCIHandleFree (err) → OCIHandleFree (env)
```

**Oracle vs PostgreSQL/MySQL key differences:**
- No explicit `BEGIN` statement — transactions start with the first DML
- `VARCHAR2(n)` instead of `VARCHAR(n)`
- `SYS_GUID()` to generate UUIDs
- `PL/SQL` for stored procedures (not T-SQL or pgSQL)
- TNS aliases for connection strings (defined in `tnsnames.ora` on the server)

### 6.2 REST API Handlers

**Files:** `rest_handler.h`, `inventory_rest_handlers.c`

Translates between the HTTP world and the domain world:

```
HTTP request (path, body, headers)
    ↓  parse query params / JSON body
Domain call (repository, entity function)
    ↓  map result codes to HTTP status codes
JSON response (status code + body)
```

**HTTP status code mapping:**

| Domain result | HTTP status | Reason |
|---|---|---|
| `SUPCIS_OK` + data | 200 | Success |
| `SUPCIS_ERR_NOT_FOUND` | 404 | Normal — resource doesn't exist |
| `SUPCIS_ERR_CONFLICT` | 409 | Business rule blocked the operation |
| `SUPCIS_ERR_INVALID_ARG` | 400 | Bad request from client |
| `SUPCIS_ERR_DB` | 500 | Internal failure — don't expose details |

404 is returned as `SUPCIS_OK` from the handler because the HTTP transaction
itself succeeded — it successfully determined the resource doesn't exist.

### 6.3 Configuration Loader

**Files:** `customer_config.h`, `customer_config.c`

Loads `config/customers/<name>/warehouse.yaml` and `erp_mappings.yaml`
into a `customer_config_t` struct at startup.

**Key mechanism — env var substitution:**
```yaml
api_key: "${AUTOSTORE_API_KEY}"   # in YAML file
```
```c
/* In customer_config.c */
const char *value = getenv("AUTOSTORE_API_KEY");  /* reads from environment */
strncpy(out, value, out_size - 1);
```
Secrets are never stored in YAML files. The deployment system injects them
as environment variables. If a required env var is missing, the server refuses
to start and logs exactly which variable is unset.

**Validation:** `config_validate()` checks required fields, value ranges,
and cross-field rules (e.g. if AutoStore is enabled, controller_url must be set).

### 6.4 ERP Adapter (SAP)

**Files:** `erp_adapter.h`, `erp_adapter.c`

Connects to SAP using the **NW RFC SDK** (NetWeaver Remote Function Call SDK),
Oracle's C library for calling SAP function modules.

**Real RFC call sequence (shown in comments in erp_adapter.c):**
```
erp_adapter_connect()                   → RfcOpenConnection()
erp_adapter_notify_picking_started()    → RfcGetFunctionDesc("ZSUPCIS_NOTIFY_PICKING")
                                          → RfcCreateFunction → RfcSetChars → RfcInvoke
erp_adapter_confirm_shipment()          → RfcInvoke("ZSUPCIS_GOODS_ISSUE")
erp_adapter_disconnect()                → RfcCloseConnection()
```

**What "ZSUPCIS_" means:**
SAP custom function modules are named with a `Z` prefix by convention.
These are written by the SAP developers at the customer site and expose
the specific actions SuPCIS needs (status updates, goods issue postings).

---

## 7. Application Layer — Workflows

**Files:** `application_service.h`, `order_app_service.c`

The application layer is stubbed in this example. The stubs show the
complete step-by-step workflow with comments explaining each step.

### Workflow: Release an Order (`app_release_order`)

```
Input: order_id (string)

1. Load order from Oracle via order_repository_find_by_id()
2. Call order_release() — domain enforces NEW → RELEASED
3. Call picking_service_create_wave() — find stock, reserve it, build tasks
4. db_begin() — start Oracle transaction
5. picking_repository_save(wave) — persist all pick tasks
6. order_repository_save(order) — persist RELEASED status
7. db_commit() — make everything visible atomically
8. erp_adapter_notify_picking_started() — tell SAP status changed

Output: populated pick_wave_t
```

### Workflow: Confirm a Pick Task (`app_confirm_pick_task`)

```
Input: task_id, qty_picked

1. Load pick_task from Oracle
2. pick_task_confirm(task, qty_picked) — domain validates state + qty
3. Load stock_item for the task's SKU + location
4. stock_item_deduct(stock, qty_picked) — physically remove from inventory
5. Save both task and stock_item in one transaction
6. Load the parent pick_wave
7. If pick_wave_pending_count(wave) == 0 → pick_wave_close(wave)
8. Load the parent order
9. If order_is_fully_picked(order) → advance order to PACKING status

Output: SUPCIS_OK or error code
```

### Workflow: Cancel an Order (`app_cancel_order`)

```
Input: order_id

1. Load all open pick_tasks for this order
2. For each task: stock_item_release(stock, task.qty_to_pick) — undo reservations
3. Cancel each task (status → CANCELLED)
4. call order_cancel(order)
5. Persist all changes in one transaction
6. Notify ERP that order was cancelled

Output: SUPCIS_OK or error code
```

---

## 8. Database Design

Oracle is the database. All schema changes go through versioned migrations.

### Migrations (`database/migrations/`)

Files named `V001__description.sql`, `V002__...` applied in version order.
Never modified after deployment — if a change is needed, a new version is added.
The `schema_version` table records which versions have been applied.

**Applied by:** `scripts/database/migrate.sh` — reads `schema_version`,
skips already-applied versions, applies new ones in order.

**Key tables created:**

| Table | Migration | Contents |
|---|---|---|
| `schema_version` | V001 | Tracks applied migrations |
| `warehouse_location` | V002 | All physical bins, ports, staging areas |
| `stock_item` | V002 | Current inventory per SKU per location |
| `order_header` | V003 | One row per customer order |
| `order_line` | V003 | Items within each order |
| `pick_wave` | (future V004) | Batches of pick tasks |
| `pick_task` | (future V004) | Individual picker instructions |

**Oracle-specific features used:**
- `VARCHAR2(36)` — UUID string columns
- `NUMBER(10)` — integer quantities (no DECIMAL — whole units only)
- `TIMESTAMP` — datetime with timezone awareness
- `CONSTRAINT CHECK` — validates enum-like string columns at DB level
- `FOREIGN KEY` — enforces relational integrity between tables
- `INDEX` — on `sku_id` and `location_code` for fast inventory lookups
- `CONSTRAINT UNIQUE` — prevents duplicate (sku, location) combinations

### Stored Procedures (`database/stored_procedures/`)

Complex multi-table operations run atomically inside Oracle.

**`sp_create_pick_wave.sql`** — creates a wave from RELEASED orders:
1. `SYS_GUID()` generates the wave UUID
2. A cursor JOIN across `order_line`, `order_header`, `stock_item` finds pickable lines
3. `INSERT` pick tasks and `UPDATE` stock reservations in one PL/SQL block
4. `COMMIT` makes the wave visible atomically — no partial waves

**Why PL/SQL instead of C application code?**
- Avoids multiple round-trips: one database call instead of N queries
- Atomicity is guaranteed by the database engine, not by C logic
- Runs closer to the data — faster for set-based operations

### Views (`database/views/`)

Pre-computed queries stored in Oracle, readable like tables.

| View | Purpose |
|---|---|
| `v_inventory_on_hand` | SKU + location + available qty (JOIN stock_item + warehouse_location) |
| `v_open_orders` | All non-shipped orders with line counts and completion percentage |
| `v_wave_progress` | Active waves with pending/confirmed/cancelled task counts |

**Why views instead of SELECT in C?**
The REST handler calls `SELECT * FROM v_open_orders WHERE ...` — simple.
The complex JOIN and CASE logic lives in the view definition.
If the schema changes, only the view is updated, not the C code.

### Seed Data (`database/seed_data/`)

Run once after migrations on a new installation.

**`warehouse_locations.sql`** — generates a sample warehouse:
- 3 aisles (A, B, C) × 4 levels × 5 positions = 60 bins
- 2 AutoStore ports (PORT-01, PORT-02)
- 1 staging area (STAGE-01)

**`default_configurations.sql`** — inserts system-wide defaults into a
`system_config` key-value table (wave size, reservation expiry, etc.).

---

## 9. REST API Reference

The HTTP server listens on port 8080 (configurable in `config/default/api.conf`).
All requests require `X-API-Key: <key>` header.
All responses are JSON.

### Inventory

| Method | Path | Description |
|---|---|---|
| GET | `/api/v1/inventory?sku=X&location=Y` | Get stock at a specific bin |
| POST | `/api/v1/inventory/adjust` | Correct stock count (physical count adjustment) |

**GET inventory — 200 response:**
```json
{
  "sku": "SKU-BEARING-001",
  "location": "BIN-A-01-01",
  "on_hand": 100,
  "reserved": 30,
  "available": 70
}
```

### Orders

| Method | Path | Description |
|---|---|---|
| POST | `/api/v1/orders` | Create order (called by ERP/SAP integration) |
| GET | `/api/v1/orders/:id` | Get order status and lines |
| POST | `/api/v1/orders/:id/release` | Release to warehouse floor |
| POST | `/api/v1/orders/:id/cancel` | Cancel order (releases all stock reservations) |

### Picking

| Method | Path | Description |
|---|---|---|
| POST | `/api/v1/picking/waves` | Create pick wave from released orders |
| GET | `/api/v1/picking/waves/:id` | Get wave progress (tasks pending/confirmed) |
| POST | `/api/v1/picking/tasks/:id/confirm` | Picker confirms a task completion |

### Robots

| Method | Path | Description |
|---|---|---|
| POST | `/api/v1/robots/commands` | Send fetch/return command to AutoStore controller |
| GET | `/api/v1/robots/commands/:id/status` | Poll whether robot completed a command |

### HTTP Status Codes

| Code | Meaning | When used |
|---|---|---|
| 200 | OK | Successful GET or action |
| 201 | Created | Successful POST that created a resource |
| 400 | Bad Request | Missing or malformed request parameters |
| 401 | Unauthorized | Missing or invalid `X-API-Key` header |
| 404 | Not Found | Resource doesn't exist — normal, not an error |
| 409 | Conflict | Business rule blocked the operation |
| 500 | Internal Error | Unexpected failure — real error is in the log file |

---

## 10. Customer Configuration

### Config File Hierarchy

```
config/default/          ← base values all installations share
    application.conf
    database.conf
    logging.conf
    api.conf
        ↓ overridden by
config/customers/<name>/ ← customer-specific values
    warehouse.yaml
    erp_mappings.yaml
        ↓ secrets substituted from
config/environments/     ← environment variables (never contains real secrets)
    development.env
    staging.env
    production.env
```

**`config/default/application.conf`** — shared defaults:
```ini
[server]
http_port = 8080

[picking]
max_tasks_per_wave    = 100
short_pick_allowed    = true

[inventory]
reservation_expiry_sec = 3600
```

**`config/customers/customer_template/warehouse.yaml`** — per-site layout:
```yaml
warehouse_id: "WH001"
autostore:
  enabled: true
  controller_url: "http://192.168.1.100:8080"
  api_key: "${AUTOSTORE_API_KEY}"       # substituted from env var at startup
  command_timeout_ms: 5000
```

**`config/environments/development.env`** — loaded by developers locally:
```bash
export DB_TNS=localhost/XEPDB1
export DB_USER=<replace-me>
export DB_PASS=<replace-me>
export LOG_LEVEL=DEBUG
```

**Secret handling rule:** Passwords and API keys are NEVER in YAML/conf files.
They use `${VAR_NAME}` placeholders. `customer_config.c::substitute_env_var()`
calls `getenv()` at startup. Missing required env vars cause startup failure
with a clear error message naming which variable is unset.

---

## 11. Testing Strategy

### Unit Tests (`tests/unit/`)

- **Framework:** cmocka (C Mock Object library)
- **Speed:** fast — no database, no network, no file I/O
- **Naming:** `test_<what>_<expected_outcome>`
- **Coverage:** every domain function has tests for success and each failure case

**What cmocka provides:**
```c
assert_int_equal(a, b)          // fails if a != b
assert_string_equal(a, b)       // string comparison
assert_true(expr)               // fails if expr is false
assert_false(expr)              // fails if expr is true
assert_non_null(ptr)            // fails if ptr is NULL
will_return(func, value)        // mock: next call to func returns value
expect_value(func, param, val)  // mock: assert that func is called with val
```

**Test files and what they cover:**

| File | Tests |
|---|---|
| `test_inventory_entity.c` | reserve (success, insufficient stock, zero qty), deduct, available |
| `test_order_entity.c` | release (from NEW, from RELEASED, from PICKING), cancel (from NEW, from SHIPPED), is_fully_picked |
| `test_picking_entity.c` | confirm (success, short pick, wrong state, bad qty), wave close (all confirmed, with pending, with mix) |
| `test_autostore_port.c` | config struct fields, command struct fields, poll stub returns false |
| `test_rest_handler.c` | 404 when not found, 200 with correct JSON fields |

### Shared Test Infrastructure

**`tests/fixtures/test_helper.h/.c`** — factory functions:
```c
stock_item_t item  = test_make_stock_item("SKU-001", "BIN-A-01", 100, 30);
order_t      order = test_make_order(ORDER_STATUS_NEW, 3, 10);
```
Using factories avoids duplicating struct setup in every test.
If `stock_item_t` gains a new required field, you fix one factory, not 20 tests.

**`tests/mocks/mock_repository.h`** — reusable mock `inventory_repository_t`:
```c
mock_repo_reset();               // clear state between tests
mock_repo_add_item(&stock_item); // seed stock items
picking_service_create_wave(&order, &mock_inventory_repo, &wave);
assert_int_equal(mock_repo_save_count(), 2); // verify 2 items were saved
```

### Integration Tests (`tests/integration/`)

- Require Oracle XE (runs as a container in CI, local in development)
- Test complete flows: order → wave creation → task confirmation
- `test_order_to_picking_flow.c` currently uses the mock repository
  so it runs without Oracle, but tests the full domain interaction

### TDD Cycle

In this codebase, tests are written alongside implementation:
1. Write a failing test describing expected behavior (Red)
2. Write minimum C code to make it pass (Green)
3. Refactor without breaking the test (Refactor)

---

## 12. Scripts Reference

All scripts are in `scripts/` and are executable (`chmod +x`).

### Build Scripts (`scripts/build/`)

| Script | Usage | What it does |
|---|---|---|
| `compile.sh [debug\|release]` | `scripts/build/compile.sh debug` | Wraps `make` with CFLAGS for debug (`-g -O0`) or release (`-O2`) |
| `clean.sh` | `scripts/build/clean.sh` | Runs `make clean` to delete `build/` |

### Database Scripts (`scripts/database/`)

| Script | Usage | What it does |
|---|---|---|
| `migrate.sh [env]` | `scripts/database/migrate.sh dev` | Applies pending V*.sql files via `sqlplus`, records in `schema_version` |
| `rollback.sh [env]` | `scripts/database/rollback.sh dev` | Runs the R*.sql rollback for the last applied migration |

`migrate.sh` logic:
1. Load environment file (gets DB credentials)
2. For each `V*.sql` in sorted order: check if version is in `schema_version`
3. If not: run the SQL file, then INSERT the version into `schema_version`

### Testing Scripts (`scripts/testing/`)

| Script | Usage | What it does |
|---|---|---|
| `run_unit_tests.sh` | `scripts/testing/run_unit_tests.sh` | Compiles test files with domain sources + `-lcmocka`, runs binary |
| `run_integration_tests.sh` | `scripts/testing/run_integration_tests.sh` | Waits for Oracle readiness (20 retries × 5s), migrates, runs tests |
| `valgrind_check.sh` | `scripts/testing/valgrind_check.sh` | Runs unit test binary under Valgrind with `--leak-check=full` |

### Deployment Scripts (`scripts/deployment/`)

| Script | Usage | What it does |
|---|---|---|
| `deploy.sh [env]` | `scripts/deployment/deploy.sh staging` | rsync binary + config → remote server, run migrations, restart service |
| `health_check.sh <host> <key>` | `scripts/deployment/health_check.sh prod.host 8080` | Hits `/api/v1/health`, verifies HTTP 200 |

`deploy.sh` steps:
1. Check binary exists (`build/bin/supcis-l8`) — fail early if not
2. Load environment file (gets SSH host, deploy user)
3. `rsync` binary to `/opt/supcis-l8/bin/`
4. `rsync` config files (excluding `.env` secret files)
5. SSH → run `migrate.sh` on the remote server
6. SSH → `systemctl restart supcis-l8`
7. Wait 3s, verify service is running with `systemctl is-active`

---

## 13. Build System

**`Makefile`** at the project root.

```bash
make          # compile everything → build/bin/supcis-l8
make test     # run unit tests
make check    # run cppcheck static analysis
make clean    # delete build/ directory
```

**How the build works:**
1. `find src -name '*.c'` discovers all source files recursively
2. Each `.c` compiles to a `.o` in `build/obj/` (same path structure as `src/`)
3. All `.o` files link into `build/bin/supcis-l8`
4. Linked libraries: `-loci` (Oracle OCI), `-lcurl` (HTTP), `-ljansson` (JSON), `-lpthread`, `-lm`

**Compiler flags:**
```makefile
CFLAGS = -Wall -Wextra -std=c99 -fPIC
```

| Flag | Effect |
|---|---|
| `-Wall -Wextra` | Enable all warnings — treated as errors in CI |
| `-std=c99` | C99 standard — enables `stdbool.h`, `stdint.h`, `//` comments |
| `-fPIC` | Position-independent code — needed if building as a shared library |

**Debug vs release:**
```bash
scripts/build/compile.sh debug    # adds -g -O0 -DDEBUG
scripts/build/compile.sh release  # uses -O2
```

**Include paths:** Each module's `include/` is passed with `-I` so you write:
```c
#include "inventory_entity.h"   // not "../../../domain/inventory/include/..."
```

---

## 14. CI/CD Pipeline

### GitLab CI (`.gitlab-ci.yml`)

Runs automatically on every push. Four sequential stages.

```
push to git
    ↓
[1] build        → make clean && make -j$(nproc)
    ↓
[2] test         → unit_tests (every push, fast)
                 → integration_tests (MR and main only, needs Oracle XE container)
    ↓
[3] analysis     → cppcheck (static analysis — finds null deref, overflows, leaks)
                 → valgrind  (runtime memory check — allow_failure: true)
    ↓
[4] deploy       → staging: automatic on develop branch
                 → production: manual approval required on main branch
```

**Oracle XE in CI:**
Integration tests need Oracle. GitLab CI spins up `gvenzl/oracle-xe:21-slim`
as a service container alongside the test job. The test script waits for it
to be ready before running.

**Artifacts:** The compiled binary is saved after the build stage so test
and deploy stages can use it without recompiling.

### Jenkins (`.jenkins/Jenkinsfile`)

Alternative pipeline for customers who run Jenkins instead of GitLab CI.
Same stages, written in Groovy (Jenkins' pipeline language).

**Key differences from GitLab CI:**

| | GitLab CI | Jenkins |
|---|---|---|
| Syntax | YAML | Groovy (Jenkinsfile) |
| Jobs run in | Docker containers (per job) | Agents (build servers you configure) |
| Secrets | GitLab CI Variables (encrypted) | Jenkins Credentials |
| Approval gate | `when: manual` | `input { message "Deploy?" }` |
| Parallel stages | `parallel:` block | `parallel { stage(...) ... }` |

**Jenkins-specific features used:**
- `credentials('name')` — injects secrets from Jenkins Credential Store
- `archiveArtifacts` — saves binary for downstream stages
- `junit` — publishes test results to Jenkins UI
- `input` — pauses pipeline and waits for human approval before production deploy
- `cleanWs()` — cleans the workspace after each build (saves disk space)

---

## 15. Key Concepts Glossary

| Term | What it means in this codebase |
|---|---|
| **SKU** | Stock Keeping Unit — a product identifier (e.g. "SKU-BEARING-001") |
| **Bin** | Physical storage location in the warehouse, addressed as "BIN-A-01-01" |
| **AutoStore** | Robotic grid: robots on rails fetch bins from a dense vertical stack |
| **Port** | Where a robot delivers a bin for a human picker |
| **Wave** | A batch of pick tasks grouped together for efficiency |
| **Short pick** | Picker found fewer items than requested — wave still proceeds with what's available |
| **DDD Entity** | An object with a stable identity (stock_id, order_id) that persists over time |
| **DDD Repository** | An abstraction hiding how data is stored — implemented as function pointers in C |
| **DDD Domain Service** | Business logic that spans multiple entities (picking_service) |
| **OCI** | Oracle Call Interface — the C library for connecting to Oracle databases |
| **PL/SQL** | Oracle's procedural SQL language for stored procedures and triggers |
| **TNS** | Oracle network alias (defined in `tnsnames.ora`) — like DNS for databases |
| **Migration** | A versioned, never-modified SQL script that evolves the database schema |
| **RFC** | Remote Function Call — SAP's protocol for calling function modules from C |
| **IDoc** | SAP document format for transferring business data (orders, shipments) |
| **`supcis_result_t`** | Return code type: 0=OK, non-zero=specific error |
| **`quantity_reserved`** | Stock soft-locked for open pick tasks (still physically in the bin) |
| **`quantity_on_hand`** | Total stock physically present in the bin |
| **Seed data** | Default data inserted into a fresh database after migrations |
| **`schema_version`** | Oracle table recording which migrations have been applied |

---

## 16. How to Read Unfamiliar Code

When you first open a module you've never seen, follow this sequence:

**Step 1 — Read the header file first**
The `.h` file is the public contract. It tells you what the module does
without showing how. Every function has a comment explaining its purpose,
parameters, and return values.
Start with `include/*.h` before touching `src/*.c`.

**Step 2 — Find the entry point**
- For a domain module: look for the service function (`picking_service_create_wave`)
- For the server: start at `main.c` and follow the startup sequence
- For tests: start at `main()` in the test file — the test names tell you everything

**Step 3 — Follow the data**
Ask: "where does data come in, where does it go out?"
In this codebase the path is always:
```
HTTP request → REST handler → application service → domain function
    → repository → Oracle → back up the same stack → JSON response
```

**Step 4 — Use result codes as a map**
Every function returns `supcis_result_t`. Grep for `if (rc != SUPCIS_OK)` to
find all error handling paths. This tells you what can go wrong in each step.

**Step 5 — Read the tests before the implementation**
Unit tests in `tests/unit/` are the best documentation for domain functions.
Each test is a named scenario: `test_reserve_fails_when_insufficient_stock`
tells you more than reading the function body.

**Step 6 — Use the logs as a trace**
All significant operations have `LOG_INFO` or `LOG_DEBUG` entries with the module name.
```bash
grep "\[picking\]"   supcis.log   # see everything the picking module did
grep "\[autostore\]" supcis.log   # see all robot commands and responses
grep "Order ORD-001" supcis.log   # trace one specific order end-to-end
```

**Step 7 — Check the scripts to understand deployment**
`scripts/deployment/deploy.sh` shows exactly what happens when a new version
goes to a customer site. Reading it answers: where does the binary go, how
are DB migrations applied, how is the service restarted.

---

## 17. End-to-End API Data Flow

This section traces the complete path of two real API calls from the moment an
HTTP packet arrives to the moment Oracle writes to disk and the JSON response
goes back to the caller.

### Flow A — GET /api/v1/inventory (read-only query)

**Who calls this:** An ERP system (SAP) or a warehouse operator's browser
dashboard asking "how much SKU-BEARING-001 do we have in bin BIN-A-01-01?"

```
HTTP Client (SAP / browser)
    │
    │  GET /api/v1/inventory?sku=SKU-BEARING-001&location=BIN-A-01-01
    │  Header: X-API-Key: prod-api-key-abc
    │
    ▼
[1] HTTP Listener (port 8080)
    │  libmicrohttpd accepts the TCP connection.
    │  Validates the X-API-Key header against the configured value.
    │  Calls rest_handler_dispatch(request) to route to the correct handler.
    │
    ▼
[2] rest_handler_dispatch()   [src/infrastructure/api/src/rest_handler.c]
    │  Matches path prefix "/api/v1/inventory" and HTTP method "GET".
    │  Calls handle_get_inventory(request, response, inventory_repo).
    │
    ▼
[3] handle_get_inventory()    [src/infrastructure/api/src/inventory_rest_handlers.c]
    │  Parses query string with sscanf:
    │    sku      = "SKU-BEARING-001"
    │    location = "BIN-A-01-01"
    │  Calls repo->find_by_sku_location(repo, sku, location, &item)
    │
    ▼
[4] oracle_inventory_repository.find_by_sku_location()
    │  [would be in src/infrastructure/database/src/stock_item_repository.c]
    │  Calls db_query_one(db, sql, &item, sizeof(item))
    │
    │  SQL executed:
    │    SELECT stock_id, sku_id, location_code, qty_on_hand, qty_reserved
    │    FROM   stock_item
    │    WHERE  sku_id = :1 AND location_code = :2
    │  Bind :1 = "SKU-BEARING-001"
    │  Bind :2 = "BIN-A-01-01"
    │
    ▼
[5] Oracle Database
    │  Looks up the index on (sku_id, location_code).
    │  Reads the matching row from the stock_item table.
    │  Returns: qty_on_hand=100, qty_reserved=30
    │
    ▼ (result travels back up the stack)
[4] Repository maps OCI column buffers → stock_item_t struct fields
    │  item.quantity_on_hand  = 100
    │  item.quantity_reserved = 30
    │  Returns SUPCIS_OK
    │
    ▼
[3] handle_get_inventory()
    │  Calls stock_item_available(&item) → 100 - 30 = 70
    │  Calls snprintf() to build JSON response body:
    │    {"sku":"SKU-BEARING-001","location":"BIN-A-01-01",
    │     "on_hand":100,"reserved":30,"available":70}
    │  Sets response.status_code = 200
    │
    ▼
[2] rest_handler_dispatch() writes HTTP response headers + JSON body
    │
    ▼
[1] libmicrohttpd sends TCP response back to the client

Total path: 6 hops, 1 Oracle SELECT, 0 writes.
```

**What happens if the SKU doesn't exist in that bin?**
- Oracle returns 0 rows → `OCIStmtFetch2` returns `OCI_NO_DATA`
- Repository returns `SUPCIS_ERR_NOT_FOUND`
- Handler maps it to HTTP 404 (not 500 — this is expected, not a crash)
- Response: `{"error":"not_found","sku":"SKU-BEARING-001","location":"BIN-A-01-01"}`

---

### Flow B — POST /api/v1/orders/:id/release (multi-step write workflow)

**Who calls this:** The SAP integration (ERP adapter) after a sales order
is confirmed, telling the warehouse to start picking.

```
SAP ERP system
    │
    │  POST /api/v1/orders/ORD-001/release
    │  Header: X-API-Key: prod-api-key-abc
    │  Body: (empty — the order ID is in the URL path)
    │
    ▼
[1] HTTP Listener + rest_handler_dispatch()
    │  Routes to handle_release_order(request, response, app_service)
    │
    ▼
[2] handle_release_order()    [src/infrastructure/api/src/order_rest_handlers.c]
    │  Extracts order_id = "ORD-001" from URL path.
    │  Calls app_release_order("ORD-001", &wave)
    │
    ▼
[3] app_release_order()       [src/application/src/order_app_service.c]
    │  This is the application layer — it ORCHESTRATES but invents no rules.
    │
    │  Step 3a: Load the order from Oracle
    │    order_repository_find_by_id(repo, "ORD-001", &order)
    │    SQL: SELECT order_id, status, ... FROM order_header WHERE order_id = :1
    │    Result: order.status = ORDER_STATUS_NEW
    │
    │  Step 3b: Call domain rule — transitions NEW → RELEASED
    │    rc = order_release(&order)          [src/domain/order/src/order_entity.c]
    │    Domain checks: is state == NEW?  YES → sets state = RELEASED, returns OK
    │    If state was already RELEASED: returns SUPCIS_ERR_CONFLICT (no duplicate waves)
    │
    │  Step 3c: Create pick wave — find stock, reserve it, build pick tasks
    │    rc = picking_service_create_wave(&order, inventory_repo, &wave)
    │                                   [src/domain/picking/src/picking_service.c]
    │    For each order line (e.g. 3 lines):
    │      inventory_repo->find_by_sku_location(repo, sku, NULL, &stock)
    │      SQL: SELECT ... FROM stock_item WHERE sku_id = :1 AND qty_on_hand > 0
    │      stock_item_reserve(&stock, to_pick)  — domain: reserved += to_pick
    │      inventory_repo->save(repo, &stock)
    │      SQL: UPDATE stock_item SET qty_reserved = :1 WHERE stock_id = :2
    │      Build pick_task_t pointing to that bin and quantity
    │    Returns wave with 3 pick tasks
    │
    │  Step 3d: Persist everything atomically
    │    db_begin(db)
    │    │
    │    │  SQL: INSERT INTO pick_wave (wave_id, order_id, created_at) VALUES (...)
    │    │  SQL: INSERT INTO pick_task (...) VALUES (...)   ← repeated for each task
    │    │  SQL: UPDATE order_header SET status = 'RELEASED' WHERE order_id = :1
    │    │
    │    db_commit(db)   ← all three writes become visible to other sessions at once
    │                       if any INSERT/UPDATE fails → db_rollback() undoes all
    │
    │  Step 3e: Notify SAP that picking has started (fire-and-forget)
    │    erp_adapter_notify_picking_started(erp, "ORD-001")
    │    SAP RFC call: RfcInvoke("ZSUPCIS_NOTIFY_PICKING", order_id="ORD-001")
    │    This is async — we log a warning if it fails but don't abort the workflow
    │
    ▼
[2] handle_release_order() serializes the result to JSON:
    │  {
    │    "order_id": "ORD-001",
    │    "status": "RELEASED",
    │    "wave_id": "A3F2...",
    │    "task_count": 3
    │  }
    │  HTTP 200 (or 409 Conflict if order was already released)
    │
    ▼
[1] Response sent back to SAP

Total path: 8 logical steps, 1 domain state check, N stock reservations,
            1 Oracle transaction (N+2 SQL statements), 1 SAP RFC call.
```

**Why is the Oracle commit a single transaction?**
If the wave is persisted but the order status update fails, the pickers would
start working on an order still marked NEW in the ERP — invoices and stock counts
would be wrong. The transaction ensures either everything changes or nothing does.

---

### Flow C — POST /api/v1/picking/tasks/:id/confirm (picker scans a barcode)

**Who calls this:** A handheld scanner app running on the picker's device, after
the picker physically takes items from a bin.

```
Picker scanner (mobile app / handheld device)
    │
    │  POST /api/v1/picking/tasks/TASK-001/confirm
    │  Body: {"qty_picked": 7}
    │
    ▼
app_confirm_pick_task("TASK-001", qty_picked=7)
    │
    ├─ Load pick_task_t from Oracle
    │    task.status         = TASK_ASSIGNED
    │    task.qty_to_pick    = 10
    │    task.sku_id         = "SKU-BEARING-001"
    │    task.location_code  = "BIN-A-01-01"
    │
    ├─ pick_task_confirm(&task, 7)   [domain function]
    │    Checks: state == ASSIGNED?  YES
    │    Checks: qty_picked (7) <= qty_to_pick (10)?  YES — short pick allowed
    │    Sets task.qty_picked = 7
    │    Sets task.status = TASK_CONFIRMED
    │
    ├─ Load stock_item for SKU-BEARING-001 at BIN-A-01-01
    │    stock.qty_on_hand  = 100
    │    stock.qty_reserved = 30
    │
    ├─ stock_item_deduct(&stock, 7)   [domain function]
    │    stock.qty_on_hand  = 100 - 7 = 93   (items physically left the bin)
    │    stock.qty_reserved = 30  - 7 = 23   (reservation consumed)
    │
    ├─ db_begin → save task + save stock_item → db_commit
    │
    ├─ Load parent pick_wave
    │    pick_wave_pending_count(&wave) — counts tasks still PENDING or ASSIGNED
    │    Result: 0 tasks left → call pick_wave_close(&wave)
    │
    └─ Load parent order
         order_is_fully_picked(&order)?
           YES → order.status = ORDER_STATUS_PACKING
                  save order + notify SAP
           NO  → another wave still has open tasks

Response: HTTP 200 {"task_id":"TASK-001","status":"CONFIRMED","qty_picked":7}
```

---

## 18. Oracle OCI — Real Implementation Deep Dive

**Reference file:** `src/infrastructure/database/src/db_connection_oci_real.c`

This file contains the full production Oracle OCI implementation with every
OCI call documented. It cannot be compiled on Mac (requires Oracle Instant Client
headers). It is there purely for study.

### The five OCI handle types you need to know

```
OCIEnv     — the environment
             Created once at process startup with OCIEnvCreate().
             Controls memory management, threading mode, character encoding.
             Everything else depends on this handle.

OCIError   — the error inspector
             One per thread. When any OCI call returns OCI_ERROR,
             call OCIErrorGet(err, ...) to get the Oracle error number
             and the human-readable message (e.g. "ORA-00942: table not found").
             Without this, OCI just gives you a -1 return code with no details.

OCISvcCtx  — the connection (service context)
             Represents one open session to one Oracle database.
             Created by OCILogon2(). Passed to every SQL operation.
             A connection pool means multiple OCISvcCtx handles sharing one server.

OCIStmt    — a prepared SQL statement
             Like a FILE* for SQL. Created by OCIStmtPrepare2(), executed by
             OCIStmtExecute(), released by OCIStmtRelease().
             Each SQL string is compiled into a plan once, then executed N times
             with different bind values — more efficient than re-parsing every time.

OCIBind    — an input parameter binding
OCIDefine  — an output column definition
             OCIBind maps a C variable → a :1 placeholder in the SQL (for input).
             OCIDefine maps a SELECT column → a C variable (for output).
```

### Why you need both OCIBind and OCIDefine

```
SQL: SELECT qty_on_hand FROM stock_item WHERE sku_id = :1

    :1 is an INPUT  → use OCIBindByPos  (you give Oracle a C variable to read)
    qty_on_hand is an OUTPUT → use OCIDefineByPos (you give Oracle a C buffer to write into)

    After OCIStmtFetch2() returns, the C buffer you passed to OCIDefineByPos
    contains the column value from the fetched row.
```

### What the NULL indicator does

Every `OCIDefineByPos` call accepts an `indp` pointer (an `sb2 *`).
After a fetch, Oracle writes into it:
- `0`  — the column has a value
- `-1` — the column is `NULL` in the database

Without checking the indicator, reading the C buffer for a NULL column gives
garbage data. For critical fields like quantities, always check `ind != -1`
before using the value.

```c
sb2 ind = 0;
int qty = 0;
OCIDefineByPos(stmt, &def, err, 1, &qty, sizeof(qty), SQLT_INT, &ind, ...);
OCIStmtFetch2(stmt, err, 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);

if (ind == -1) {
    qty = 0;   /* NULL in DB means zero stock */
}
```

### The full OCI lifecycle in one diagram

```
process start
    │
    OCIEnvCreate(&env, OCI_THREADED, ...)
    OCIHandleAlloc(env, &err, OCI_HTYPE_ERROR, ...)
    OCILogon2(env, err, &svc, user, pass, tns, OCI_DEFAULT)
    │
    │  per SQL operation:
    │
    │  OCIStmtPrepare2(svc, &stmt, err, sql, ...)
    │  OCIBindByPos(stmt, &bind, err, 1, value, ...)    ← once per :N input
    │  OCIDefineByPos(stmt, &def, err, 1, buffer, ...)  ← once per SELECT column
    │  OCIStmtExecute(svc, stmt, err, iters, ...)
    │  OCIStmtFetch2(stmt, err, 1, OCI_FETCH_NEXT, ...) ← for SELECT only
    │  OCIStmtRelease(stmt, err, ...)
    │
    │  for writes:
    │  OCITransCommit(svc, err, OCI_DEFAULT)     ← make changes permanent
    │  OR
    │  OCITransRollback(svc, err, OCI_DEFAULT)   ← undo on error
    │
process shutdown
    OCILogoff(svc, err)
    OCIHandleFree(err, OCI_HTYPE_ERROR)
    OCIHandleFree(env, OCI_HTYPE_ENV)
```

### Oracle-specific things that surprise PostgreSQL/MySQL developers

| PostgreSQL / MySQL | Oracle |
|---|---|
| `BEGIN` starts a transaction | No `BEGIN` — any DML starts a transaction implicitly |
| `SERIAL` or `AUTO_INCREMENT` | `SEQUENCE` + trigger, or `GENERATED ALWAYS AS IDENTITY` |
| `VARCHAR(n)` | `VARCHAR2(n)` — subtle differences in NULL handling |
| `gen_random_uuid()` | `SYS_GUID()` — returns RAW(16), often cast to VARCHAR2 |
| `LIMIT n` | `FETCH FIRST n ROWS ONLY` or `ROWNUM <= n` (older syntax) |
| `BOOLEAN` column type | No native BOOLEAN in Oracle tables — use `NUMBER(1)` or `CHAR(1)` |
| `TEXT` column | `CLOB` for large text, `VARCHAR2(4000)` for normal strings |
| `NOW()` | `SYSDATE` (no timezone) or `SYSTIMESTAMP` (with timezone) |

---

## 19. AutoStore Robot Navigation — How It Really Works

### The key insight: SuPCIS does NOT navigate robots

SuPCIS-L8 does not know robot positions, drive paths, or grid topology.
It only tells the AutoStore controller **what** it wants:
"Please bring bin BIN-42 to port PORT-01."

The AutoStore controller (a separate server, usually by AutoStore AS of Norway)
handles **how** that happens: which robot to use, the optimal path, avoiding
collisions, digging through stacked bins.

This is a deliberate boundary. If SuPCIS had to understand grid topology it
would need to be reconfigured every time the grid is expanded or a robot is
added. The controller owns that knowledge.

### How AutoStore robots know their position

```
┌────────────────────────────────────────────┐
│          AutoStore Grid (top view)         │
│                                            │
│   col: 1   2   3   4   5   6   7   8       │
│   row: ┌───┬───┬───┬───┬───┬───┬───┬───┐  │
│     1  │   │   │   │   │   │   │   │   │  │
│        ├───┼───┼───┼───┼───┼───┼───┼───┤  │
│     2  │   │ R │   │   │   │ R │   │   │  │  R = robot
│        ├───┼───┼───┼───┼───┼───┼───┼───┤  │
│     3  │   │   │   │   │   │   │   │   │  │
│        └───┴───┴───┴───┴───┴───┴───┴───┘  │
│                                            │
│   PORT-01  PORT-02   (openings at the edge)│
└────────────────────────────────────────────┘
```

Robots navigate using two mechanisms:

**1. Grid markers (QR codes or RFID tags)**
Every grid cell has a marker on the rail surface. Each robot has a downward-facing
camera or RFID reader. When the robot crosses a marker it knows exactly which
(column, row) cell it is in. Grid position is absolute — no GPS, no dead reckoning.

**2. Wheel encoders**
Between markers, robots use encoder pulses to track movement in millimeters.
This gives sub-cell precision (where in cell (3,2) is the robot right now?).
The controller fuses both: marker = coarse position, encoder = fine correction.

The controller maintains a real-time map of all robot positions. When it assigns
a robot to fetch a bin, it calculates the shortest collision-free path and sends
movement commands to that robot's on-board CPU via the internal robot network
(not the same network SuPCIS uses).

### How the controller knows where every bin is

AutoStore bins are stacked vertically in columns (like a deep container).
The controller tracks the 3D position of every bin in a database:

```
Bin ID    Column  Row  Stack depth (1=top, higher=deeper)
BIN-042    3       2       1   ← top of the stack, fastest to retrieve
BIN-087    3       2       2
BIN-133    3       2       3   ← deepest, robot must dig through BIN-042 and BIN-087
```

When a pick task needs BIN-133, the controller must:
1. Remove BIN-042 from column (3,2) and temporarily place it elsewhere
2. Remove BIN-087 and do the same
3. Pick up BIN-133 and bring it to the port
4. Return BIN-087 and BIN-042 (in an optimized order based on future demand)

SuPCIS never sees this complexity — it just polls "is the bin at the port yet?"

### What SuPCIS stores about bins vs. what the AutoStore controller stores

| What SuPCIS knows | What the AutoStore controller knows |
|---|---|
| `location_code = "BIN-A-01-01"` (human-readable name) | `bin_id = "BIN-042"` (internal grid ID) |
| `sku_id` and `quantity` stored in that bin | 3D grid coordinates (column, row, depth) |
| Whether a bin is reserved for a pick task | Current robot positions |
| Which port a wave should deliver to | Collision-avoidance paths |
| Nothing about physical grid layout | Bin stacking order per column |

The `warehouse_location` table in Oracle has a `bin_id` column that maps
SuPCIS's `location_code` to the AutoStore controller's internal ID.
When SuPCIS commands a fetch, it looks up the `bin_id` for the location
and sends it to the controller.

```c
/* Simplified — what happens before autostore_send_command() is called */
warehouse_location_t loc;
db_query_one(db,
    "SELECT bin_id, port_assignment "
    "FROM warehouse_location "
    "WHERE location_code = :1",
    &loc, sizeof(loc));

robot_command_t cmd;
strncpy(cmd.bin_id,      loc.bin_id,         sizeof(cmd.bin_id));
strncpy(cmd.target_port, loc.port_assignment, sizeof(cmd.target_port));
cmd.type = ROBOT_CMD_FETCH_BIN;

autostore_send_command(&cfg, &cmd);
```

### What are staging areas and why do they exist?

```
AutoStore Grid
    ↓
  PORT-01  PORT-02   ← robots deliver bins here
    ↓         ↓
  Picker A  Picker B  ← humans take items from bins
    ↓         ↓
  STAGE-01  STAGE-02  ← flat tables / conveyor zones
    ↓
  Packing desk
    ↓
  Shipping dock
```

Staging areas solve the **consolidation problem**:

One customer order (e.g. order ORD-001) might contain 10 different SKUs
stored in 10 different bins. Those bins arrive at the port one at a time —
the robot can only bring one bin at a time. The picker takes the required
quantity from each bin as it arrives.

The items from each bin need to be held somewhere while waiting for the
other 9 bins to be processed. That "somewhere" is the staging area.

**Example:**
- ORD-001 needs: 5× SKU-A, 3× SKU-B, 7× SKU-C
- Robot brings BIN-10 (has SKU-A) → picker takes 5 units → puts them on STAGE-01
- Robot brings BIN-47 (has SKU-B) → picker takes 3 units → puts them on STAGE-01
- Robot brings BIN-22 (has SKU-C) → picker takes 7 units → puts them on STAGE-01
- Now STAGE-01 has all items for ORD-001 → packer comes, packs the box, ships it

**What SuPCIS tracks about staging:**
- `warehouse_location` table includes rows with `location_type = 'STAGING'`
- A staging assignment links an order to a staging location: "ORD-001 is building at STAGE-01"
- When `order_is_fully_picked()` returns true, the app layer marks the staging location
  as ready for packing and notifies the packing station

**Multiple orders sharing ports:**
In high-throughput warehouses, waves are designed so multiple orders are picked
in parallel. PORT-01 might serve 4 orders at once — each bin that arrives is
routed to the correct staging table based on the pick task's order assignment.
SuPCIS knows the routing; the controller just delivers the bin.

### Full picking workflow with AutoStore involvement

```
1. app_release_order("ORD-001")
   │
   ├─ picking_service_create_wave() builds 3 tasks:
   │    TASK-1: fetch BIN-10 to PORT-01, pick 5× SKU-A
   │    TASK-2: fetch BIN-47 to PORT-01, pick 3× SKU-B
   │    TASK-3: fetch BIN-22 to PORT-01, pick 7× SKU-C
   │
   └─ (tasks saved to Oracle, wave saved, order → RELEASED)

2. Picking station app shows TASK-1 on the picker's screen
   │
   └─ autostore_send_command(FETCH_BIN, bin_id="BIN-10", port="PORT-01")
        Controller assigns robot R3 to fetch BIN-10
        Robot R3 navigates to column(3,2), digs to depth 1, lifts BIN-10
        Robot R3 drives to PORT-01 opening, lowers BIN-10 into the port chute
        Controller marks command COMPLETE

3. Picker sees BIN-10 arrive at PORT-01 port screen
   Picker takes 5× SKU-A, places on STAGE-01
   Scanner app calls POST /api/v1/picking/tasks/TASK-1/confirm  {qty_picked: 5}
   │
   └─ app_confirm_pick_task() deducts stock, marks TASK-1 CONFIRMED

4. autostore_send_command(RETURN_BIN, bin_id="BIN-10")
   Robot returns BIN-10 to the grid (possibly to a different depth
   based on frequency-of-access optimization in the controller)

5. Steps 2-4 repeat for TASK-2 and TASK-3

6. pick_wave_pending_count(wave) == 0
   pick_wave_close(wave)
   order_is_fully_picked(order) → true
   order.status → PACKING
   Packer is notified: "STAGE-01 is ready, pack ORD-001"
```

### What happens if a robot fails mid-task?

The AutoStore controller handles robot failures entirely. If robot R3 breaks
down while carrying BIN-10, the controller:
1. Detects the fault via heartbeat timeout or error signal
2. Dispatches another robot to retrieve BIN-10 from R3's last known position
3. Continues the command — from SuPCIS's perspective, the poll eventually returns COMPLETE

SuPCIS only needs to handle the case where `autostore_poll_status()` never
returns COMPLETE within its timeout window — in that case `autostore_port.c`
returns `SUPCIS_ERR_EXTERNAL` and the application layer cancels the task
and creates a manual exception for the warehouse supervisor to resolve.
