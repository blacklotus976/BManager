#pragma once
// Κοινόχρηστα -- the split logic behind the "Χρέωση κοινοχρήστου" dialog.
//
// A kind is the UI helper that turns a few inputs (a total, litres x price, per-apartment thousandths...) into a
// PROPOSED split. The person can still change any share, include/exclude apartments, and the database only
// accepts it when the shares add up to the total. A kind's inputs are saved as JSON on the operation
// (joint_ops.params_json), so adding a kind = one more entry in Kinds() -- no database change.
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "imgui.h"
#include "Money.h"

struct JointRow {
    int apartment_id = -1, user_id = -1;
    std::string apt_label, user_name;
    bool included = true;
    char reason[128] = "";
    double amount = 0.0;
    double weight = 0.0;           // thousandths / meter units / ... when the kind uses them
};

struct JointDraft {
    double total = 0.0;
    std::vector<JointRow> rows;
    std::map<std::string, double> num;   // the kind's own inputs (litres, price_per_litre, ...)
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

struct JointKind {
    const char* id;
    const char* name;
    const char* help;
    const char* weightLabel;            // nullptr = the kind does not use per-apartment weights
    bool totalFromInputs;               // true = the total is computed by drawInputs (not typed)
    void (*drawInputs)(JointDraft&);    // the kind's own fields; may set d.total
};

inline std::string JointNum(double v) { char b[48]; std::snprintf(b, sizeof(b), "%.6g", v); return b; }

// params_json = the kind's inputs + the weights per apartment, e.g.
// {"litres":800,"price_per_litre":1.289,"weights":{"12":120,"13":95.5}}
inline std::string JointParamsJson(const JointDraft& d, const JointKind& k) {
    std::string s = "{";
    bool first = true;
    for (auto& [key, v] : d.num) { s += std::string(first ? "" : ",") + "\"" + key + "\":" + JointNum(v); first = false; }
    if (k.weightLabel) {
        s += std::string(first ? "" : ",") + "\"weights\":{";
        bool f2 = true;
        for (auto& r : d.rows) {
            if (r.weight <= 0) continue;
            s += std::string(f2 ? "" : ",") + "\"" + std::to_string(r.apartment_id) + "\":" + JointNum(r.weight);
            f2 = false;
        }
        s += "}";
    }
    return s + "}";
}

// Reads the "weights" object written above (apartment id -> weight); anything else is ignored.
inline std::map<int, double> JointWeightsFromJson(const std::string& js) {
    std::map<int, double> out;
    size_t p = js.find("\"weights\"");
    if (p == std::string::npos) return out;
    p = js.find('{', p);
    size_t end = js.find('}', p);
    if (p == std::string::npos || end == std::string::npos) return out;
    std::string body = js.substr(p + 1, end - p - 1);
    size_t i = 0;
    while ((i = body.find('"', i)) != std::string::npos) {
        size_t j = body.find('"', i + 1);
        size_t c = body.find(':', j);
        if (j == std::string::npos || c == std::string::npos) break;
        int apt = std::atoi(body.substr(i + 1, j - i - 1).c_str());
        double w = std::atof(body.c_str() + c + 1);
        if (apt > 0) out[apt] = w;
        i = body.find(',', c);
        if (i == std::string::npos) break;
    }
    return out;
}

inline double JointNumFromJson(const std::string& js, const std::string& key, double def = 0.0) {
    size_t p = js.find("\"" + key + "\"");
    if (p == std::string::npos) return def;
    p = js.find(':', p);
    return p == std::string::npos ? def : std::atof(js.c_str() + p + 1);
}

namespace jointkinds {
inline void NoInputs(JointDraft&) {}

inline void HeatingOil(JointDraft& d) {
    double& litres = d.num["litres"];
    double& price = d.num["price_per_litre"];
    double& extra = d.num["extra"];
    ImGui::SetNextItemWidth(140); ImGui::InputDouble("Λίτρα", &litres, 0, 0, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140); ImGui::InputDouble("Τιμή / λίτρο (€)", &price, 0, 0, "%.3f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120); ImGui::InputDouble("Λοιπά (μεταφορικά, €)", &extra, 0, 0, "%.2f");
    if (litres < 0) litres = 0;
    if (price < 0) price = 0;
    if (extra < 0) extra = 0;
    d.total = FromCents(std::llround(litres * price * 100.0) + ToCents(extra));
    ImGui::TextDisabled("%.0f λίτρα x %.3f €/λίτρο + %.2f € = %s €", litres, price, extra, FormatMoney(d.total).c_str());
}
}

inline const std::vector<JointKind>& JointKinds() {
    static const std::vector<JointKind> kinds = {
        {"equal", "Ισόποση κατανομή",
         "Το ποσό μοιράζεται ισόποσα στους ενοίκους που χρεώνονται. Κάθε μερίδιο αλλάζει χειροκίνητα.",
         nullptr, false, jointkinds::NoInputs},
        {"millesimal", "Κατά χιλιοστά",
         "Το ποσό μοιράζεται ανάλογα με τα χιλιοστά κάθε διαμερίσματος (προτείνονται από την προηγούμενη χρέωση).",
         "Χιλιοστά", false, jointkinds::NoInputs},
        {"heating_oil", "Πετρέλαιο θέρμανσης",
         "Σύνολο = λίτρα x τιμή λίτρου + λοιπά. Κατανομή με τα χιλιοστά θέρμανσης (ή ισόποσα αν δεν δοθούν).",
         "Χιλ. θέρμανσης", true, jointkinds::HeatingOil},
    };
    return kinds;
}

inline const JointKind& JointKindById(const std::string& id) {
    for (auto& k : JointKinds()) if (id == k.id) return k;
    return JointKinds().front();
}
