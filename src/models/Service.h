#pragma once
#include <string>

struct Service {
    int id = -1;
    std::string label;
    std::string extra_notes;
    std::string created_at;
    double suggested_price = 0.0;   // catalog price hint; packages sum their components'
};
