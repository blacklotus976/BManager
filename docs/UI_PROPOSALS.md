# UI proposals — edited payments, receipts, search at 70k clients

_Proposals only; nothing here is implemented yet. Everything listed is already supported by the database
(audit log with actor/reason/JSON before-after, receipts, batches, SQL totals)._

---

## 1. Showing an edited payment (crucial)

Goal: anyone looking at a payment can see at a glance that it was changed, and in two clicks see
**what, when, by whom and why** — without reading raw logs.

### 1.1 In every list (Πληρωμές, timeline, client ledger)
- A small badge on the row: **`✎ 2`** = edited twice. Hover tooltip: *"Τελευταία αλλαγή 11/10/2026 14:32 από
  γραμματεία@pc1 — «Διόρθωση ποσού μετά από τηλεφώνημα»"*.
- A **lock** icon when the payment is on an issued receipt (`🔒 Α-123`), click → opens the receipt.
- Optional filter chip "Μόνο επεξεργασμένες" (payments with ≥1 UPDATE in `audit_log`).

### 1.2 Payment view window → new tab **«Ιστορικό αλλαγών»**
A vertical timeline, newest on top, one card per audit entry for this payment:

```
● 11/10/2026 14:32 · γραμματεία@pc1
  Αιτιολογία: Διόρθωση ποσού μετά από τηλεφώνημα
  Πληρωμένο ποσό   40,00 €  →  35,00 €
  Υπόλοιπο          0,00 €  →   5,00 €
● 03/10/2026 09:10 · γραμματεία@pc1 · Δημιουργία
  Πελάτης ΠΑΠΑΔΟΠΟΥΛΟΣ Γ. · ΔΙΑΧΕΙΡΙΣΗ · 40,00 € · Α1 Πατησίων 205
```
- Only the **changed fields**, with Greek labels and formatted values (money as `35,00 €`, dates as `dd/mm/yyyy`,
  ids resolved to names). Built from `old_data`/`new_data` JSON (`json_extract`), so no new storage.
- Red/green colouring for decreases/increases of money; struck-through old value.
- Footer: *"Το ιστορικό είναι αμετάβλητο και ελέγχεται κρυπτογραφικά"* + the entry's short hash, for credibility
  when showing it to the customer.

### 1.3 Editing
- The edit dialog gets a **required "Αιτιολογία αλλαγής"** field whenever money, date, client, service or
  apartment change (optional for notes only). The app calls `setAuditReason(text)` before saving and clears it
  after. The existing confirm modal (field diffs) shows the reason too.
- Receipted payments open **read-only**, with a banner: *"Έχει εκδοθεί απόδειξη Α-123. Για αλλαγή ακυρώστε πρώτα
  την απόδειξη."* + button **«Μετάβαση στην απόδειξη»**.
- Deleting asks for a reason too. Deleted payments stay visible in a **«Διαγραμμένες»** view (from DELETE
  entries in the audit log), read-only, with who/when/why.

### 1.4 Towards the customer
- The client statement (καρτέλα) printout gets a short **«Διορθώσεις»** section listing corrected entries
  (date, old → new, reason). This is what removes misunderstandings: the customer sees the correction, not just a
  different number.

---

## 2. Showing receipts

### 2.1 Receipt view
```
 ΑΠΟΔΕΙΞΗ ΕΙΣΠΡΑΞΗΣ  Α-000123            [ Εκδόθηκε ]   (or diagonal red «ΑΚΥΡΩΜΕΝΗ»)
 11/10/2026 · ΠΑΠΑΔΟΠΟΥΛΟΣ ΓΕΩΡΓΙΟΣ · Πατησίων 205, Α1
 ─────────────────────────────────────────────────────────
 ΔΙΑΧΕΙΡΙΣΗ – ΜΗΝΙΑΙΑ ΑΜΟΙΒΗ          40,00 €
 ΑΠΕΝΤΟΜΩΣΗ                            30,00 €      (πακέτο BASIC, ίδια καταχώριση)
 ─────────────────────────────────────────────────────────
 Σύνολο                                70,00 €
 Τρόπος: e-banking · RF18 5390 0754 7034 [αντιγραφή] [QR] · Τράπ. αναφ.: 0012345
 Χάρτινη απόδειξη: 4521 · Συνημμένο: 📎 scan.jpg
```
- RF with copy button and a QR code (e-banking / IRIS).
- Actions: **Εκτύπωση (A5/PDF)**, **Ακύρωση** (modal: reason required, lists the payments that will be unlocked).
- A voided receipt stays in every list, greyed and struck through, with *"Αντικαταστάθηκε από Α-000130"* when its
  payments were re-receipted (derivable: same payments now point to a newer receipt).

### 2.2 Issuing
- From the Payments list: tick payments → **«Έκδοση απόδειξης»**. The button is enabled only for a valid
  selection: one payment, or payments of the same batch (package charge) and client — the rule the database enforces;
  the tooltip explains why when disabled.
- Right after a package charge: a prompt *"Έκδοση μίας απόδειξης για τις 3 πληρωμές;"*.

### 2.3 Receipts tab
- List with filters (series, date range, method, status, client) and **totals per method** for the day —
  this is the daily cash closing (κλείσιμο ταμείου): cash in hand vs bank vs card.

---

## 3. Fast search and 70,000 clients

### 3.1 Where it stands (measured: 70k clients, 4.7k buildings, 1.05M payments, 1.19M audit entries)
| Action | Time |
|---|---|
| App start (after the first run of the day) | **2 ms** |
| Full audit verification (background thread) | 8.6 s, non-blocking |
| Edit a payment (+ audit + hash chain) | 3–5 ms |
| One client's ledger / one apartment / one month totals / first page | < 1 ms |
| Dashboard totals over everything | 0.44 s *(only after a change)* |
| Totals per client / apartment (Buildings tab) | 0.9 s *(only after a change)* |
| Reports: per-day buckets / per client+service | 0.46 s / 1.7 s *(only after a change)* |
| Free-text payment search (Greek, accent-insensitive) | 1.3–1.5 s *(debounced)* |
| Load all 70k clients into memory | 96 ms, ~20 MB |

Unchanged screens now cost nothing per frame (results are cached until data changes); before, every screen
recomputed everything every frame.

### 3.2 Proposed next steps (in order of value)
1. **Pickers instead of combos.** The payment dialog's client combo lists *all* clients; with 70k that is unusable.
   Use the existing paged "phone book" picker (`DrawUserFilterPicker`) everywhere a client/service is chosen,
   with type-ahead.
2. **Full-text index for clients** (SQLite FTS5): `clients_fts(name, phone, code, address)` storing the
   *folded* text, kept in sync by triggers. Prefix search `παπαδ*` → milliseconds at any size; search as you type,
   top 50 + "περισσότερα".
3. **Summary tables kept by triggers** (`client_totals`, `apartment_totals`, `daily_totals`): every payment
   insert/update/delete adjusts a few counters in the same transaction. Dashboard/Buildings/Reports then read
   hundreds of rows instead of scanning a million — instant regardless of size, and they stay exact because the
   audit verification can recompute and compare them.
4. **Heavy work off the UI thread**: WAL is already on, so a second read-only connection can compute reports in
   the background with a "υπολογισμός…" indicator instead of a hitch.
5. **Payment search by structure first**: pick client/building/service (instant, indexed), free text only within
   that scope; global free text keeps the debounce + a minimum of 3 characters.
6. **Stop loading all clients**: replace `g_app.users` with paged queries + a small id→name cache.
7. **On the server** (PostgreSQL): `unaccent` + `pg_trgm` GIN indexes give fuzzy Greek search; summary tables
   become materialized views or the same trigger-maintained tables.
