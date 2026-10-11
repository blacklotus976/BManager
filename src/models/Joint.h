#pragma once
#include <string>
#include "Service.h"

// Κοινόχρηστα. A joint charge is ONE cost split across a collection of apartments (usually the
// apartments of a building). It never multiplies: the shares always add up to the total.
//
//   JointService  = a catalog entry ("ΘΕΡΜΑΝΣΗ", "ΚΑΘΑΡΙΟΤΗΤΑ ΚΛΙΜΑΚΟΣ"): a services row + the kind of
//                   split logic the charge dialog uses for it.
//   JointOp       = one charge of a cost to one apartment collection (immutable once saved).
//   JointShare    = one row per apartment/resident of that collection: included with its amount, or
//                   excluded with a reason. Included shares became charges (same batch, same apartment).
//
// `kind` selects the UI helper that proposes the split ("equal", "heating_oil", ...). Its inputs are kept
// in params_json, so a new kind is C++ only -- the database never changes for it.
struct JointService {
    int id = -1;
    Service service;               // label, notes, default total (suggested_price)
    std::string kind = "equal";
    bool active = true;
};

struct JointOp {
    int id = -1;
    int building_id = -1;          // -1 = a hand-picked collection spanning buildings
    std::string source_type;       // "joint" | "service" | "package"
    int source_id = -1;
    std::string kind = "equal";
    std::string label;
    double total = 0.0;
    std::string op_date;           // YYYY-MM-DD, the charges' date
    std::string params_json = "{}";
    std::string note;
    int batch_id = -1;
    std::string created_at;
    // read-only, filled by queries
    std::string building_address;
    int included = 0, excluded = 0;
    double paid = 0.0;             // money already allocated to the op's charges
};

struct JointShare {
    int id = -1;
    int op_id = -1;
    int apartment_id = -1;
    int user_id = -1;              // -1 = nobody lives there (always excluded)
    double amount = 0.0;
    bool included = true;
    std::string exclude_reason;
    // read-only
    std::string apartment_label, user_name;
    double paid = 0.0;
    // read-only, filled by getJointSharesFor: the charge this share belongs to
    std::string op_label, op_date, building_address;
    double op_total = 0.0;
};

// A line of what is being split (a package splits each component; a service or joint service is one line).
struct JointLine {
    int service_id = -1;
    double total = 0.0;
};
