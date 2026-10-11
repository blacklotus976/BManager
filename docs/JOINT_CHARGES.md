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

## Error log
Every error the app shows (database refusals, failed operations, integrity warnings), as well as graphics errors and
crashes, is appended to `<database folder>/logs/errors.csv` with the time, the source and the message. The file is
UTF-8 with a BOM, so Excel shows the Greek correctly. To read it: **Ρυθμίσεις → «Αρχείο σφαλμάτων»** (filter, newest first,
open in Excel, open the folder).
