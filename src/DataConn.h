#pragma once
#include "models/User.h"
#include "models/Building.h"
#include "models/Service.h"
#include "models/MasterService.h"
#include "models/Joint.h"
#include "models/Partner.h"
#include "models/Offer.h"
#include "models/Payment.h"
#include "models/Note.h"
#include "models/Receipt.h"
#include "models/MoneyPayment.h"
#include "models/Attachment.h"
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
    // "Deleting" a client takes him out of every list, closes his stay in his apartment and records when -- his charges,
    // payments and receipts stay (they are the books). userDeletedAt: "" while he is active.
    virtual bool removeUser(int id) = 0;
    virtual std::string userDeletedAt(int id) = 0;
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
    // Every error the app shows is also appended to <db folder>/logs/errors.csv (time, source, message).
    virtual std::string errorLogPath() = 0;

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
    virtual bool removeMasterService(int id) = 0;          // retires it (kept for history, hidden from lists)
    virtual bool renameMasterService(int id, const std::string& label) = 0;
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
    // We pay (part of) what we owe on a paid-out cost line: one new payment out on payDate, allocated to that line (never more than its balance).
    virtual bool payOutCharge(int chargeId, double amount, const std::string& payDate) = 0;
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

    // --- Bills (charges) and money (payments) ---
    // The `Payment` struct used by the screens is a CHARGE line (what is owed), with amount_paid / balance
    // maintained by the database from the money allocated to it. Money itself is a MoneyPayment.
    //
    // Records money and what it settles, optionally with its receipt, all-or-nothing. Allocations may be fewer
    // than the amount (the rest is the client's credit). Returns the new payment id or -1.
    virtual int recordPayment(const MoneyPayment& money, const std::vector<Allocation>& allocations, const Receipt* receipt) = 0;
    virtual std::vector<MoneyPayment> getMoneyForUser(int userId) = 0;
    virtual std::vector<MoneyPayment> getRecentMoney(int limit) = 0;                    // newest entered first                 // newest first
    virtual std::vector<MoneyPayment> getMoneyForCharge(int chargeId) = 0;             // .allocated = the part for this charge
    virtual std::vector<Allocation> getAllocationsForMoney(int paymentId) = 0;
    virtual std::vector<Payment> getOpenCharges(int userId) = 0;                       // resolved, balance > 0, oldest first
    virtual double getClientCredit(int userId) = 0;                                    // money in not yet allocated
    // Receipt for money already recorded (payment ids, not charge ids). Same rules as issueReceipt.
    virtual int issueReceiptForMoney(const Receipt& r, const std::vector<int>& moneyIds) = 0;

    // --- Attachments (photos, videos, PDFs, ...) ---
    // Copies the file into the attachments folder (by content hash) and records it. Returns the id or -1.
    virtual int addAttachment(const std::string& entity, int entityId, const std::string& sourcePath, const std::string& note) = 0;
    virtual std::vector<Attachment> getAttachments(const std::string& entity, int entityId, bool includeRemoved = false) = 0;
    virtual bool removeAttachment(int id, const std::string& reason) = 0;   // hides it; refused for receipts
    virtual std::string attachmentFullPath(const Attachment& a) = 0;          // absolute path of the stored copy

    // --- Receipts search (Αποδείξεις screen) ---
    // Conditions over what a receipt covers, combined with ALL (AND) or ANY (OR); each can be negated (NOT).
    struct ReceiptCond {
        enum Field { Client, Service, Package, Building, Method } field = Client;
        int id = -1;              // client / service / package / building id
        std::string text;         // method code for Method
        bool negate = false;
    };
    struct ReceiptQuery {
        std::vector<ReceiptCond> conds;
        bool matchAll = true;
        std::string fromDate, toDate;     // issue date, inclusive
        int status = 0;                   // 0 all, 1 issued, 2 void
        std::string number;               // substring of "A-123" / paper / bank reference / RF
        std::string text;                 // free text: number fields above OR the client's name (accent-insensitive)
        SortSpec sort;                    // 0 number, 1 date, 2 client, 3 method, 4 references, 5 total, 6 status, 7 kind (personal / joint); empty = newest first
    };
    virtual std::vector<Receipt> searchReceipts(const ReceiptQuery& q, int offset, int limit) = 0;   // newest first
    virtual long long countReceipts(const ReceiptQuery& q, double* total = nullptr) = 0;
    struct ReceiptLine { Payment charge; double amount = 0.0; };      // what a receipt pays, per charge
    virtual std::vector<ReceiptLine> getReceiptLines(int receiptId) = 0;
    virtual std::vector<MoneyPayment> getReceiptMoney(int receiptId) = 0;

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
                                       const Receipt* receipt, bool receiptPerLine = false) = 0;
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
        bool missingReceipt = false;         // money came in for it but no issued receipt covers that money
        double minDue = 0.0, maxDue = 0.0;   // amount_due bounds, 0 = none
        int joint = 0;                       // 0 any, 1 only shares of joint charges (κοινόχρηστα), 2 only the client's own charges
        int jointOpId = -1;                  // only the lines of this joint charge
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
    enum class PayGroup { User, Service, Apartment, Day, UserService, ApartmentUser };   // ApartmentUser key: "apt|user"
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
    virtual std::vector<JointServiceSummary> getJointServices() = 0;   // legacy: services shared by several clients

    // --- Installment plans (δόσεις) ---
    // A charge (a service, or a package's services) split into N installments every month/week. All lines are one
    // batch; installments not paid yet are 'future' charges with their due date as resolution date.
    // receiptMode "end": ONE receipt, issued automatically when the last installment is paid, covering all the money
    // of the plan (a receipt can only be for money received). "each": every installment gets its own receipt.
    struct PaymentPlan {
        int id = -1, batch_id = -1, installments = 0;
        std::string every, receipt_mode, label, created_at;
        double total = 0.0, paid = 0.0;
        int paidInstallments = 0;          // installments with nothing left to pay
        int receipt_id = -1;               // "end" mode: the receipt once issued
    };
    // lines: every charge of every installment, already dated (see the dialog). firstReceipt: receipt for money paid
    // now, used in "each" mode (in "end" mode only its method is kept on the money). Returns the plan id or -1.
    virtual int addInstallmentPlan(const std::vector<Payment>& lines, const std::string& label, int installments,
                                   const std::string& every, const std::string& receiptMode, const Receipt* firstReceipt) = 0;
    virtual PaymentPlan getPlanForBatch(int batchId) = 0;           // id -1 = the batch is not a plan

    // --- Paged lookups (everything assumes millions of rows: search + offset/limit in SQL) ---
    // text is matched accent/case-insensitively; sortCol/asc pick the ORDER BY.
    virtual std::vector<Building> pageBuildings(const std::string& text, int offset, int limit) = 0;   // address, area, ΤΚ, ΑΦΜ
    virtual int countBuildings(const std::string& text) = 0;
    struct PackageRow { MasterService pkg; int components = 0; double total = 0.0, totalCost = 0.0; std::string parts; };   // parts: "Α (10,00), Β (5,00)"
    // sort columns: 0 name, 1 services count, 2 total price, 3 total cost to us
    virtual std::vector<PackageRow> pagePackages(const std::string& text, const SortSpec& sort, int offset, int limit) = 0;   // sort columns: 0 name, 1 services, 2 total (several keys allowed)
    virtual int countPackages(const std::string& text) = 0;
    virtual std::vector<JointService> pageJointCatalog(const std::string& text, int offset, int limit, const SortSpec& sort = {}) = 0;   // sort columns: 0 name, 1 notes
    virtual int countJointCatalog(const std::string& text) = 0;
    // Joint charges, newest first by default. sortCol: 0 date, 1 building, 2 label, 3 kind, 4 total, 5 paid, 6 remaining,
    // 7 completion %, 8 charged, 9 excluded. text: label / building address. totals (optional): sum of total / paid.
    virtual std::vector<JointOp> pageJointOps(int buildingId, const std::string& text, const SortSpec& sort, int offset, int limit, bool onlyOpen = false,
                                              const std::vector<int>& serviceIds = {}, const std::string& fromDate = "", const std::string& toDate = "") = 0;
    virtual int countJointOps(int buildingId, const std::string& text, double* total = nullptr, double* paid = nullptr, bool onlyOpen = false,
                              const std::vector<int>& serviceIds = {}, const std::string& fromDate = "", const std::string& toDate = "") = 0;   // onlyOpen: not fully collected yet
    // Clients who bought services of a package. show: 0 all, 1 whole package, 2 part. sortCol: 0 name, 1 services, 3 charged,
    // 4 paid, 5 remaining.
    struct PackageBuyer { int user_id = -1; std::string name, parts; int count = 0; double due = 0, paid = 0; };
    virtual std::vector<PackageBuyer> pagePackageBuyers(int pkgId, const std::string& text, int show, const SortSpec& sort, int offset, int limit) = 0;
    virtual int countPackageBuyers(int pkgId, const std::string& text, int show) = 0;
    // The clients living now in a building's apartments (for charging a building).
    virtual std::vector<User> getBuildingResidents(int buildingId) = 0;
    virtual std::string getPackageLabel(int pkgId) = 0;                 // "" if none
    virtual JointService getJointServiceById(int id) = 0;               // id -1 if none

    // --- Offers (προσφορές) ---
    virtual std::vector<Offer> getOffers() = 0;                                    // all, newest first, with uses
    virtual int saveOffer(const Offer& o) = 0;                                     // insert (id -1) or update; id or -1
    virtual bool cancelOffer(int id, const std::string& reason) = 0;               // reason required
    // active offers that fit a charge: the client (or everyone), the service or package (or all), and the date
    // active offers for a client (his own and the ones for everyone) that have not ended by `date`, soonest end first
    virtual std::vector<Offer> getClientOffers(int userId, const std::string& date) = 0;
    virtual std::vector<Offer> findOffers(int userId, int serviceId, int packageId, const std::string& date) = 0;

    // --- Report queries ---
    struct MethodTotal { std::string method; long long count = 0; double total = 0.0, receipted = 0.0; };
    // money received (direction in) between two dates (inclusive, "" = open), per payment method
    virtual std::vector<MethodTotal> moneyByMethod(const std::string& fromDate, const std::string& toDate) = 0;
    struct DebtRow { int user_id = -1; double upTo30 = 0, upTo90 = 0, over90 = 0, total = 0; std::string oldest; };
    // what each client still owes (resolved, collections, balance > 0) by age of the charge on `asOf`
    virtual std::vector<DebtRow> debtAging(const std::string& asOf) = 0;

    // --- Κοινόχρηστα: a cost split across a collection of apartments (see models/Joint.h) ---
    virtual std::vector<JointService> getJointCatalog(bool includeInactive = false) = 0;
    virtual int addJointService(const Service& s, const std::string& kind) = 0;            // new id or -1
    virtual bool editJointService(int id, const Service& s, const std::string& kind, bool active) = 0;
    // Saves the operation, its shares (included and excluded) and one charge per included share and line,
    // all or nothing. Refused unless: total > 0, the lines add up to the total, at least one share is
    // included, included shares have a client and a positive amount and add up to the total exactly (cents),
    // and every excluded share has a reason. Returns the new operation id, -1 + takeLastError() otherwise.
    // costTotal > 0: what the job cost US (paid out at once). It is recorded as paid-out lines in a batch of its own,
    // whose id is kept in the operation's params ("cost_batch"); the lines are spread over `lines` by their totals.
    virtual int chargeJoint(const JointOp& op, const std::vector<JointLine>& lines, const std::vector<JointShare>& shares,
                            double costTotal = 0.0, int costPartnerId = -1, double costPaid = -1.0, const Receipt* receipt = nullptr) = 0;
    virtual double getJointOpCost(int opId) = 0;     // what the operation cost us (0 = not recorded)
    virtual std::vector<JointOp> getJointOps(int buildingId, int offset, int limit) = 0;    // buildingId -1 = all, newest first
    virtual int countJointOps(int buildingId) = 0;
    virtual JointOp getJointOpById(int id) = 0;
    virtual std::vector<JointShare> getJointShares(int opId) = 0;
    virtual std::vector<int> getJointOpCharges(int opId) = 0;
    // A client's (userId) or an apartment's (apartmentId) shares across all joint charges, newest first,
    // excluded ones too. Pass -1 for the side you don't filter on.
    virtual std::vector<JointShare> getJointSharesFor(int userId, int apartmentId) = 0;


    // --- What things cost US (service default cost, per-charge cost lines) and the profit figures ---
    // A charge may come with a cost: a paid-out line (direction "out") of the same client and service, recorded in the
    // same action. Profit = what we charge - what it cost us. Everything is before taxes.
    virtual double getServiceCost(int serviceId) = 0;                   // default cost, 0 = not set
    virtual bool setServiceCost(int serviceId, double cost) = 0;        // 0 clears it
    // Money relations with the business partners: what we paid / still owe each of them for the jobs of our costs.
    // jobs/cost/paid are limited to the dates (charge dates, "" = open); owedNow is everything still unpaid, any date.
    struct PartnerStat { int partner_id = -1; std::string name, trade, lastJob; bool active = true; int jobs = 0; double cost = 0, paid = 0, owedNow = 0; };
    virtual std::vector<PartnerStat> partnerStats(const std::string& fromDate, const std::string& toDate) = 0;
    // Every cost line paid to a partner (partnerId <= 0 = all partners), newest first. A cost of a joint charge carries
    // the joint operation it belongs to (jointOpId / jointLabel); a cost of a client's service carries the client.
    struct PartnerJob {
        int charge_id = -1, partner_id = -1; std::string partner, date, service, client, status, notes;
        double amount = 0, paid = 0, balance = 0;
        int jointOpId = -1; std::string jointLabel;
        std::string kind;                 // "joint" | "package" | "service": what the cost belonged to
    };
    // sort columns: 0 date, 1 partner, 2 service, 3 for whom, 4 amount, 5 paid, 6 still owed, 7 kind (several keys allowed)
    // kind: 0 all, 1 the clients' own services (and packages), 2 joint charges
    virtual std::vector<PartnerJob> partnerJobs(int partnerId, const std::string& fromDate, const std::string& toDate, const SortSpec& sort, int offset, int limit, int kind = 0, bool owedOnly = false) = 0;
    virtual int countPartnerJobs(int partnerId, const std::string& fromDate = "", const std::string& toDate = "", int kind = 0, bool owedOnly = false) = 0;
    // A client's own services (not their parts of joint charges), one row per charge, with what they paid and what it cost us.
    // sort columns: 0 date, 1 service, 2 charged, 3 paid, 4 owed, 5 our cost, 6 paid on our cost, 7 profit (several keys allowed)
    struct ClientChargeRow {
        int charge_id = -1; std::string date, service, status, offerName;
        double due = 0, paid = 0, balance = 0, cost = 0, costPaid = 0;
        double profit() const { return due - cost; }
    };
    // userId 0 = every client; serviceIds limits to those services (a service, or the services of a package)
    virtual std::vector<ClientChargeRow> pageClientCharges(int userId, const std::string& fromDate, const std::string& toDate, const SortSpec& sort, int offset, int limit, const std::vector<int>& serviceIds = {}) = 0;
    virtual int countClientCharges(int userId, const std::string& fromDate, const std::string& toDate, const std::vector<int>& serviceIds = {}) = 0;
    // The money side of one charge: what it brought in and what it cost us, paid to whom (a service can have several cost lines).
    // For a client's service: its cost lines (same action, same service); for a joint share: the operation's cost lines;
    // for a cost line: itself and the income line it belongs to.
    struct ChargeEconomics { int incomeChargeId = -1; double incomeDue = 0, incomePaid = 0; std::vector<PartnerJob> costs; double costDue() const { double s = 0; for (auto& c : costs) s += c.amount; return s; } double costPaid() const { double s = 0; for (auto& c : costs) s += c.paid; return s; } };
    virtual ChargeEconomics chargeEconomics(int chargeId) = 0;
    // Shares of joint charges of one client (userId > 0) or one apartment (apartmentId > 0), by charge date. onlyOpen: shares
    // still owed by the resident OR with a cost we still owe -- completed ones are not listed. sort columns: 0 date, 1 charge,
    // 2 resident, 3 building total, 4 share, 5 paid, 6 owed.
    virtual std::vector<JointShare> pageShares(int userId, int apartmentId, const std::string& fromDate, const std::string& toDate, bool onlyOpen, const SortSpec& sort, int offset, int limit) = 0;
    virtual int countShares(int userId, int apartmentId, const std::string& fromDate, const std::string& toDate, bool onlyOpen) = 0;
    // The viewer of a service / package: its clients (own, not joint charges), what it brought and cost, and who we paid for it.
    // sort columns: 0 client, 1 charges, 2 services, 3 charged, 4 paid, 5 owed, 6 our cost, 7 profit
    struct EntityClientRow { int user_id = -1; std::string name; int charges = 0, services = 0; double due = 0, paid = 0, balance = 0, cost = 0, costPaid = 0; double profit() const { return due - cost; } };
    virtual std::vector<EntityClientRow> entityClients(const std::vector<int>& serviceIds, const std::string& fromDate, const std::string& toDate, const SortSpec& sort, int offset, int limit) = 0;
    virtual int countEntityClients(const std::vector<int>& serviceIds, const std::string& fromDate, const std::string& toDate) = 0;
    virtual std::vector<PartnerStat> entityPartners(const std::vector<int>& serviceIds, const std::string& fromDate, const std::string& toDate) = 0;
    struct HomeFigures;
    struct HomeFigures {
        long long charges = 0;                  // income charges in the range
        double charged = 0, collected = 0;      // income: charged / already received
        double owed = 0;                        // income still owed (balance of those charges)
        double cost = 0, costPaid = 0;          // cost lines (paid-out charges): cost / already paid
        double profit() const { return charged - cost; }          // before anything is collected
        double cashProfit() const { return collected - costPaid; }
    };
    // scope 0 = the clients' own services, 1 = the buildings (joint charges); dates are charge dates, inclusive, "" = open.
    virtual HomeFigures homeFigures(int scope, const std::string& fromDate, const std::string& toDate) = 0;
    struct CashNow {                            // the whole business, not limited by dates
        double moneyIn = 0, moneyOut = 0;       // money actually received / paid out
        double owedIn = 0, owedOut = 0;         // still to receive / still to pay
        double net() const { return moneyIn - moneyOut; }
        double expected() const { return net() + owedIn - owedOut; }
    };
    virtual CashNow cashNow() = 0;
    // charged / collected / owed and our cost for some services, for the joint charges (joint = true) or the clients' own
    virtual HomeFigures entityFigures(const std::vector<int>& serviceIds, bool joint, const std::string& fromDate, const std::string& toDate) = 0;
    // Clients with open charges of their OWN services (joint shares are the building's business). sort columns:
    // 0 name, 1 owing, 2 open charges, 3 oldest charge.
    struct ClientOwing { int user_id = -1; std::string name; double owing = 0; int openCharges = 0; std::string oldest; };
    virtual std::vector<ClientOwing> pageClientOwings(const std::string& text, const SortSpec& sort, int offset, int limit, bool joint = false) = 0;
    virtual int countClientOwings(const std::string& text, double* total = nullptr, bool joint = false) = 0;
    // How well each client pays and what we make on them (Προσφορές). One row per client with any resolved charge.
    // *Client = their own services, *Joint = their shares of joint charges (κοινόχρηστα). Days are charge date -> date the
    // charge was fully paid, weighted by the amount; a charge not fully paid does not count toward the days.
    struct QualityRow {
        int user_id = -1; std::string name;
        double income = 0, cost = 0;                  // own services: charged, and what it cost us
        double paidAmtClient = 0, daysClient = 0;     // fully paid own charges: amount, amount-weighted sum of days
        double paidAmtJoint = 0, daysJoint = 0;       // same for joint shares
        double openClient = 0, openJoint = 0;         // still owed
        double incomeJoint = 0;                       // what they were charged as their parts of joint charges
        // Payment speed index, 0-100, per block: every payment made on a charge counts with its OWN amount and delay
        // (weight 2^(-days/30): paid at once = 1, after a month = 1/2...), and what is still unpaid counts as if it were
        // paid 30 days from today. index = 100 * sum(amount * weight) / sum(amount), over ALL payments, not an average of averages.
        double idxNumClient = 0, idxDenClient = 0, idxNumJoint = 0, idxDenJoint = 0;
        static double Index(double num, double den) { return den > 0.005 ? 100.0 * num / den : -1.0; }   // -1 = nothing to rate
        int chargesClient = 0, chargesJoint = 0;
    };
    // userId 0 = every client; dates (charge dates, inclusive, "" = open) limit what is rated
    virtual std::vector<QualityRow> clientQuality(int userId = 0, const std::string& fromDate = "", const std::string& toDate = "") = 0;

    // --- Συνεργάτες (people who do jobs) ---
    // sort columns: 0 name, 1 trade, 2 phone, 3 email, 4 active
    virtual std::vector<Partner> pagePartners(const std::string& text, bool activeOnly, const SortSpec& sort, int offset, int limit) = 0;
    virtual int countPartners(const std::string& text, bool activeOnly) = 0;
    virtual int savePartner(const Partner& p) = 0;        // insert (id -1) or update; the id, or -1 (+ takeLastError())
    virtual bool removePartner(int id) = 0;               // hides the partner (his jobs stay); see the archive
    struct PartnerRow { Partner p; int jobs = 0; double owed = 0; std::string last; };
    // Partners with their figures, one page at a time (the "#-1" line for payments with no partner included).
    // sort columns: 0 name, 1 trade, 2 phone, 3 email, 4 jobs, 5 owed now, 6 last job
    virtual std::vector<PartnerRow> pagePartnerRows(const std::string& text, const SortSpec& sort, int offset, int limit) = 0;
    virtual int countPartnerRows(const std::string& text) = 0;

    // --- Deleted things. Nothing is ever lost: a deleted service / partner / note / client is only HIDDEN (the row stays);
    // a deleted charge / building / apartment is removed from the live tables but its full record is kept here as JSON.
    // Both show up in the archive, searchable, e.g. for tax questions about the past.
    struct DeletionRow { int id = -1; std::string entity, label, deleted_at, restored_at, snapshot; int record_id = -1; bool hidden = false; };
    // sort columns: 0 date, 1 kind, 2 title, 3 status
    virtual std::vector<DeletionRow> pageDeletions(const std::string& text, const SortSpec& sort, int offset, int limit) = 0;
    virtual int countDeletions(const std::string& text) = 0;
    virtual bool restoreDeletion(int id) = 0;             // only for hidden ones (the row still exists)
    virtual Partner getPartner(int id) = 0;               // id < 0 when it does not exist

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
    // missingReceiptOnly: only lines whose incoming money has no issued receipt.
    virtual std::vector<Payment> pagePayments(const std::string& startDate, const std::string& endDate,
                                               int userId, int serviceId,
                                               const std::string& status, const std::string& direction,
                                               int offset, int limit, const SortSpec& sort = {},
                                               bool missingReceiptOnly = false) = 0;
    virtual int countPayments(const std::string& startDate, const std::string& endDate,
                               int userId, int serviceId,
                               const std::string& status, const std::string& direction,
                               bool missingReceiptOnly = false) = 0;

};
