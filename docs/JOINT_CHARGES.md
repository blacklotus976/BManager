# Κοινόχρηστα — joint charges

## The rule
A joint charge is **one cost split across a collection of apartments** (normally one building's).
It splits, it never multiplies: the shares always add up exactly to the total (checked in cents by the database).
Charging the same thing to every apartment in full is not a joint charge; record it as separate charges.

## What gets stored (migration v11)
| Table | Holds |
|---|---|
| `joint_services` | The catalog: a `services` row (name, notes) plus `kind`, the split logic the dialog proposes. |
| `joint_ops` | One joint charge: building, what was charged (`source_type` joint/package/service + id), kind, description, total, date, `params_json` (the kind's inputs, e.g. litres, price per litre, thousandths), note, `batch_id`. |
| `joint_shares` | One row per apartment/resident the charge looked at: **included** with an amount, or **excluded** with a required reason (a vacant flat, its own boiler, …). |
| `charges` | One normal charge per included resident (per component for a package), with the op's `batch_id` and the share's `apartment_id`. |

- Operations and shares are a record of a decision: triggers refuse any UPDATE or DELETE. A mistake is fixed on
  the residents' charges (audited as usual).
- All three tables are covered by the audit log and the hash chain.
- Each resident then pays and gets a receipt through the normal flow (Νέα είσπραξη → receipt). Bills and money
  stay separate, as in the rest of the app.
- Upgrading: the old "joint" services (one service shared by several clients) are added to the catalog. Their past
  charges are not changed and stay visible under «Παλαιές κοινές υπηρεσίες».

## Kinds (split logic) — add new ones without changing the database
`src/ui/JointKinds.h` holds a list of kinds. Each kind has a name, a help text, optional per-apartment weights
and its own input fields:

| kind | Inputs | Proposal |
|---|---|---|
| `equal` | total | equal shares, leftover cents spread one by one |
| `millesimal` | total + thousandths per apartment | proportional to thousandths |
| `heating_oil` | litres × price per litre + extras = total; heating thousandths | proportional to heating thousandths (equal if none given) |

The weights are saved in `params_json` and offered again on the next charge of the same kind in the same building.
A new kind (for example an elevator split by floor, or water by meter readings) is one more entry in that list.
The database does not change.

## Where it is in the app
- **Κοινόχρηστα tab**: the catalog (new/edit, kind, active), «Χρέωση» per entry, every joint charge across buildings
  (total, collected so far, how many apartments were charged or excluded) → «Άνοιγμα» shows each apartment's share,
  what it has paid, and the exclusion reasons.
- **Πολυκατοικίες → building → «Χρέωση κοινοχρήστου»** plus the **«Κοινόχρηστα (N)»** tab with that building's charges.
- **Charge dialog**:
  1. Choose what to charge: a joint service, a package or a service.
  2. Choose the building. Every current resident is listed; a vacant apartment is listed as excluded.
  3. Choose the kind and fill in its inputs. The dialog proposes the shares.
  4. Change any share or exclude an apartment (a reason is required).

  Saving is only possible when the shares add up exactly to the total. A summary is shown for confirmation first.

## One charge, many views — nothing counted twice
- **Building** (Πολυκατοικίες → «Κοινόχρηστα (N)»): the joint charge as a whole: total, collected so far, and which
  apartments were charged or excluded.
- **Apartment** (Διαμερίσματα → «Κοινόχρηστα διαμερίσματος»): every share of that apartment, with the resident *at
  the time*. An empty apartment shows as excluded.
- **Client** (profile → «Κοινόχρηστα πελάτη»): only that client's own shares, wherever they lived.
- **Payments / reminders / payment view**: a share is an ordinary charge marked «[Κοινόχρηστο]». The payment view
  links back to the joint charge.

How a share is linked: the charge carries the joint charge's `batch_id` (one batch per joint charge), its
`apartment_id` and `user_id`. Together these point at exactly one `joint_shares` row. No column is duplicated,
so nothing can drift out of sync.

Money is counted where it happened, by the apartment stored on each charge:
- A building's totals are the sum of its apartments.
- An apartment's totals are the sum of its current residents' rows plus one «Προηγούμενοι ένοικοι» row.
- A client who moved is never counted in two places.

## Residents over time (no overlaps)
`residencies` keeps every stay, (client, apartment, from, to). The app refuses overlapping stays for the same client.
The apartment stored on a charge is the one the client lived in **on the charge's date**:
- Inside a stay: that stay's apartment.
- Before the client's first recorded stay (older entries for a client added today): the first stay's apartment.
- In a gap between stays, or after the last stay ended: no apartment. A charge is never put in an apartment the
  client did not live in at that date.

So a client who moves out keeps their old charges and debts in the old apartment. The new resident starts clean.
The apartment view shows both of them, each with their own rows. The client view shows only that client's rows.

## Attachments (photos, videos, PDFs): how they are stored
SQLite *can* hold files as BLOBs, but the app deliberately does not:
- **The file** is copied to `<database folder>/attachments/<first 2 letters of hash>/<sha256>.<ext>`. The name is the
  SHA-256 of the content, so identical files are stored once and any change to a file is detectable.
- **The database** keeps one `attachments` row per attachment:
  - what it is attached to (`entity` = receipt / charge / client …, plus `entity_id`);
  - the original file name, the kind (image / video / pdf / other), the size and the hash;
  - where it is stored, a note, and when it was added.

  The row is audited like everything else.
- **Never deleted.** Removing an attachment needs a reason and only hides it. Files on receipts can't be removed.
  The integrity check confirms that every file still exists with its recorded size.
- **Why not BLOBs:**
  - Videos would make the database file huge, and slow to back up and copy.
  - The files can be backed up and synced as plain files.
  - On the future server the same rows point to object storage (for example S3) without changes.

  Back up or move the database **together with** its `attachments/` folder.
- The demo data attaches the files in `demo_media/` (receipt scans, a delivery note, a boiler video and photo, a
  contract PDF). The build copies that folder next to the exe.

## Error log
Every error the app shows (database refusals, failed operations, integrity warnings), as well as graphics errors and
crashes, is appended to `<database folder>/logs/errors.csv` with the time, the source and the message. The file is
UTF-8 with a BOM, so Excel shows the Greek correctly. To read it: **Ρυθμίσεις → «Αρχείο σφαλμάτων»** (filter, newest first,
open in Excel, open the folder).
