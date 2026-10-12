#pragma once
#include <string>

struct Payment {
    int id = -1;
    int service_id = -1;
    int user_id = -1;
    // The payment date. Stored in the DB as one ISO date column (payments.pay_date = "YYYY-MM-DD");
    // kept split here because the UI works with the parts.
    int year = 0;
    int month = 0;
    int day = 1;
    double amount_due = 0.0;
    double amount_paid = 0.0;
    double balance = 0.0;
    std::string status; // pending, paid, overdue, future
    std::string notes;
    // Payment direction/"type" -- 2 values only:
    //   "in"  = collecting from client
    //   "out" = manager paid on client's behalf
    // A free-text comment/note on a payment is NOT a direction/type -- it's just
    // the `notes` field above, an always-visible optional attribute like any other
    // (per client correction: comment is an attribute of the payment, not a
    // substitute for it).
    std::string direction = "in";
    // Future-payment resolution: when a payment is created with status='future' it has
    // no real due date yet (year/month/day is just "when scheduled"). resolution_date is
    // the date it's expected to become real/due; resolved_at is stamped with the current
    // timestamp when the user actually resolves it (via the Resolve action), at which point
    // status moves away from 'future' to something else (paid/pending/overdue). Both are
    // nullable/empty until set.
    std::string resolution_date;
    std::string resolved_at;
    // Where it happened: the apartment the client lived in AT THE PAYMENT'S DATE (a snapshot taken
    // from the residency history when the payment is saved, so moving a client later does not
    // re-attribute old payments). -1 = unknown. created_at = when the row was entered (minute precision).
    int apartment_id = -1;
    std::string created_at;        // local time, "YYYY-MM-DD HH:MM:SS" (stored UTC in the DB)
    // The receipt covering this payment (-1 = none). Several payments share a receipt only when they
    // were created together (same batch_id, e.g. one package purchase); otherwise one each.
    int receipt_id = -1;
    // Payments created together in one action (a package charge) share a batch (-1 = created alone).
    int batch_id = -1;
    // Read-only: the joint charge (κοινόχρηστο) this line is one resident's share of (-1 = not joint).
    // Linked through batch_id: a joint charge owns its batch, and the share is the line's apartment + client.
    int joint_op_id = -1;
    // Write-only, used when the charge is created: the offer claimed for it and the price before the discount
    // (saved in offer_uses, not on the charge).
    int offer_id = -1;
    double list_price = 0.0;
};
