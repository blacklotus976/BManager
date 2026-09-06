#pragma once
#include <string>

struct User {
    int id = -1;
    std::string full_name;
    std::string phone;
    std::string address;
    std::string area;
    std::string postal_code;
    std::string contract_code;
    // Client's own legacy numbering/coding scheme -- a free-text second key that
    // doesn't align with our DB `id`, plays no other role beyond being searchable.
    std::string special_code;
    std::string created_at;
};
