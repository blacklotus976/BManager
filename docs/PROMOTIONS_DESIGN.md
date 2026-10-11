# Promotions, offers and contracts — design (not implemented yet)

The client's real offers (from expresshomeservice.gr) are the requirements:

| Offer on the site | Rule type |
|---|---|
| Pest control: **3 applications (Mar/Jun/Sep) for the price of 2** | N-for-M on a service, inside a period |
| Heating oil: **1 in 4 fuel additives free** | every-Nth-free, counted over time per building |
| Κοινόχρηστα **free with a package**; extinguisher **lent free** in a package | included in package (price 0) |
| BASIC **from €15/month**, ALL IN ONE **from €40**, FULL PACK **from €120** | per-building contract price ("από") |
| FULL PACK: **2 drain jobs / year** included | entitlement with a counter |
| ALL IN ONE price "can drop to €40 with conditions" | conditional discount (bimonthly issuing, agreed payment method…) |
| "Προσφορά του μήνα" | time-boxed promotion, any target |

## 1. Principles

1. **A promotion never changes history.** It is evaluated when a charge is created and its effect is
   *frozen on the charge* (list price, discount, final price, which promotion). Editing or ending a promotion later
   changes nothing already billed. This keeps the audit log and the receipts meaningful.
2. **Rules are data, not code.** One `promotions` table with a `kind` and parameters; a small evaluator in C++
   (later the server) applies them. New offers = new rows, not new releases.
3. **Scope is flexible**: everyone, a client, a building, a group of buildings/clients (tags), a package, a service.
4. **Deterministic and explainable**: for every discounted line the app can say *which rule* and *why*
   ("3ο ψεκασμό του 2026, προσφορά 3-για-2").

## 2. Tables

```sql
-- what was agreed with a building or client (the package and its price)
contracts(id, party_type['client'|'building'], party_id, package_id, monthly_price, billing_every_months,
          start_date, end_date, conditions TEXT, status['active'|'ended'], created_at)

-- what a contract includes, counted
entitlements(id, contract_id, service_id, quantity, period['year'|'contract'|'month'], included_price DEFAULT 0)

-- the rules
promotions(id, name, kind, params_json, valid_from, valid_to, priority, stackable BOOL, active BOOL, created_at)
promotion_scopes(promotion_id, scope_type['all'|'client'|'building'|'tag'|'package'|'service'], scope_id)
tags(id, name);  tag_members(tag_id, party_type, party_id)      -- e.g. "Βόρεια προάστια", "Νέοι πελάτες 2026"

-- frozen on every charge line (columns added to charges)
charges.list_price, charges.discount, charges.promotion_id, charges.entitlement_id, charges.pricing_note
```

`kind` + `params_json` examples:

| kind | params | meaning |
|---|---|---|
| `percent` | `{"percent": 10}` | 10% off the matching services |
| `fixed` | `{"amount": 5.00}` | €5 off each matching line |
| `price` | `{"price": 40.00}` | set the price (introductory / conditional price) |
| `n_for_m` | `{"buy": 3, "pay": 2, "window": "year", "service_id": 17}` | 3rd of 3 within the window is free |
| `every_nth_free` | `{"n": 4, "service_id": 12, "counter": "building"}` | 1 in 4 free, counted per building |
| `bundle_price` | `{"package_id": 1, "price": 15.00}` | package total, spread over its lines |
| `free_with_package` | `{"package_id": 2, "service_id": 8}` | service costs 0 while the client has the package |

## 3. How a charge gets its price

```
list price (service catalog, or contract monthly price)
  → entitlement? (included visit left in this period → price 0, counter +1)
  → applicable promotions (scope matches, date valid, active), by priority:
        non-stackable: best single result for the customer;  stackable: applied in order
  → final price  (never below 0, cents)
  → store list_price, discount, promotion_id / entitlement_id, pricing_note on the charge
```

Counters (n-for-m, every-nth, entitlements) are **derived from existing charges** (count of matching charges
in the window/party), not from a separate counter that could drift, so voiding/removing a charge is automatically
reflected.

## 4. UI

- **Promotions screen**: list (active/expired), builder with the kinds above, scope pickers (client, building,
  tag, package, service), validity dates, preview: *"Θα επηρεάσει 37 πελάτες, 4 πολυκατοικίες"*.
- **Contracts tab per building/client**: package, monthly price, entitlements with "used 1 / 2 this year".
- **On every charge** (and in the payment dialog before saving): `Τιμή 30,00 € − 30,00 € (3-για-2) = 0,00 €`.
- **Report**: discount given per promotion / month — how much each offer costs the business.

## 5. Phasing

1. Contracts + entitlements (the packages he already sells) — biggest value, simplest rules.
2. Percent / fixed / price promotions with scopes and validity.
3. N-for-M and every-Nth (counters from charges).
4. Monthly-offer workflow and the discounts report.
