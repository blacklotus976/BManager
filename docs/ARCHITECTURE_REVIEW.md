# BManager — Architecture Review & Roadmap

_Review date: 2026-10-11 · Scope: `src/` (PropertyManager, ~10k LOC), schema in `SqliteConn.cpp`, demo data in `Seed.cpp`._

## 1. What the client actually needs

The client runs a **building-management / home-services business** (expresshomeservice.gr):
monthly building management fees, κοινόχρηστα issuing/collection, heating, burner service,
fire extinguishers, pest control, cleaning crews. Customers are residents/owners of apartments
in buildings; some services are per-apartment, some are shared by a whole building.

So "glorified Excel" really means: **a customer ledger** — who owes what, who paid what,
what the business paid out on someone's behalf — with search, reports, and a trustworthy history.
Future: customers log in and pay online, and bank transactions get matched automatically.

That makes it an **accounts-receivable / billing system**, not a spreadsheet. The single most
important property is: *money records can be trusted and disputes can be settled from history.*

## 2. What exists today (good parts)

- Clean domain split already: buildings → apartments → residents, with **residency history**
  and a per-payment apartment snapshot. This is better than most competitors at this size.
- Catalog services shared via `service_users` (joint services), packages via `master_services`.
- A `DataConn` interface separating UI from storage — the right seam for a later server backend.
- Parameterized queries for most user-entered data, paged list queries, daily auto-backup,
  CSV export, Greek UI, single static exe (zero install for the client).
- Audit log exists and is wired into business CRUD.

## 3. Problems found (verified on a freshly seeded DB)

| # | Issue | Evidence | Why it matters |
|---|---|---|---|
| 1 | ~~Money stored as `REAL` with float noise~~ **Fixed** — see §10 | `payments.amount_paid` contained `182.52499999999998` | Rounding drift in totals; banks/AADE need exact cents. |
| 2 | **Payments are mutable and deletable** (`UPDATE`/`DELETE payments`) | `editPayment`, `removePayment` | Exactly the dispute scenario the client fears: a recorded payment can silently change. |
| 3 | **Audit log is not trustworthy** | 16 of 407 entries are invalid JSON (strings concatenated without escaping); INSERT logs record `"id": -1`; DELETE logs store no old data; user/apartment deletes lose content | You can't reconstruct what was deleted, and the history can't be parsed reliably. |
| 4 | **Audit log can be tampered with** | Raw SQL console (`executeRawQuery`) can `DELETE FROM audit_log`; no hash chain | Anyone with the app can rewrite history. |
| 5 | **No "who" in the log** | `audit_log` has no user/actor column | Fine for one user, useless once there are staff or a web portal. |
| 6 | **Foreign keys are off** | `PRAGMA foreign_keys` = 0; code does cascades by hand | Orphan rows are possible; every new delete path must remember the manual cleanup. |
| 7 | **No transactions around multi-step writes** | `addUser` = insert user + insert residency + log, each separately | A crash mid-way leaves half-written data. |
| 8 | **Charge and receipt mixed into one row** (`amount_due`, `amount_paid`, `balance`, `status`) | `payments` table | Partial payments, one bank transfer covering 3 bills, refunds, credits — none fit cleanly. This is the main modelling issue. |
| 9 | **Dates as `year/month/day` ints**, `changed_at` in UTC but `created_at` in local time | schema | Awkward range queries; inconsistent timestamps in a dispute. |
| 10 | **Schema migrations are ad-hoc** `PRAGMA table_info` checks, `user_version` used once | constructor | Each release adds more fragile code to the constructor. |
| 11 | **`App.cpp` is 6.6k lines** of global state + drawing + business logic | `src/ui/App.cpp` | Business rules (balances, statuses) live in the UI and would have to be rewritten for the web. |
| 12 | No promotions/discounts/contracts model | — | Requested by the client. |
| 13 | No invoices/receipts (παραστατικά), no myDATA link | — | Greek legal requirement for the business's own invoicing (see §6). |
| 14 | Windows-only build/fonts/paths | fixed in this branch (`build_linux.sh`) | — |

## 4. How professional systems model this

Every serious billing/AR product (Stripe Billing, Chargebee, Buildium/AppFolio for property
management, Greek κοινόχρηστα software, any ERP) converges on the same shape:

1. **Immutable ledger.** Money rows are append-only. A mistake is fixed with a
   *reversal/credit note* plus a new correct entry — never by editing. The balance is
   `SUM(entries)`, never a stored column.
2. **Charges (invoices) and payments are separate documents**, joined by **allocations**
   (payment X pays €30 of charge A and €20 of charge B). This gives partial payments,
   overpayments/credit, and one-transfer-many-bills for free.
3. **Pricing layer:** catalog price → price lists → contracts/subscriptions per customer →
   discounts/promotions applied *at charge time* and frozen on the charge line.
4. **Audit = event log with actor, reason, before/after**, written in the same DB transaction,
   ideally hash-chained so tampering is detectable.
5. **Payment references**: every charge or customer gets a structured reference
   (ISO 11649 **RF code**) so incoming bank transfers can be matched automatically.

## 5. Target data model (SQLite now, PostgreSQL later — same schema)

```text
customers(id, full_name, phone, email, special_code, created_at)
buildings / apartments / residencies           -- keep as today
services(id, label, unit_price_cents, vat_rate, active)
packages / package_items                       -- keep as master_services
contracts(id, customer_id, apartment_id?, service_id, price_cents, start, end, billing_rule)
promotions(id, code, kind[percent|fixed|free_unit|bundle], value, valid_from, valid_to,
           applies_to_service_id?, applies_to_package_id?, max_uses, stackable)

charges(id, customer_id, apartment_id?, issue_date, due_date, status[open|void],
        reference_rf, created_by, created_at)
charge_lines(id, charge_id, service_id, qty, unit_price_cents, discount_cents,
             promotion_id?, vat_rate, total_cents)          -- price frozen here
payments(id, customer_id, date, amount_cents, method[cash|bank|card|iris],
         bank_txn_id?, reference, created_by, created_at)
allocations(payment_id, charge_id, amount_cents)
adjustments(id, customer_id, kind[credit_note|write_off|refund|reversal],
            amount_cents, reverses_id?, reason NOT NULL, created_by)
expenses / payouts(...)                         -- today's direction='out'

audit_log(id, at_utc, actor, action, entity, entity_id, before_json, after_json,
          reason, prev_hash, hash)
users_auth(id, username, password_hash, role)   -- empty for now, ready for the portal
```

Rules enforced **in the DB**, not the UI:
- `PRAGMA foreign_keys=ON`, `journal_mode=WAL`, every write in `BEGIN IMMEDIATE … COMMIT`.
- Triggers `BEFORE UPDATE/DELETE ON payments|allocations|charge_lines|audit_log → RAISE(ABORT)`
  (only `status='void'` on charges allowed, and only with a reason).
- Money as `INTEGER` cents. Dates as ISO `TEXT` (`YYYY-MM-DD`), timestamps UTC ISO-8601.
- Balance view: `customer_balance = Σcharges − Σallocated payments − Σcredits`.
- Migrations: numbered `migrations/NNN_*.sql` files applied by `PRAGMA user_version`.
- Raw SQL console: read-only (`sqlite3_stmt_readonly` / open a `?mode=ro` connection).

This answers the client's "fairness" requirement directly: nothing financial can be changed
silently; every correction is a visible, dated, reasoned entry the customer can be shown.

## 6. Greek-market specifics worth knowing

- **RF payment codes (ISO 11649):** generating an RF per charge lets customers pay from any Greek
  e-banking with zero typing errors, and lets you auto-match the statement line. Cheap, high value — do it now.
- **IRIS:** instant payments via phone/QR/ΑΦΜ; IRIS for professionals and online merchants exists and
  business payments have no monthly cap. A QR on each statement is a low-effort win.
- **Bank feeds:** pulling transactions needs a PSD2/AIS aggregator or the bank's business API
  (licensed third party; the app should consume a feed, not scrape). Plan for a
  `bank_transactions` import table + matching screen; CSV/MT940/CAMT.053 statement import works today without any license.
- **myDATA / e-invoicing:** AADE's B2B e-invoicing mandate has phased in during 2026 for smaller businesses
  (dates have shifted — confirm with the client's accountant). The app should not become an invoicing provider;
  keep `charges` exportable/linkable to the accountant's or a certified provider's system and store the MARK/UID they return.
- **GDPR:** names, phones, apartments = personal data. Encrypt backups, keep a retention policy, log who exported what.

## 7. Recommended architecture path (respecting "Excel first")

The client said no to a rewrite — and that's fine: **the language isn't the risk, the data model is.**
Keep C++/ImGui for the desktop, but move business rules out of the UI so they're reusable.

```text
 ImGui desktop ──► Domain layer (C++: Ledger, Billing, Promotions, Audit)
                          │  (pure functions + repository interface)
                          ▼
                 Repository (SQLite now)  ── same schema ──►  PostgreSQL (server)
                                                                   ▲
                                         Web backend (ASP.NET Core / any) + portal
                                         JWT, Argon2id hashes, payments, bank feed
```

**Phase 1 — make the desktop trustworthy (do now, ~2–4 weeks)**
1. Cents integers, ISO dates, FKs on, WAL, transactions, migration files.
2. Split `payments` into charges / payments / allocations / adjustments (migrate existing rows:
   each old row → one charge + one payment + one allocation).
3. Append-only triggers; corrections via reversal with mandatory reason.
4. Proper audit: real JSON (use nlohmann_json, already a dependency), actor, reason, hash chain;
   "Ιστορικό" button on every customer/charge showing its full history.
5. Read-only SQL console.
6. Customer statement (καρτέλα πελάτη) PDF/print with running balance and RF code.
7. Tests for the ledger math (Catch2/doctest) — the first tests in the repo.

**Phase 2 — commercial features**
Contracts & recurring charge generation (button "Έκδοση χρεώσεων μήνα", user-confirmed, not automatic),
promotions engine, aging report (0–30/31–60/60+), dunning reminders (SMS/Viber/email), CSV bank-statement import + match.

**Phase 3 — server & portal**
Move the same schema to PostgreSQL, put a backend in front of it (here .NET is a sensible choice —
the desktop stays a client of the API or keeps a sync). Customer portal: login (Argon2id, 2FA for staff),
view statement, pay by card/IRIS via a PSP (Viva Wallet, Stripe, Nexi, etc.), webhook → payment row → auto-allocation.
Never store card data; PCI scope stays with the PSP.

## 8. What I'd do differently, in one line each

- Model **documents and events**, not editable spreadsheet rows.
- Store **cents**, not floats; **ISO dates**, not y/m/d ints.
- Put integrity in the **database** (FKs, triggers, transactions), not in UI discipline.
- Make the audit log **complete, structured, attributed and tamper-evident**.
- Keep business logic in a **domain layer** so the web backend can reuse or mirror it 1:1.
- Add **RF codes** from day one — it's what makes future automatic reconciliation possible.
- Add a **test suite** around money math before any of the above refactors.

## 9. Building on Linux

```bash
sudo apt install cmake g++ libglfw3-dev libgl-dev
./build_linux.sh            # fetches Dear ImGui v1.91.9 into third_party/imgui on first run
./build-linux/PropertyManager
```
Windows build (`build_imgui.bat` + vcpkg) is unchanged.

## 10. Money handling (implemented)

- `src/Money.h`: `ToCents`/`FromCents`/`RoundMoney`/`MoneySub`/`FormatMoney`/`ParseMoney`.
- Every amount written by `addPayment`/`editPayment`/`addService*`/`editService` is snapped to whole
  cents, and `balance` is always re-derived as `due − paid` in integer cents (the DB layer owns this rule).
- SQL totals and joint-service nets are summed in integer cents, so "net = 0" is exactly 0.
- Migration v3 (`PRAGMA user_version = 3`): cleans existing rows once, in a transaction, logged to `audit_log`.
- UI: all money inputs accept `12,50`, `12.50`, `1.234,56`, `€ 40`; invalid/3-decimal input is shown in red
  and blocks Save (previously `atof("12,50")` silently stored **12.00**). Prefills show `35.00`, not `35.000000`.
- `tests/money_test.cpp` (run by `build_linux.sh` via ctest).
- Columns stay `REAL` euros so the raw view, CSV and SQL console read naturally; on PostgreSQL use `NUMERIC(12,2)`.

## 11. The client's business (expresshomeservice.gr)

EXPRESS HOME SERVICE Ε.Π.Ε., Νέα Ερυθραία. Offering (from the site/catalog seeded in `Seed.cpp`): building
management & κοινόχρηστα, heating oil, burner/boiler service, disinfection/pest control, drain unblocking,
fire extinguishers (buy / lend / service / refill), cleaning crews. **Introductory packages (πακέτα γνωριμίας):**
- **BASIC — from €15/month**: κοινόχρηστα issuing, 2 disinfection/pest-control visits, burner service, free public-liability insurance.
- **ALL IN ONE — from €40/month**: full management, expense transparency, fuel cost allocation, multiple payment methods, legal receipts.

What that implies for the admin panel:
1. **Packages are subscriptions with entitlements**, not just groupings: "€15/month includes 2 pest-control
   visits/year + 1 burner service". Needed: `contracts` (customer/building ↔ package, start/end, monthly price)
   and an **entitlement counter** (used 1 of 2 visits) so extra visits are billed and included ones are €0.
2. **"From €X"** = price depends on building size → per-contract price override, frozen on each charge.
3. **Promotions** like "free extinguisher lending inside a package", "1 in 4 fuel additive orders free",
   introductory price for N months → promotion rules applied at charge time and stored on the charge line.
4. **Customer is often the building (πολυκατοικία, own ΑΦΜ)**, not a person: the building pays the management
   fee, apartments owe κοινόχρηστα shares. Model "who is billed" as either a building or a resident.
5. **Expense transparency & legal receipts** are part of what he sells: every expense (`direction='out'`)
   should carry the supplier, document number and an attachment (scan/PDF), visible to residents later on the portal.
6. **Fuel allocation** (κατανομή πετρελαίου by χιλιοστά/hours) is a calculation the ALL IN ONE package promises —
   a future module: apartment `millesimal shares` per expense category.
7. **Scheduled service work** (annual extinguisher check, burner service, pest control twice a year) →
   a service-visit calendar with reminders; this is the "controller" side of the admin panel.
