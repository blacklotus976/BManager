#pragma once
#include "models/User.h"
#include "models/Building.h"
#include "models/Service.h"
#include "models/MasterService.h"
#include "models/Payment.h"
#include "models/Note.h"
#include "models/Receipt.h"
#include <vector>
#include <string>
#include <utility>

// Column-header sort for the paged lists: (column number, ascending) pairs, most important first.
// Column numbers are each table's own; unknown ones are ignored; empty = the default order.
using SortSpec = std::vector<std::pair<int, bool>>;

class DataConn {
public:
    virtual ~DataConn() = default;

    // --- Users ---
    virtual std::vector<User> searchUsers(const std::string& query, int offset, int limit) = 0;
    // Advanced multi-field search (Αναζήτηση tab): every non-empty field in `criteria`
    // becomes an AND-ed LIKE clause; empty fields are not filtered on.
    // idQuery is a free-text substring match against CAST(id AS TEXT), so "half-written ids"
    // (e.g. "5" matching id=5, 15, 52) work the same way as every other text field.
    virtual std::vector<User> advancedSearchUsers(const User& criteria, const std::string& idQuery = "") = 0;
    virtual bool addUser(const User& user) = 0;
    virtual bool editUser(int id, const User& user) = 0;
    virtual bool removeUser(int id) = 0;
    virtual User getUserById(int id) = 0;

    // --- Buildings & apartments ---
    // A building exists on its own, regardless of who lives in it: address, area,
    // ΤΚ and the building's own ΑΦΜ, plus a list of apartments (real rows, so
    // vacant ones are visible). A client points at ONE apartment via
    // User::apartment_id -- that is the only place "where they live" is stored.
    // User::address/area/postal_code/apartment/building_id are derived by JOIN.
    //
    // Removal semantics (FK cascades are NOT enforced in this app, so these do the
    // cleanup by hand): removing an apartment leaves its residents unassigned
    // (apartment_id = NULL, nobody is deleted); removing a building removes its
    // apartments and unassigns their residents.
    virtual std::vector<Building> getBuildings() = 0;
    virtual Building getBuildingById(int id) = 0;
    virtual int addBuilding(const Building& building) = 0;          // new id, or -1
    virtual bool editBuilding(int id, const Building& building) = 0;
    virtual bool removeBuilding(int id) = 0;
    virtual std::vector<Apartment> getApartmentsForBuilding(int buildingId) = 0;
    virtual int addApartment(int buildingId, const std::string& label) = 0;  // new id, or -1
    virtual bool editApartment(int id, const std::string& label) = 0;
    virtual bool removeApartment(int id) = 0;

    // --- Apartment history ---
    // Residencies record who lived in which apartment and when (oldest first). Moving a client
    // (editUser with a different apartment) closes the old stay and opens the new one by itself.
    virtual std::vector<Residency> getResidencies(int apartmentId) = 0;
    virtual std::vector<Residency> getUserResidencies(int userId) = 0;
    // Replaces a client's whole history (used by the demo data / imports); also re-derives the
    // apartment snapshot of that client's payments from the new dates.
    virtual bool setResidencyHistory(int userId, const std::vector<Residency>& history) = 0;
    virtual std::vector<Payment> getPaymentsForApartment(int apartmentId) = 0;
    // The apartment a client lived in on that date (residency history; falls back to their current
    // apartment), -1 = none. This is the automatic apartment a new payment is recorded against.
    virtual int getApartmentOnDate(int userId, int y, int m, int d) = 0;
    // The last database error nobody has looked at yet ("" = none); the UI shows it as a banner.
    virtual std::string takeLastError() = 0;

    // --- Services ---
    // A `services` row is a generic, reusable catalog entry (e.g. "Πετρέλαιο",
    // "Καθαριότητα") created ONCE, never duplicated per-user. Which user(s) it
    // applies to lives in service_users -- one row = single owner, several rows
    // = joint/shared. Periodicity/recurrence concepts were removed per the
    // client: the app logs/registers payments (including scheduling ahead as
    // future payments); it is not the app's job to automate anything periodical.
    virtual std::vector<Service> searchServices(const std::string& query, int offset, int limit) = 0;
    virtual std::vector<Service> advancedSearchServices(const std::string& label, const std::string& extra_notes,
                                                          const std::string& idQuery = "") = 0;
    virtual bool addService(int userId, const Service& service) = 0;
    // Same insert as addService, but returns the new service id (-1 on failure) so callers
    // can chain more service_users rows onto it (multi-owner creation).
    virtual int addServiceGetId(int userId, const Service& service) = 0;
    virtual bool editService(int id, const Service& service) = 0;
    virtual bool removeService(int id) = 0;
    virtual Service getServiceById(int id) = 0;
    virtual std::vector<Service> getServicesForUser(int userId) = 0;
    virtual std::vector<User> getUsersForService(int serviceId) = 0;
    virtual bool addServiceUser(int serviceId, int userId) = 0;

    // --- Master Services (named packages grouping several real, independent
    // services) ---
    // A package is purely an organizational/viewing grouping on top of services
    // that already work exactly as they do now -- payments never change, they
    // still attach to one real (component) service + one user via the existing
    // service_id/user_id/service_users mechanism. No new payment dimension.
    virtual std::vector<MasterService> getMasterServices() = 0;
    virtual std::vector<Service> getMasterServiceComponents(int masterServiceId) = 0;
    virtual int addMasterService(const std::string& label) = 0;
    virtual bool addMasterServiceComponent(int masterServiceId, int serviceId) = 0;
    virtual bool removeMasterServiceComponent(int masterServiceId, int serviceId) = 0;
    virtual bool removeMasterService(int id) = 0;
    struct MasterServiceSummary {
        MasterService masterService;
        std::vector<Service> components;
        std::vector<std::vector<User>> componentRosters; // roster per component, via getUsersForService
    };
    virtual std::vector<MasterServiceSummary> getMasterServiceSummaries() = 0;

    // --- Payments ---
    virtual bool addPayment(const Payment& payment) = 0;
    // Several payments created together as ONE action (e.g. charging a package: one payment per
    // component service). All or nothing, in one transaction; they share a new batch id, which is what
    // later allows them -- and only them -- to be put on one shared receipt. Returns the batch id or -1.
    virtual int addPaymentBatch(const std::vector<Payment>& payments, const std::string& label) = 0;
    virtual bool editPayment(int id, const Payment& payment) = 0;
    virtual bool removePayment(int id) = 0;
    virtual Payment getPaymentById(int id) = 0;
    virtual std::vector<Payment> getPaymentsForService(int serviceId) = 0;
    virtual std::vector<Payment> getPaymentsForUser(int userId) = 0;
    virtual std::vector<Payment> getFuturePayments() = 0;
    virtual std::vector<Payment> getRecentPayments(int limit) = 0;
    // All payments, unfiltered. Not used by any screen (they query with findPayments/paymentTotals);
    // kept for exports and tests.
    virtual std::vector<Payment> getAllPayments() = 0;
    // Advanced payment search (Αναζήτηση tab): payment's own fields plus owner-name /
    // service-label substring match via JOIN against users/services. Any empty string
    // param is not filtered on. year/month/day, idQuery, serviceIdQuery and userIdQuery are
    // all free-text substring matches against the stringified column (LIKE, not exact
    // equality) so half-written numbers behave like half-written text everywhere.
        // Advanced payment search (Αναζήτηση tab): payment's own fields plus owner-name /
    // service-label substring match via JOIN against users/services. Any empty string
    // param is not filtered on. year/month/day, idQuery, serviceIdQuery and userIdQuery are
    // all free-text substring matches against the stringified column (LIKE, not exact
    // equality) so half-written numbers behave like half-written text everywhere.
    virtual std::vector<Payment> advancedSearchPayments(const std::string& year, const std::string& month, const std::string& day,
                                                          const std::string& amount_due_query, const std::string& amount_paid_query,
                                                          const std::string& status, const std::string& direction,
                                                          const std::string& notes, const std::string& resolution_date,
                                                          const std::string& userName, const std::string& serviceLabel,
                                                          const std::string& idQuery = "", const std::string& serviceIdQuery = "",
                                                          const std::string& userIdQuery = "") = 0;

    // --- Receipts (N payments -> 1 receipt) ---
    // issueReceipt links the given payments to one new receipt and returns its id (-1 + takeLastError()
    // on refusal). Rules: at least one payment; each one exists, is resolved (not 'future'), is a
    // collection (direction 'in') and is not already on an ISSUED receipt. total = sum of their
    // amount_paid (in cents) and must be > 0. number is the next one in r.series. rf_code is kept if
    // given (must be a valid RF) or generated from series+number. Everything happens in one transaction.
    // Once issued, the DB itself refuses to change/delete those payments' amounts, client, service or
    // receipt link, and refuses any edit of the receipt except voiding it.
    virtual int issueReceipt(const Receipt& r, const std::vector<int>& paymentIds) = 0;
    // Records payments (one, or several as one batch) AND, if `receipt` is given, their receipt -- all in one
    // transaction: either everything is saved or nothing. Returns the first new payment id, -1 on failure.
    virtual int addPaymentsWithReceipt(const std::vector<Payment>& payments, const std::string& batchLabel,
                                       const Receipt* receipt) = 0;
    // The number the next receipt of a series will get (for showing "θα εκδοθεί Α-124").
    virtual int nextReceiptNumber(const std::string& series) = 0;
    // Edits a payment (e.g. resolving a future one) and issues its receipt in the same transaction.
    virtual bool editPaymentWithReceipt(int id, const Payment& payment, const Receipt& receipt) = 0;
    // Voids an issued receipt (reason required). Its payments become free to edit or re-receipt;
    // they keep pointing at the voided receipt until they are put on a new one, so history stays.
    virtual bool voidReceipt(int id, const std::string& reason) = 0;
    virtual Receipt getReceiptById(int id) = 0;
    virtual std::vector<Receipt> getReceipts(int offset, int limit) = 0;   // newest first
    virtual std::vector<Receipt> getReceiptsForUser(int userId) = 0;
    virtual std::vector<int> getReceiptPaymentIds(int receiptId) = 0;
    virtual int getReceiptIdForPayment(int paymentId) = 0;                 // -1 = none

    // --- Payment queries & totals in SQL (screens never load every payment into memory) ---
    struct PaymentFilter {
        std::string fromDate, toDate;        // inclusive ISO dates, "" = open
        int userId = -1, serviceId = -1, apartmentId = -1;
        std::string direction;               // "", "in", "out"
        int status = 0;                      // 0 all, 1 resolved (not 'future'), 2 future
        int owed = 0;                        // 0 any, 1 settled (resolved, balance <= 0), 2 still owed (resolved, balance > 0)
        double minDue = 0.0, maxDue = 0.0;   // amount_due bounds, 0 = none
        // Accent/case-insensitive substring filters (same matching as the search screen). `text` matches any of
        // client, service, notes, status, date, client's building address.
        std::string text, clientText, serviceText, notesText, buildingText;
    };
    // Sums in exact cents. "Resolved" = status != 'future'.
    struct MoneyTotals {
        long long count = 0;
        double collected = 0, paidOut = 0;      // resolved: amount_paid by direction
        double owedIn = 0, owedOut = 0;         // resolved: positive balance by direction
        double futureIn = 0, futureOut = 0;     // future: amount_due by direction
        double dueAll = 0, paidAll = 0;         // every matching row
    };
    enum class PayGroup { User, Service, Apartment, Day, UserService };
    struct GroupedTotals { std::string key; MoneyTotals t; };   // key: id ("12"), date, or "user|service"
    enum class PayOrder { DateDesc, DateAsc, ResolutionDesc };
    virtual std::vector<Payment> findPayments(const PaymentFilter& f, PayOrder order, int offset, int limit) = 0;
    virtual long long countPaymentsWhere(const PaymentFilter& f) = 0;
    virtual MoneyTotals paymentTotals(const PaymentFilter& f) = 0;
    virtual std::vector<GroupedTotals> paymentTotalsBy(PayGroup group, const PaymentFilter& f) = 0;
    // Changes whenever this connection writes anything. Screens cache what they computed against it, so
    // an unchanged screen costs nothing per frame.
    virtual long long dataVersion() = 0;

    // --- Paged "phone book" pickers (dashboard filters) -------------------
    // These fetch only one page of rows at a time -- the dashboard pickers
    // never load the full users/services table just to render a filter row.
    //
    // pickerUsers:
    //   letterFilter  "" = all rows; "Α" = only names whose first char is Α
    //                 (uses LIKE '<letter>%'). Matches how the rest of the app
    //                 treats Greek text -- caller passes the exact letter form
    //                 that appears in stored data).
    //   nameSearch    substring match anywhere in full_name (LOWER-matched).
    //   reverseSort   true = Z→A, false = A→Z.
    // letters: set of starting-letter prefixes to include (OR-ed together).
    //          Empty = no letter filtering (all users).
    //          e.g. {"Α","Π"} matches names starting with Α or Π.
    virtual std::vector<User> pickerUsers(const std::vector<std::string>& letters,
                                           const std::string& nameSearch,
                                           int offset, int limit,
                                           bool reverseSort) = 0;
    virtual int pickerUsersCount(const std::vector<std::string>& letters,
                                  const std::string& nameSearch) = 0;

    // pickerServices: name substring match, always alphabetical by label.
    virtual std::vector<Service> pickerServices(const std::string& nameSearch,
                                                 int offset, int limit) = 0;
    virtual int pickerServicesCount(const std::string& nameSearch) = 0;

    // --- Audit trust ---
    // Every insert/update/delete on business tables is recorded by DATABASE TRIGGERS (not by app code),
    // as JSON snapshots, with who did it (actor) and an optional reason, in an append-only, hash-chained
    // audit_log. The actor defaults to "<os user>@<machine>"; set it to the logged-in user once logins
    // exist. The reason applies to every write until it is set again ("" = none).
    virtual void setAuditActor(const std::string& actor) = 0;
    virtual void setAuditReason(const std::string& reason) = 0;
    struct AuditCheck {
        bool ok = true;
        long long entries = 0;
        std::vector<std::string> problems;   // human-readable, Greek
    };
    // Full integrity check: audit triggers present and unmodified, hash chain unbroken, every current row
    // equal to its last audited snapshot (catches edits made with the triggers removed), daily anchors match.
    virtual AuditCheck verifyAuditLog() = 0;

    // --- Timeline / Audit ---
    // category: "business" for normal-app-flow CRUD on users/services/payments/service_users,
    // "db" for the raw SQL console (and anything else that isn't a business action).
    virtual std::vector<std::string> getTimelineActions(const std::string& dateFilter, int offset, int limit,
                                                          const std::string& category = "", bool friendly = false) = 0;
    virtual void logAction(const std::string& table, int recordId, const std::string& action, const std::string& oldData, const std::string& newData, const std::string& category = "db") = 0;

    // --- Metrics ---
    virtual double getTotalPaid(int year, int month, int userId = -1, int serviceId = -1) = 0;
    virtual double getTotalPending(int year, int month, int userId = -1, int serviceId = -1) = 0;

    // --- Notes (standalone scratchpad, not tied to users/services/payments) ---
    virtual std::vector<Note> getAllNotes() = 0;
    virtual bool addNote(const Note& note) = 0;
    virtual bool editNote(int id, const Note& note) = 0;
    virtual bool removeNote(int id) = 0;

    // --- Backup ---
    virtual bool exportAllTablesToCSV(const std::string& folder) = 0;

    // --- Raw View (paginated) ---
    virtual std::vector<std::vector<std::string>> getPaginatedData(const std::string& table, int offset, int limit) = 0;
    virtual std::vector<std::string> getTableColumns(const std::string& table) = 0;
    virtual int getRowCount(const std::string& table) = 0;

    struct JointServiceSummary {
        Service service;
        std::vector<User> users;
        std::vector<double> userTotalPaid;   // per user, total paid for this service (any direction)
        std::vector<double> userTotalDue;    // per user, total due for this service (any direction)
        double totalDue, totalPaid;
        // Direction-aware net, e.g. a bill paid on the clients' behalf (direction='out')
        // then collected back from them (direction='in') -- what should trend to zero.
        std::vector<double> userNet;         // per user: paidOut - collected
        double totalNet = 0.0;
    };
    virtual std::vector<JointServiceSummary> getJointServices() = 0;

    // --- Raw SQL console (Settings screen power-user feature) ---
    virtual bool executeRawQuery(const std::string& sql, std::vector<std::string>& outColumns,
                                  std::vector<std::vector<std::string>>& outRows, std::string& outError) = 0;

    // --- Paged list queries (list-view pagers; SQL-level offset/limit) ---
    // sort = the list's column-header sort (several keys allowed). It is applied in SQL so it
    // spans every page.
    // These back the Clients / Services / Payments list tabs so the UI only
    // ever holds ONE page in RAM, regardless of table size. Each pageX takes
    // an offset + limit and returns exactly that slice; each countX returns
    // the total for the same filter set so the pager can display "X-Y of Z".
    //
    // pageUsers scope: 0 = all fields, 1 = full_name only, 2 = special_code only,
    //                  3 = phone only, 4 = building address only. `letters` is
    //                  the multi-select Greek-letter name filter (empty = none).
    virtual std::vector<User> pageUsers(const std::string& query, int scope,
                                         const std::vector<std::string>& letters,
                                         int offset, int limit, const SortSpec& sort = {}) = 0;
    virtual int countUsers(const std::string& query, int scope,
                            const std::vector<std::string>& letters) = 0;

    // Services: only single-owner services (joint ones live on their own tab).
    // query does a label/notes substring match.
    virtual std::vector<Service> pageServices(const std::string& query, int offset, int limit,
                                               const SortSpec& sort = {}) = 0;
    virtual int countServices(const std::string& query) = 0;

    // Payments: full filter set at SQL level (dates as YYYY-MM-DD strings,
    // userId/serviceId -1 = no filter, status "" | "future" | "resolved",
    // direction "" | "in" | "out").
    virtual std::vector<Payment> pagePayments(const std::string& startDate, const std::string& endDate,
                                               int userId, int serviceId,
                                               const std::string& status, const std::string& direction,
                                               int offset, int limit, const SortSpec& sort = {}) = 0;
    virtual int countPayments(const std::string& startDate, const std::string& endDate,
                               int userId, int serviceId,
                               const std::string& status, const std::string& direction) = 0;

};
