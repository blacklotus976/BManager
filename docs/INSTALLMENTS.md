# Installments (Πληρωμή σε δόσεις)

When charging a service or a package (from the Υπηρεσίες / Πακέτα tabs, or from the client's profile →
«+ Χρέωση υπηρεσίας / πακέτου»), tick **«Πληρωμή σε δόσεις»** and choose:
- the number of installments, monthly or weekly;
- whether the 1st installment is paid now;
- the receipt: **one, issued when the last installment is paid** (default), or **one per installment**.

## What is stored (migration v12)
- All installments are ordinary charges in **one batch**. A package's charges are split per service.
- Installments not yet paid are *future* charges. Each one's due date is its resolution date, so they show up in
  the reminders.
- `payment_plans` stores one row per plan: batch, number of installments, period, receipt mode and total. It is
  audited and cannot be changed or deleted.

## Paying the installments
- Resolve an installment («Επίλυση»), or record a payment («Νέα είσπραξη»). The dialog also lists upcoming
  installments, so they can be paid early.
- Money placed on an installment makes it a real (resolved) charge.
- Money that pays only one plan's installments belongs to that plan's batch.

## The one receipt
A receipt can only be issued for money received, never ahead of time. So in "one receipt" mode the installments are
collected without receipts. When the **last** installment is paid, the app automatically issues **one receipt
covering all the money of the plan**, inside the same transaction. The resolve / record-payment dialogs say so and
don't offer a separate receipt. The payment view shows the plan's progress and links to the final receipt.
