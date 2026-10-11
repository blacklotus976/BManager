#pragma once
#include <string>

// One receipt (απόδειξη είσπραξης) covering one or more payments: N payments -> 1 receipt.
// The link lives on payments.receipt_id, so a payment can never sit on two receipts at once.
// Receipts are never deleted or edited: a mistake is fixed by voiding it (with a reason) and
// issuing a new one; the voided receipt stays in the books with its number.
struct Receipt {
    int id = -1;
    std::string series = "A";      // receipt book / series
    int number = 0;                // sequential per series, assigned on issue, never reused
    std::string issue_date;        // YYYY-MM-DD
    int user_id = -1;              // who paid (-1 = not a single client, e.g. the building)
    int building_id = -1;          // building it concerns (-1 = none)
    double total = 0.0;            // = sum of the linked payments' amount_paid, fixed at issue
    // How the money came in: cash | collector | bank | ebanking | card | iris | other
    std::string method = "cash";
    std::string rf_code;           // ISO 11649 RF reference the payment carried (generated if empty)
    std::string paper_ref;         // number printed on the paper receipt book, if any
    std::string bank_ref;          // bank transaction id / statement reference, if any
    std::string notes;
    std::string status = "issued"; // issued | void
    std::string void_reason;
    std::string voided_at;
    std::string created_at;
};
