#pragma once
// Κοινόχρηστα -- the draft of a joint charge being typed in the "Χρέωση πολυκατοικίας" dialog.
// A joint charge is purely logistics: a total, and a free amount per apartment that must add up to it.
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>

struct JointRow {
    int apartment_id = -1, user_id = -1;
    std::string apt_label, user_name;
    bool included = true;
    char reason[128] = "";
    double amount = 0.0;
    double paid = 0.0;            // what the resident already paid on it (0 = he owes all of it)
};

struct JointDraft {
    double total = 0.0;
    std::vector<JointRow> rows;
};

// Splits `total` cents over the weights (all zero -> equally); the cents left by rounding go to the largest
// remainders, so the parts always add up exactly.
inline std::vector<long long> SplitCents(long long total, const std::vector<double>& weights) {
    std::vector<long long> out(weights.size(), 0);
    if (weights.empty() || total <= 0) return out;
    double sum = 0; for (double w : weights) sum += w > 0 ? w : 0;
    std::vector<double> w(weights.size());
    for (size_t i = 0; i < w.size(); i++) w[i] = sum > 0 ? (weights[i] > 0 ? weights[i] : 0) : 1.0;
    if (sum <= 0) sum = (double)w.size();
    long long given = 0;
    std::vector<std::pair<double, size_t>> rem;
    for (size_t i = 0; i < w.size(); i++) {
        double exact = (double)total * w[i] / sum;
        out[i] = (long long)std::floor(exact);
        given += out[i];
        rem.push_back({exact - (double)out[i], i});
    }
    std::stable_sort(rem.begin(), rem.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t k = 0; given < total && !rem.empty(); k = (k + 1) % rem.size())
        if (w[rem[k].second] > 0) { out[rem[k].second]++; given++; }
    return out;
}
