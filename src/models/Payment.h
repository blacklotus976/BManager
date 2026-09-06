#pragma once
#include <string>

struct Payment {
    int id = -1;
    int service_id = -1;
    int user_id = -1;
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
};
