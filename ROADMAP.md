# BManager — Roadmap / TODO

Small steps, each one usable on its own, so the client is never flooded.
Tick a box when it ships. Rule for every step: **no split logic** — amounts are typed as the client says them.

## Key idea: every money line knows *what kind of money* it is

The database already has the right shape (charges ↔ allocations ↔ payments ↔ receipts, `direction` in/out,
`partner_id`, `method`). What is missing is one label on each charge and the screens that use it:

| `nature` | Meaning | VAT? | Document when paid |
|---|---|---|---|
| `service` | Our own service / management fee = **our income** | yes | ΑΠΥ (private) or ΤΠΥ (has ΑΦΜ) |
| `pass_through` | Κοινόχρηστα collected **for the building** (oil, ΔΕΗ, cleaning paid to a partner) | no | Plain collection receipt (απόδειξη είσπραξης) |
| `cost` | What we pay a partner (`direction = out`) | partner's invoice | Partner's invoice is recorded, not issued |

So: **VAT lives on the charge line** (what was sold), **the payment carries how it was paid** (method, reference),
and **the receipt type is decided from the charges the payment covers**:
only `service` → ΑΠΥ/ΤΠΥ; only `pass_through` → collection receipt; mixed → one of each, automatically.

A collected κοινόχρηστο then shows as: *collected €500 for Building X → paid €470 to partner → our margin €30*
(the €30 is either a `service` charge with VAT, or shown as margin — accountant decides, see Open questions).

---

## Step 1 — Show the money honestly (UI first, tiny schema change)
- [ ] Add `nature` to charges (`service` / `pass_through` / `cost`); default old rows from `direction` + joint flag
- [ ] Building screen: per joint charge show **Collected · Paid to partners · Remaining/margin**
- [ ] Payments list: columns for method, reference, receipt no., nature
- [ ] Remove the χιλιοστά / ισόποσες / heating-oil calculators from the UI; keep one **manual amounts** mode
- [ ] Old records stay readable

## Step 2 — Tax details (needed before any real document)
- [ ] Company settings: name, ΑΦΜ, ΔΟΥ, ΓΕΜΗ, address, logo, VAT status
- [ ] Client: individual / business, ΑΦΜ (check-digit validation), ΔΟΥ, profession
- [ ] Partner: ΑΦΜ, ΔΟΥ, IBAN

## Step 3 — VAT on charges
- [ ] Service catalog: default VAT category (24 / 13 / 6 / 0 + exemption reason)
- [ ] Charge line stores net, VAT %, VAT amount, gross — frozen at creation (cents)
- [ ] Price entry either "with VAT" or "without VAT"; app computes the other
- [ ] `pass_through` charges: no VAT fields

## Step 4 — Payment → right document
- [ ] Payment method list matched to myDATA codes (cash 3, bank 1/6, card 7, IRIS 8, credit 5)
- [ ] On "Νέα είσπραξη": app proposes the document(s) from the charges' nature + client type
- [ ] Document types + own series/numbering: ΑΠΥ, ΤΠΥ, credit note, collection receipt
- [ ] Issued documents locked; cancel only via credit note / void (same rule receipts already follow)
- [ ] PDF print with VAT breakdown (QR placeholder until step 6)

## Step 5 — Costs & simple bookkeeping (λογιστικά)
- [ ] Record partner invoice: partner, date, number, net, VAT, expense category, attachment, link to building/job
- [ ] Partner balance: owed / paid / open
- [ ] Reports: income vs costs per month & per building; VAT report (output vs input); unpaid clients; unpaid partners
- [ ] Excel/CSV export for the accountant

## Step 6 — myDATA via a certified provider (ΠΠΗΤ)
- [ ] `outbox` table: issue offline, send in background, retry on failure
- [ ] Store MARK, UID, QR, provider signature on each document
- [ ] Income classification per service (category1_3 + E3_561_001/003)
- [ ] Screen for "not sent / failed" documents
- [ ] Expense classification of partner invoices

## Step 7 — Organise the code (after steps 1–6)
- [ ] Move money rules out of `App.cpp` into a Core layer: `issueDocument`, `recordPayment`, `voidDocument`, `recordPartnerInvoice` — one transaction + audit each
- [ ] Numbered migration files instead of constructor checks; foreign keys on
- [ ] Amounts as integer cents everywhere (payments/allocations still `REAL`), dates `YYYY-MM-DD`, timestamps UTC
- [ ] `uuid` + `created_by` on main tables
- [ ] Staff users + login; actor in audit log
- [ ] Remove unrelated code from repo (`src/stockviewer`, `tools/scale-bridges`, `RELEASE-V1.zip`)

## Later — Server, client portal, Viva Wallet
- [ ] Move DB to PostgreSQL behind an HTTP API using the same Core rules
- [ ] `client_accounts`: login + Argon2id password hash, lockout, must-change-password; read-only + pay
- [ ] Viva Smart Checkout: `payment_intents`, `gateway_events` (unique transaction id), webhook verified server-side
- [ ] Paid online → `recordPayment` + allocations + document via ΠΠΗΤ linked to the card transaction

## Open questions (client / accountant)
- [ ] Is collected κοινόχρηστα the building's money (pass-through) — confirm with accountant
- [ ] Our margin on κοινόχρηστα: invoiced as a management fee with VAT, or how?
- [ ] Normal VAT regime? Island reduced rates?
- [ ] Which ΠΠΗΤ (provider)? Already has one?
