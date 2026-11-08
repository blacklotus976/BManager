// A small .xlsx writer (Office Open XML in a zip, stored, no compression): sheets of text, numbers, money and dates, a bold
// header with filters, frozen panes, column widths and highlighted sums. Enough for exporting the app's tables; no dependencies.
#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

namespace xlsx {

enum Style { Text = 0, Header = 1, Money = 2, Int = 3, SumLabel = 4, SumMoney = 5, SumInt = 6, Date = 7, Title = 8, Number = 9, SumNumber = 10, Muted = 11 };
struct Cell { std::string text; double num = 0; bool isNum = false; int style = Text; };
struct Sheet {
    std::string name;
    std::vector<std::vector<Cell>> rows;
    std::vector<double> widths;          // in characters; empty = worked out from the content
    int freezeRows = 1, freezeCols = 0;  // header row (and label column) stay in view
    int filterRow = 0;                   // 1-based row of the header that gets filter buttons, 0 = none
};

inline Cell T(const std::string& s, int style = Text) { Cell c; c.text = s; c.style = style; return c; }
inline Cell N(double v, int style = Number) { Cell c; c.num = v; c.isNum = true; c.style = style; return c; }

// "1.234,56 €" / "-12,5" / "45%" / "07/11/2026" / "2026-11-07" as seen on screen -> a real number or date for Excel
inline bool ParseShown(const std::string& s0, Cell& out) {
    std::string s; for (char ch : s0) if (ch != ' ' && ch != '\t') s += ch;
    // the minus sign U+2212 shows as "−"
    for (size_t at; (at = s.find("\xE2\x88\x92")) != std::string::npos; ) s.replace(at, 3, "-");
    if (s.empty()) return false;
    const bool euro = s.size() >= 3 && s.compare(s.size() - 3, 3, "\xE2\x82\xAC") == 0;     // €
    std::string n = euro ? s.substr(0, s.size() - 3) : s;
    const bool pct = !n.empty() && n.back() == '%'; if (pct) n.pop_back();
    if (n.empty()) return false;
    // a date dd/mm/yyyy or yyyy-mm-dd
    int d = 0, m = 0, y = 0; char tail = 0;
    if ((std::sscanf(n.c_str(), "%d/%d/%d%c", &d, &m, &y, &tail) == 3 && n.size() == 10) ||
        (std::sscanf(n.c_str(), "%d-%d-%d%c", &y, &m, &d, &tail) == 3 && n.size() == 10 && n[4] == '-')) {
        if (y < 1900 || m < 1 || m > 12 || d < 1 || d > 31) return false;
        // days since 1899-12-30 (Excel's day 0, with its 1900 leap-year quirk already counted in)
        auto days = [](int Y, int M, int D) { Y -= M <= 2; const int era = (Y >= 0 ? Y : Y - 399) / 400; const unsigned yoe = (unsigned)(Y - era * 400);
            const unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1; const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; return era * 146097 + (long long)doe - 719468; };
        out.num = (double)(days(y, m, d) - days(1899, 12, 30)); out.isNum = true; out.style = Date; return true;
    }
    // a number: Greek style (1.234,56) -- dots group thousands, a comma is the decimal point
    bool digits = false; std::string t;
    for (size_t i = 0; i < n.size(); i++) {
        const char ch = n[i];
        if (ch >= '0' && ch <= '9') { t += ch; digits = true; }
        else if (ch == '.') { continue; }
        else if (ch == ',') t += '.';
        else if ((ch == '-' || ch == '+') && i == 0) t += ch;
        else return false;
    }
    if (!digits) return false;
    const std::string plain = (t[0] == '-' || t[0] == '+') ? t.substr(1) : t;
    // codes, phones, ΑΦΜ: leading zeros or many digits stay text
    if (!euro && !pct && (plain.size() >= 8 || (plain.size() > 1 && plain[0] == '0' && plain.find('.') == std::string::npos))) return false;
    if (!euro && s0.find('.') != std::string::npos && s0.find(',') == std::string::npos) {     // "1.5" is not how the app shows numbers: keep as text
        bool grouping = true; size_t dot = s0.find('.'); if (s0.size() - dot - 1 != 3) grouping = false; if (!grouping) return false;
    }
    out.num = std::atof(t.c_str()); if (pct) out.num /= 100.0;
    out.isNum = true; out.style = euro ? Money : (t.find('.') != std::string::npos ? Number : Int);
    return true;
}

namespace detail {
#ifdef _WIN32
inline std::wstring Widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L' '); if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif
inline unsigned Crc(const std::string& d) {
    static unsigned tbl[256]; static bool init = false;
    if (!init) { for (unsigned i = 0; i < 256; i++) { unsigned x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xedb88320u ^ (x >> 1) : x >> 1; tbl[i] = x; } init = true; }
    unsigned c = 0xffffffffu; for (unsigned char ch : d) c = tbl[(c ^ ch) & 255] ^ (c >> 8); return c ^ 0xffffffffu;
}
inline std::string Esc(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (unsigned char ch : s) {
        if (ch == '&') o += "&amp;"; else if (ch == '<') o += "&lt;"; else if (ch == '>') o += "&gt;"; else if (ch == '"') o += "&quot;";
        else if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') o += ' ';
        else o += (char)ch;
    }
    return o;
}
inline std::string Col(int c) { std::string s; c++; while (c > 0) { s.insert(s.begin(), (char)('A' + (c - 1) % 26)); c = (c - 1) / 26; } return s; }
inline size_t Utf8Len(const std::string& s) { size_t n = 0; for (unsigned char ch : s) if ((ch & 0xC0) != 0x80) n++; return n; }
inline std::string SheetName(std::string s) {
    for (char& ch : s) if (ch == '[' || ch == ']' || ch == ':' || ch == '*' || ch == '?' || ch == '/' || ch == '\\') ch = '-';
    // at most 31 characters (not bytes)
    std::string o; size_t n = 0; for (size_t i = 0; i < s.size(); i++) { if ((s[i] & 0xC0) != 0x80) { if (++n > 31) break; } o += s[i]; }
    return o.empty() ? "Φύλλο" : o;
}
struct Zip {
    std::string out, central; int count = 0;
    void put16(std::string& b, unsigned v) { b += (char)(v & 255); b += (char)((v >> 8) & 255); }
    void put32(std::string& b, unsigned v) { put16(b, v & 0xffff); put16(b, v >> 16); }
    void add(const std::string& name, const std::string& data) {
        const unsigned crc = Crc(data), off = (unsigned)out.size(), sz = (unsigned)data.size();
        std::string h; put32(h, 0x04034b50); put16(h, 20); put16(h, 0x0800); put16(h, 0); put16(h, 0); put16(h, 0x5821);   // UTF-8 names, stored, a fixed time
        put32(h, crc); put32(h, sz); put32(h, sz); put16(h, (unsigned)name.size()); put16(h, 0);
        out += h; out += name; out += data;
        std::string c; put32(c, 0x02014b50); put16(c, 20); put16(c, 20); put16(c, 0x0800); put16(c, 0); put16(c, 0); put16(c, 0x5821);
        put32(c, crc); put32(c, sz); put32(c, sz); put16(c, (unsigned)name.size()); put16(c, 0); put16(c, 0); put16(c, 0); put16(c, 0); put32(c, 0); put32(c, off);
        central += c; central += name; count++;
    }
    std::string finish() {
        const unsigned cdOff = (unsigned)out.size(), cdSize = (unsigned)central.size();
        std::string e; put32(e, 0x06054b50); put16(e, 0); put16(e, 0); put16(e, (unsigned)count); put16(e, (unsigned)count); put32(e, cdSize); put32(e, cdOff); put16(e, 0);
        return out + central + e;
    }
};
} // namespace detail

inline std::string SheetXml(const Sheet& sh) {
    using namespace detail;
    size_t ncols = 0; for (auto& r : sh.rows) ncols = std::max(ncols, r.size());
    std::vector<double> w = sh.widths;
    if (w.size() < ncols) {
        w.resize(ncols, 0.0);
        for (auto& r : sh.rows) for (size_t c = 0; c < r.size(); c++) {
            const double len = r[c].isNum ? (r[c].style == Date ? 10.0 : 12.0) : (double)Utf8Len(r[c].text);
            w[c] = std::max(w[c], std::min(60.0, len * 1.1 + 2.0));
        }
    }
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
    x += "<sheetViews><sheetView workbookViewId=\"0\"";
    if (sh.freezeRows > 0 || sh.freezeCols > 0) {
        x += "><pane";
        if (sh.freezeCols > 0) x += " xSplit=\"" + std::to_string(sh.freezeCols) + "\"";
        if (sh.freezeRows > 0) x += " ySplit=\"" + std::to_string(sh.freezeRows) + "\"";
        x += " topLeftCell=\"" + Col(sh.freezeCols) + std::to_string(sh.freezeRows + 1) + "\" activePane=\"" +
             std::string(sh.freezeCols > 0 && sh.freezeRows > 0 ? "bottomRight" : sh.freezeRows > 0 ? "bottomLeft" : "topRight") + "\" state=\"frozen\"/></sheetView>";
    } else x += "/>";
    x += "</sheetViews>";
    if (ncols) {
        x += "<cols>";
        for (size_t c = 0; c < ncols; c++) { char b[96]; std::snprintf(b, sizeof b, "<col min=\"%zu\" max=\"%zu\" width=\"%.1f\" customWidth=\"1\"/>", c + 1, c + 1, std::max(6.0, w[c])); x += b; }
        x += "</cols>";
    }
    x += "<sheetData>";
    for (size_t r = 0; r < sh.rows.size(); r++) {
        x += "<row r=\"" + std::to_string(r + 1) + "\">";
        for (size_t c = 0; c < sh.rows[r].size(); c++) {
            const Cell& cl = sh.rows[r][c];
            const std::string ref = Col((int)c) + std::to_string(r + 1);
            if (cl.isNum && std::isfinite(cl.num)) { char b[64]; std::snprintf(b, sizeof b, "%.10g", cl.num); x += "<c r=\"" + ref + "\" s=\"" + std::to_string(cl.style) + "\"><v>" + b + "</v></c>"; }
            else if (!cl.text.empty()) x += "<c r=\"" + ref + "\" s=\"" + std::to_string(cl.style) + "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">" + Esc(cl.text) + "</t></is></c>";
            else if (cl.style != Text) x += "<c r=\"" + ref + "\" s=\"" + std::to_string(cl.style) + "\"/>";
        }
        x += "</row>";
    }
    x += "</sheetData>";
    if (sh.filterRow > 0 && ncols > 0 && (int)sh.rows.size() >= sh.filterRow)
        x += "<autoFilter ref=\"A" + std::to_string(sh.filterRow) + ":" + Col((int)ncols - 1) + std::to_string(sh.rows.size()) + "\"/>";
    x += "<pageMargins left=\"0.5\" right=\"0.5\" top=\"0.6\" bottom=\"0.6\" header=\"0.3\" footer=\"0.3\"/>";
    x += "<pageSetup orientation=\"landscape\" fitToWidth=\"1\" fitToHeight=\"0\"/>";
    x += "</worksheet>";
    return x;
}

// Writes the workbook; false + why when the file cannot be written (open in Excel, no permission...)
inline bool Write(const std::string& path, const std::vector<Sheet>& sheets, std::string& err, const std::string& author = "BManager") {
    using namespace detail;
    if (sheets.empty()) { err = "Τίποτα για εξαγωγή."; return false; }
    Zip z;
    std::string ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>";
    for (size_t i = 0; i < sheets.size(); i++) ct += "<Override PartName=\"/xl/worksheets/sheet" + std::to_string(i + 1) + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    ct += "</Types>";
    z.add("[Content_Types].xml", ct);
    z.add("_rels/.rels", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/></Relationships>");
    {   std::time_t t = std::time(nullptr); std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char ts[32]; std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
        z.add("docProps/core.xml", std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
            "<dc:title>") + Esc(sheets[0].name) + "</dc:title><dc:creator>" + Esc(author) + "</dc:creator><dcterms:created xsi:type=\"dcterms:W3CDTF\">" + ts + "</dcterms:created></cp:coreProperties>"); }
    std::string wb = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>";
    std::string rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    std::vector<std::string> used;
    for (size_t i = 0; i < sheets.size(); i++) {
        std::string nm = SheetName(sheets[i].name), base = nm; int k = 2;
        while (std::find(used.begin(), used.end(), nm) != used.end()) nm = SheetName(base.substr(0, 26) + " (" + std::to_string(k++) + ")");
        used.push_back(nm);
        wb += "<sheet name=\"" + Esc(nm) + "\" sheetId=\"" + std::to_string(i + 1) + "\" r:id=\"rId" + std::to_string(i + 1) + "\"/>";
        rels += "<Relationship Id=\"rId" + std::to_string(i + 1) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet" + std::to_string(i + 1) + ".xml\"/>";
    }
    wb += "</sheets>";
    // the filters' hidden names, so Excel shows their buttons
    std::string names;
    for (size_t i = 0; i < sheets.size(); i++) if (sheets[i].filterRow > 0) {
        size_t nc = 0; for (auto& r : sheets[i].rows) nc = std::max(nc, r.size());
        if (!nc || (int)sheets[i].rows.size() < sheets[i].filterRow) continue;
        names += "<definedName name=\"_xlnm._FilterDatabase\" localSheetId=\"" + std::to_string(i) + "\" hidden=\"1\">'" + Esc(used[i]) + "'!$A$" + std::to_string(sheets[i].filterRow) +
                 ":$" + Col((int)nc - 1) + "$" + std::to_string(sheets[i].rows.size()) + "</definedName>";
    }
    if (!names.empty()) wb += "<definedNames>" + names + "</definedNames>";
    wb += "</workbook>";
    rels += "<Relationship Id=\"rId" + std::to_string(sheets.size() + 1) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/></Relationships>";
    z.add("xl/workbook.xml", wb);
    z.add("xl/_rels/workbook.xml.rels", rels);
    // styles, in the order of the Style enum
    z.add("xl/styles.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<numFmts count=\"2\"><numFmt numFmtId=\"164\" formatCode=\"#,##0.00\\ &quot;€&quot;;\\-#,##0.00\\ &quot;€&quot;\"/><numFmt numFmtId=\"165\" formatCode=\"dd/mm/yyyy\"/></numFmts>"
        "<fonts count=\"4\"><font><sz val=\"11\"/><name val=\"Calibri\"/><family val=\"2\"/></font><font><b/><sz val=\"11\"/><color rgb=\"FFFFFFFF\"/><name val=\"Calibri\"/><family val=\"2\"/></font>"
        "<font><b/><sz val=\"11\"/><name val=\"Calibri\"/><family val=\"2\"/></font><font><b/><sz val=\"14\"/><name val=\"Calibri\"/><family val=\"2\"/></font></fonts>"
        "<fills count=\"4\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill>"
        "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FF2F5677\"/><bgColor indexed=\"64\"/></patternFill></fill>"
        "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFFFF2CC\"/><bgColor indexed=\"64\"/></patternFill></fill></fills>"
        "<borders count=\"2\"><border><left/><right/><top/><bottom/><diagonal/></border>"
        "<border><left style=\"thin\"><color rgb=\"FFBFBFBF\"/></left><right style=\"thin\"><color rgb=\"FFBFBFBF\"/></right><top style=\"thin\"><color rgb=\"FFBFBFBF\"/></top><bottom style=\"thin\"><color rgb=\"FFBFBFBF\"/></bottom><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs><cellXfs count=\"12\">"
        "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyBorder=\"1\"/>"                                                        // Text
        "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"2\" borderId=\"1\" xfId=\"0\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\" applyAlignment=\"1\"><alignment vertical=\"center\" wrapText=\"1\"/></xf>"   // Header
        "<xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyBorder=\"1\"/>"                              // Money
        "<xf numFmtId=\"3\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyBorder=\"1\"/>"                                // Int
        "<xf numFmtId=\"0\" fontId=\"2\" fillId=\"3\" borderId=\"1\" xfId=\"0\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>"                       // SumLabel
        "<xf numFmtId=\"164\" fontId=\"2\" fillId=\"3\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>"   // SumMoney
        "<xf numFmtId=\"3\" fontId=\"2\" fillId=\"3\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>"     // SumInt
        "<xf numFmtId=\"165\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyBorder=\"1\"/>"                              // Date
        "<xf numFmtId=\"0\" fontId=\"3\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/>"                                                          // Title
        "<xf numFmtId=\"4\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyBorder=\"1\"/>"                                // Number
        "<xf numFmtId=\"4\" fontId=\"2\" fillId=\"3\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>"     // SumNumber
        "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"                                                                          // Muted (no border)
        "</cellXfs><cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>");
    for (size_t i = 0; i < sheets.size(); i++) z.add("xl/worksheets/sheet" + std::to_string(i + 1) + ".xml", SheetXml(sheets[i]));
    const std::string data = z.finish();
#ifdef _WIN32
    std::ofstream f(Widen(path).c_str(), std::ios::binary | std::ios::trunc);
#else
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
#endif
    if (!f) { err = "Δεν γράφεται το αρχείο (μήπως είναι ανοιχτό στο Excel;): " + path; return false; }
    f.write(data.data(), (std::streamsize)data.size());
    if (!f) { err = "Το αρχείο δεν γράφτηκε ολόκληρο: " + path; return false; }
    return true;
}

} // namespace xlsx
