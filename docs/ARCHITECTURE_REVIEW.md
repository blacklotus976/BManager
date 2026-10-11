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

## 11. The client's business (expresshomeservice.gr, crawled 2026-10-11)

**EXPRESS HOME SERVICE Ε.Π.Ε.**, Νέα Ερυθραία (north Attica), founded 1997; the site claims an active
portfolio of **70,000 buildings** in greater Athens. Phones 210 625 1962-6, 210 625 0548, 210 807 6087,
210 620 7058 · info@expresshomeservice.gr. Pest-control licence no. 1281142/14.

### Price list ("από" = starting price, varies per building)
| Service | Price | Notes |
|---|---|---|
| Κοινόχρηστα (issuing) | free with a package | delivered within 24–48h; per-building history of oil level, heating meters, boiler, sub-meters, ΔΕΗ, septic |
| Διαχείριση πολυκατοικίας | from €40/month | pays ΔΕΗ/water/gas, crews (cleaning, lift, gardener), heating oil; collects κοινόχρηστα |
| Απολύμανση/απεντόμωση/μυοκτονία | from €30 per application | **offer: 3 applications (Mar, Jun, Sep) for the price of 2** |
| Αποφράξεις (preventive pressure-jet) | from €55 | emergencies priced per case/time (holidays) |
| Συντήρηση καυστήρα | oil from €50, gas from €60 | yearly maintenance sheet (φύλλο συντήρησης) for the administrator's file |
| Πυροσβεστήρες | buy / lend free (παραχώρηση) / service / refill | keeps the record and reminds of the yearly check |
| Συνεργεία καθαριότητας | from €50 | priced per building after an inspection; signed agreement incl. yearly extra jobs |
| Θέρμανση (heating oil) | below wholesale | same-day delivery, orders <1000 L, free tank cleaning, **1 in 4 fuel additives free**, "save up to €200", order code archive |

### Packages (πακέτα γνωριμίας)
- **BASIC, from €15/month**: κοινόχρηστα · 2 pest-control applications · burner/boiler service **or** a drain unblocking · flue-gas measurements · burner faults · tank cleaning (oil) · free civil-liability insurance.
- **ALL IN ONE, from €40/month**: full management + transparency of expenses and allocation · exact fuel calculation · payment by collector, bank deposit, e-banking or card · email to non-resident owners · automatic balance updates · legal receipts posted for every tenant · owners notified when tenants owe > 2 months · legal/advisory cover. *Price conditional: bimonthly issuing, agreed payment method, adequate reserve, number of apartments, monthly expenses.*
- **FULL PACK, from €120/month**: management + cleaning crew + 3 pest-control applications + burner service, measurements, faults, tank cleaning + **2 drain unblockings/year** + roof extinguisher refill + insurance.

("Προσφορά του μήνα" page sits behind a bot check; ask the client what is on it — it's a monthly promo slot.)

### What the admin panel must model (derived from the above)
1. **Contracts per building**: package + negotiated monthly price ("από") + billing period (monthly / **bimonthly**) + conditions. Price frozen per charge.
2. **Entitlements with counters**: "2 pest-control/yr", "2 drain jobs/yr", "burner service OR drain" (choice), "3 for 2" scheduled Mar/Jun/Sep. Included visits charge €0; extra ones bill at list price.
3. **Promotion rules**: N-for-M (3 for 2), every-Nth-free (1 in 4 additives), free-with-package (κοινόχρηστα, extinguisher lending, insurance), monthly offer. Stored on the charge line with the promotion id.
4. **Two kinds of money**: (a) *his revenue* — fees and service invoices to the building; (b) *the building's fund (ταμείο)* that he manages — expenses paid on its behalf (`direction='out'`) and κοινόχρηστα collected from apartments. These must be separate ledgers per building; mixing them is the classic dispute.
5. **Κοινόχρηστα allocation**: per-apartment **millesimal shares (‰) per category** (general, heating, lift, …), owner-only vs tenant expenses, closed-apartment rules, heating formula (pre-1985 fixed % + hours×‰; post-1985 Πi=[fi·εi + Mi/ΣMi·(1−Σfi·εi)]·100).
6. **Owner vs tenant**: an apartment has an owner and maybe a tenant; owner gets notified when the tenant owes > 2 months.
7. **Heating oil stock in FIFO lots**: each purchase (litres × price), tank readings, consumption charged at the lot's purchase price (their own 460 L × 0.90 + 380 L × 0.89 example).
8. **Meter readings** per apartment per period (hours / heat units / gas), dated to the gas company's reading day.
9. **Compliance calendar** per building: burner service yearly (twice for large), gas tightness every 4 years, extinguisher check/refill yearly, pest control 2–3×/season, drain cleaning 1–2×/yr — with reminders and the issued certificate attached.
10. **Expense documents**: only expenses with legal receipts (παραστατικά) can be charged; store supplier, number, scan; εργόσημο for workers without receipts.
11. **Approvals**: extraordinary expenses need the building administrator's approval (record who/when); big works need a general-assembly decision.
12. **Collection channels**: collector visits on scheduled dates (cash, receipt book), bank deposit, e-banking (→ RF codes), card.
13. **Scale**: 70k buildings × apartments is far beyond a single-user desktop SQLite — confirms the server/PostgreSQL target; the desktop should be designed as a client of that from now on.

## 12. Receipts (implemented, migration v4)

`receipts` table + `payments.receipt_id`. N payments → 1 receipt; one column on the payment means a payment can
never be on two receipts. Rules enforced **by SQLite triggers** (so the SQL console can't bypass them either):
- receipts are never deleted, never edited; the only change allowed is `issued → void` with a reason;
- while a payment is on an issued receipt, its amounts, client, service, direction, status and receipt link are frozen
  (notes stay editable); voiding the receipt unlocks them; re-receipting gives the next number in the series;
- numbers are sequential per series (`UNIQUE(series, number)`), allocated under `BEGIN IMMEDIATE`;
- only resolved, incoming (`direction='in'`) payments with total > 0 can be receipted;
- `rf_code` is validated as ISO 11649 (`src/Rf.h`), or generated from series+number when left empty.
API: `issueReceipt`, `voidReceipt`, `getReceiptById`, `getReceipts`, `getReceiptsForUser`, `getReceiptPaymentIds`,
`getReceiptIdForPayment`. Tests: `tests/receipts_test.cpp`. No UI yet.

Note on RF: an RF code really belongs to the *request* for payment (the notice/ειδοποιητήριο the customer pays
against). Storing it on the receipt records what the payment carried; when payment notices are added, generate the
RF there and copy it onto the receipt that settles it.

## 13. Extensibility: oil (litres) and future service types without schema churn

Three layers, used in this order:

1. **Service kind + JSON details (no DB change per new kind).**
   - `services.kind TEXT` (`generic`, `heating_oil`, `pest_control`, `extinguisher`, `drain`, …).
   - `payments.details TEXT` (JSON) holding kind-specific fields, e.g. heating oil
     `{"litres": 1000, "price_per_litre": 0.89, "order_code": "…", "delivery_date": "…", "tank_before": 460}`;
     extinguisher `{"units": 3, "type": "ABC 6kg", "serial": ["…"], "next_check": "2027-10"}`.
   - SQLite's `json_extract()` (and PostgreSQL `jsonb` later) query it; an expression index can be added for a hot field.
   - C++ side: a `ServiceKind` registry — each kind provides `DrawDetailsEditor(json&)`, `Validate(json)`,
     `ComputeAmount(json) -> cents` (litres × price), `Summary(json)`. Adding a kind = one new C++ file + one registry line,
     zero migrations. Unknown kinds fall back to the generic editor, so old/new app versions coexist.
   - A `service_kinds` reference table (kind, label, JSON schema version) keeps the list visible in the DB.

2. **Dedicated tables when the data has its own life across rows** — JSON is per-payment; it cannot model stock or
   time series. Heating oil needs exactly this:
   - `fuel_lots(id, building_id, purchase_date, litres, price_per_litre, supplier, document_no, payment_id)` — FIFO stock;
   - `tank_readings(id, building_id, date, litres_remaining)`;
   - `meter_readings(id, apartment_id, period, kind[hours|units|gas], value)`;
   - allocation run = consume lots FIFO between two tank readings, split by the heating formula and ‰ shares
     (the website's own example: 460 L × 0.90 + 380 L × 0.89).
   These are generic to "metered consumable" (oil, gas), not oil-specific.

3. **Domain tables everyone needs anyway** (not per-service): `contracts`, `entitlements`, `promotions`,
   `apartment_shares(apartment_id, category, permille)`, `expense_documents`, `compliance_tasks`.

Rule of thumb: *attributes of one event* → JSON details; *things that are counted, consumed or scheduled over time* → a table.
"Never change the DB again" is not a realistic goal; "a new service type needs no DB change, a new *capability* needs
one well-defined migration" is.

## 14. State after the v7/v8 migrations (2026-10-11)

- **Dates**: `payments.pay_date` (ISO date, CHECKed); all timestamps UTC `YYYY-MM-DD HH:MM:SS`, shown in local time.
- **Receipts ↔ payments**: separate tables; `payments.receipt_id` (N→1). Several payments share one receipt only if
  they were created together (`payment_batches`, e.g. a package charge) for the same client; otherwise one each.
- **Audit log**: written by triggers on every business table (JSON before/after, actor, reason), append-only,
  SHA-256 hash chain, daily anchors outside the DB, startup quick check + background full verification
  (chain, trigger integrity, every live row vs its last snapshot). SQL console is read-only.
  Limits, stated honestly: a person with the file *and* this source code can still rebuild a consistent chain;
  the anchors (and, later, sending anchors to the server/e-mail) are what make that detectable.
- **Screens** query SQL (`findPayments`, `paymentTotals`, `paymentTotalsBy`) and cache results until the data
  changes; nothing loads all payments.
- Remaining structural gap: bill vs payment are still one row (partial payments against one bill, one transfer
  for several bills). Next: see `docs/UI_PROPOSALS.md` §3 for scale, and the charges/allocations model in §5.

## 15. State after migrations v9–v10

- **Bills vs money (v9)**: `charges` = what is owed (the former payment lines, same ids); `payments` = money that
  moved (date, amount, method, reference, optional receipt); `allocations` = which money settles which charge.
  Partial payments, instalments, one transfer for several bills and client credit (unallocated money) all work.
  `charges.amount_paid`/`balance` are kept by triggers from allocations, so every existing screen shows the same
  numbers as before (verified tab by tab against the previous build).
- **Receipts** belong to money. One receipt per payment, or one for payments recorded together (same batch,
  same client). An issued receipt freezes its money, its allocations and the billed fields of the charges it
  settles; corrections = void with reason + new receipt for the same money.
- **Receipts screen**: AND/OR/NOT conditions over client, service, package, building, payment method; EXISTS
  sub-queries over indexed joins, paged.
- **Attachments (v10)**: files next to the DB under their SHA-256, rows audited and never deleted; photo and video
  viewers in the app. **Backups**: the daily backup copies the database; copy the `attachments/` folder together
  with it (next step: include it in the backup routine).
- **Promotions/contracts**: designed in `docs/PROMOTIONS_DESIGN.md`.
