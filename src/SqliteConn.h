#pragma once
#include "DataConn.h"
#include <sqlite3.h>
#include <string>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>

class SqliteConn : public DataConn {
private:
    sqlite3* db = nullptr;
    std::map<std::string, std::vector<std::string>> greekHeaders;

    bool executeQuery(const std::string& sql);
    std::string lastError_;
    std::string dbPath_;
    int apartmentAt(int userId, int y, int m, int d);
    double priceOf(int serviceId);
    double costOf(int serviceId);
    std::string toJson(const std::vector<std::string>& cols, const std::vector<std::string>& vals);
    std::vector<std::string> getColumnNames(const std::string& table);
    int insertPayment(const Payment& p, int batchId);
    // building blocks, used inside a savepoint by the public operations
    int insertCharge(const Payment& p, int batchId);
    int insertMoney(const MoneyPayment& m);
    bool allocate(int paymentId, int chargeId, double amount);
    bool reconcilePaid(int chargeId, double desiredPaid);
    bool savepoint(const char* name);
    bool closePlanIfPaid(int batchId);   // "end"-mode plan fully paid -> its one receipt
    bool release(const char* name);
    void rollbackTo(const char* name);
    int issueReceiptInTx(const Receipt& r, const std::vector<int>& paymentIds);   // caller owns the transaction
    void registerSqlFunctions();
    // (Re)creates the per-table audit triggers from the tables' current columns. Returns how many were
    // missing or altered (tampering signal once they have been installed).
    int ensureAuditTriggers();
    void writeAuditAnchor();
    // background full integrity check (own read-only connection), reported via takeLastError()
    std::thread bgVerify_;
    std::atomic<sqlite3*> bgDb_{nullptr};
    std::mutex bgMutex_;
    std::string bgMessage_;

public:
    std::string auditActor_;     // read by the app_actor() SQL function
    std::string auditReason_;    // read by the app_reason() SQL function
private:

public:
    SqliteConn(const std::string& dbPath);
    ~SqliteConn();

    // All interface methods...
    std::vector<User> searchUsers(const std::string& query, int offset, int limit) override;
    std::vector<User> advancedSearchUsers(const User& criteria, const std::string& idQuery = "") override;
    bool addUser(const User& user) override;
    bool editUser(int id, const User& user) override;
    bool removeUser(int id) override;
    std::string userDeletedAt(int id) override;
    User getUserById(int id) override;

    std::vector<Building> getBuildings() override;
    Building getBuildingById(int id) override;
    int addBuilding(const Building& building) override;
    bool editBuilding(int id, const Building& building) override;
    bool removeBuilding(int id) override;
    std::vector<Apartment> getApartmentsForBuilding(int buildingId) override;
    std::vector<Residency> getResidencies(int apartmentId) override;
    std::vector<Residency> getUserResidencies(int userId) override;
    bool setResidencyHistory(int userId, const std::vector<Residency>& history) override;
    std::vector<Payment> getPaymentsForApartment(int apartmentId) override;
    int getApartmentOnDate(int userId, int y, int m, int d) override { return apartmentAt(userId, y, m, d); }
    std::string takeLastError() override;
    std::string errorLogPath() override;
    int addInstallmentPlan(const std::vector<Payment>& lines, const std::string& label, int installments,
                           const std::string& every, const std::string& receiptMode, const Receipt* firstReceipt) override;
    PaymentPlan getPlanForBatch(int batchId) override;
    std::vector<Offer> getOffers() override;
    std::vector<Building> pageBuildings(const std::string& text, int offset, int limit) override;
    int countBuildings(const std::string& text) override;
    std::vector<PackageRow> pagePackages(const std::string& text, const SortSpec& sort, int offset, int limit) override;
    int countPackages(const std::string& text) override;
    std::vector<JointService> pageJointCatalog(const std::string& text, int offset, int limit, const SortSpec& sort = {}) override;
    int countJointCatalog(const std::string& text) override;
    std::vector<JointOp> pageJointOps(int buildingId, const std::string& text, const SortSpec& sort, int offset, int limit, bool onlyOpen = false,
                                      const std::vector<int>& serviceIds = {}, const std::string& fromDate = "", const std::string& toDate = "") override;
    int countJointOps(int buildingId, const std::string& text, double* total = nullptr, double* paid = nullptr, bool onlyOpen = false,
                      const std::vector<int>& serviceIds = {}, const std::string& fromDate = "", const std::string& toDate = "") override;
    std::vector<PackageBuyer> pagePackageBuyers(int pkgId, const std::string& text, int show, const SortSpec& sort, int offset, int limit) override;
    int countPackageBuyers(int pkgId, const std::string& text, int show) override;
    std::vector<User> getBuildingResidents(int buildingId) override;
    std::vector<int> getManagedBuildings() override;
    bool isBuildingManaged(int buildingId) override;
    bool setBuildingManaged(int buildingId, bool on) override;
    std::vector<Resident> getResidents(int buildingId, bool includeEnded) override;
    std::vector<Resident> getResidentStays(int userId) override;
    Resident getResidentById(int id) override;
    int residentUserFor(int anyUserId) override;
    int addResident(const Resident& r, int reuseOf = -1) override;
    bool endResident(int id, const std::string& endDate) override;
    bool editResident(int id, const std::string& name, const std::string& phone, const std::string& email, const std::string& note) override;
    std::vector<FundMove> residentFundMoves(int userId) override;
    std::vector<FundActionRow> pageFundActions(int buildingId, const SortSpec& sort, int offset, int limit) override;
    int countFundActions(int buildingId) override;
    std::vector<JointShare> pageShareRows(int opId, const std::vector<int>& userIds, const SortSpec& sort, int offset, int limit) override;
    int countShareRows(int opId, const std::vector<int>& userIds) override;
    ShareTotals shareTotals(int opId, const std::vector<int>& userIds) override;
    std::vector<AptResidentRow> pageAptResidents(int buildingId, bool previous, const std::string& text, const SortSpec& sort, int offset, int limit) override;
    int countAptResidents(int buildingId, bool previous, const std::string& text) override;
    std::vector<JointOp> pageFees(const std::string& fromDate, const std::string& toDate, const SortSpec& sort, int offset, int limit) override;
    int countFees(const std::string& fromDate, const std::string& toDate, double* charged = nullptr, double* collected = nullptr) override;
    std::vector<ManagedRow> pageManagedBuildings(const std::string& text, const SortSpec& sort, int offset, int limit) override;
    int countManagedBuildings(const std::string& text) override;
    MonthFund monthFund(int buildingId, const std::string& ym) override;
    std::vector<ResidentQualityRow> residentQuality(const std::string& fromDate, const std::string& toDate) override;
    int fundExpenseShared(int buildingId, double amount, const std::string& date, const std::string& description,
                          const std::string& note, const std::vector<JointShare>& shares) override;
    std::vector<JointShare> getExpenseShares(int moveId) override;
    MonthResidents monthResidents(int buildingId, const std::string& ym) override;
    double getFundBalance(int buildingId) override;
    FundTotals getFundTotals(int buildingId) override;
    std::vector<FundMove> pageFundMoves(int buildingId, int offset, int limit, bool expensesOnly = false) override;
    int countFundMoves(int buildingId, bool expensesOnly = false) override;
    FundMove getFundMove(int id) override;
    int fundExpense(int buildingId, double amount, const std::string& date, const std::string& description, int partnerId, const std::string& payee,
                    const std::string& note = "", int jointOpId = -1) override;
    int fundAdjust(int buildingId, double signedAmount, const std::string& date, const std::string& description, bool opening) override;
    int fundPayOurBill(int buildingId, int chargeId, double amount, const std::string& date) override;
    bool renameMasterService(int id, const std::string& label) override;
    std::string getPackageLabel(int pkgId) override;
    JointService getJointServiceById(int id) override;
    std::vector<MoneyPayment> getRecentMoney(int limit) override;
    int saveOffer(const Offer& o) override;
    bool cancelOffer(int id, const std::string& reason) override;
    std::vector<Offer> getClientOffers(int userId, const std::string& date) override;
    std::vector<Offer> findOffers(int userId, int serviceId, int packageId, const std::string& date) override;
    std::vector<MethodTotal> moneyByMethod(const std::string& fromDate, const std::string& toDate) override;
    std::vector<DebtRow> debtAging(const std::string& asOf) override;
    std::vector<JointService> getJointCatalog(bool includeInactive = false) override;
    int addJointService(const Service& s, const std::string& kind) override;
    bool editJointService(int id, const Service& s, const std::string& kind, bool active) override;
    int chargeJoint(const JointOp& op, const std::vector<JointLine>& lines, const std::vector<JointShare>& shares,
                    double costTotal = 0.0, int costPartnerId = -1, double costPaid = -1.0, const Receipt* receipt = nullptr) override;
    std::vector<PartnerJob> partnerJobs(int partnerId, const std::string& fromDate, const std::string& toDate, const SortSpec& sort, int offset, int limit, int kind = 0, bool owedOnly = false) override;
    int countPartnerJobs(int partnerId, const std::string& fromDate = "", const std::string& toDate = "", int kind = 0, bool owedOnly = false) override;
    std::vector<PartnerStat> partnerStats(const std::string& fromDate, const std::string& toDate) override;
    double getJointOpCost(int opId) override;
    double getServiceCost(int serviceId) override;
    bool setServiceCost(int serviceId, double cost) override;
    HomeFigures homeFigures(int scope, const std::string& fromDate, const std::string& toDate) override;
    CashNow cashNow() override;
    std::vector<ClientOwing> pageClientOwings(const std::string& text, const SortSpec& sort, int offset, int limit, bool joint = false) override;
    int countClientOwings(const std::string& text, double* total = nullptr, bool joint = false) override;
    std::vector<QualityRow> clientQuality(int userId = 0, const std::string& fromDate = "", const std::string& toDate = "") override;
    std::vector<ClientChargeRow> pageClientCharges(int userId, const std::string& fromDate, const std::string& toDate, const SortSpec& sort, int offset, int limit, const std::vector<int>& serviceIds = {}) override;
    int countClientCharges(int userId, const std::string& fromDate, const std::string& toDate, const std::vector<int>& serviceIds = {}) override;
    std::vector<EntityClientRow> entityClients(const std::vector<int>& serviceIds, const std::string& fromDate, const std::string& toDate, const SortSpec& sort, int offset, int limit) override;
    int countEntityClients(const std::vector<int>& serviceIds, const std::string& fromDate, const std::string& toDate) override;
    std::vector<PartnerStat> entityPartners(const std::vector<int>& serviceIds, const std::string& fromDate, const std::string& toDate) override;
    HomeFigures entityFigures(const std::vector<int>& serviceIds, bool joint, const std::string& fromDate, const std::string& toDate) override;
    ChargeEconomics chargeEconomics(int chargeId) override;
    std::vector<JointShare> pageShares(int userId, int apartmentId, const std::string& fromDate, const std::string& toDate, bool onlyOpen, const SortSpec& sort, int offset, int limit) override;
    int countShares(int userId, int apartmentId, const std::string& fromDate, const std::string& toDate, bool onlyOpen) override;
    std::vector<Partner> pagePartners(const std::string& text, bool activeOnly, const SortSpec& sort, int offset, int limit) override;
    int countPartners(const std::string& text, bool activeOnly) override;
    int savePartner(const Partner& p) override;
    bool removePartner(int id) override;
    std::vector<PartnerRow> pagePartnerRows(const std::string& text, const SortSpec& sort, int offset, int limit) override;
    int countPartnerRows(const std::string& text) override;
    std::vector<DeletionRow> pageDeletions(const std::string& text, const SortSpec& sort, int offset, int limit) override;
    int countDeletions(const std::string& text) override;
    bool restoreDeletion(int id) override;
    Partner getPartner(int id) override;
    std::vector<JointOp> getJointOps(int buildingId, int offset, int limit) override;
    int countJointOps(int buildingId) override;
    JointOp getJointOpById(int id) override;
    std::vector<JointShare> getJointShares(int opId) override;
    std::vector<int> getJointOpCharges(int opId) override;
    std::vector<JointShare> getJointSharesFor(int userId, int apartmentId) override;
    // Once a day: copy the database to <db folder>/backups/ (keeps the newest `keep`).
    void autoBackup(int keep = 14);
    int addApartment(int buildingId, const std::string& label) override;
    bool editApartment(int id, const std::string& label) override;
    bool removeApartment(int id) override;

    std::vector<Service> searchServices(const std::string& query, int offset, int limit) override;
    std::vector<Service> advancedSearchServices(const std::string& label, const std::string& extra_notes,
                                                 const std::string& idQuery = "") override;
    bool addService(int userId, const Service& service) override;
    int addServiceGetId(int userId, const Service& service) override;
    bool editService(int id, const Service& service) override;
    bool removeService(int id) override;
    Service getServiceById(int id) override;
    std::vector<Service> getServicesForUser(int userId) override;
    std::vector<User> getUsersForService(int serviceId) override;
    bool addServiceUser(int serviceId, int userId) override;

    std::vector<MasterService> getMasterServices() override;
    std::vector<Service> getMasterServiceComponents(int masterServiceId) override;
    int addMasterService(const std::string& label) override;
    bool addMasterServiceComponent(int masterServiceId, int serviceId) override;
    bool removeMasterServiceComponent(int masterServiceId, int serviceId) override;
    bool removeMasterService(int id) override;
    std::vector<DataConn::MasterServiceSummary> getMasterServiceSummaries() override;

    bool addPayment(const Payment& payment) override;
    int addPaymentBatch(const std::vector<Payment>& payments, const std::string& label) override;
    void setAuditActor(const std::string& actor) override { auditActor_ = actor; }
    void setAuditReason(const std::string& reason) override { auditReason_ = reason; }
    AuditCheck verifyAuditLog() override;
    bool editPayment(int id, const Payment& payment) override;
    bool payOutCharge(int chargeId, double amount, const std::string& payDate) override;
    bool removePayment(int id) override;
    Payment getPaymentById(int id) override;
    std::vector<Payment> getPaymentsForService(int serviceId) override;
    std::vector<Payment> getPaymentsForUser(int userId) override;
    std::vector<Payment> getFuturePayments() override;
    std::vector<Payment> getRecentPayments(int limit) override;
    std::vector<Payment> getAllPayments() override;
    std::vector<Payment> advancedSearchPayments(const std::string& year, const std::string& month, const std::string& day,
                                                 const std::string& amount_due_query, const std::string& amount_paid_query,
                                                 const std::string& status, const std::string& direction,
                                                 const std::string& notes, const std::string& resolution_date,
                                                 const std::string& userName, const std::string& serviceLabel,
                                                 const std::string& idQuery = "", const std::string& serviceIdQuery = "",
                                                 const std::string& userIdQuery = "") override;

    std::vector<std::string> getTimelineActions(const std::string& dateFilter, int offset, int limit,
                                                 const std::string& category = "", bool friendly = false) override;
    void logAction(const std::string& table, int recordId, const std::string& action, const std::string& oldData, const std::string& newData, const std::string& category = "db") override;

    double getTotalPaid(int year, int month, int userId = -1, int serviceId = -1) override;
    double getTotalPending(int year, int month, int userId = -1, int serviceId = -1) override;

    std::vector<Note> getAllNotes() override;
    bool addNote(const Note& note) override;
    bool editNote(int id, const Note& note) override;
    bool removeNote(int id) override;

    bool exportAllTablesToCSV(const std::string& folder) override;

    std::vector<std::vector<std::string>> getPaginatedData(const std::string& table, int offset, int limit) override;
    std::vector<std::string> getTableColumns(const std::string& table) override;
    int getRowCount(const std::string& table) override;



    std::vector<DataConn::JointServiceSummary> getJointServices() override;

    bool executeRawQuery(const std::string& sql, std::vector<std::string>& outColumns,
                          std::vector<std::vector<std::string>>& outRows, std::string& outError) override;

    std::vector<User> pageUsers(const std::string& query, int scope,
                                 const std::vector<std::string>& letters,
                                 int offset, int limit, const SortSpec& sort = {}, int kinds = 7) override;
    int countUsers(const std::string& query, int scope,
                    const std::vector<std::string>& letters, int kinds = 7) override;
    std::vector<Service> pageServices(const std::string& query, int offset, int limit,
                                       const SortSpec& sort = {}) override;
    int countServices(const std::string& query) override;
    std::vector<Payment> pagePayments(const std::string& startDate, const std::string& endDate,
                                       int userId, int serviceId,
                                       const std::string& status, const std::string& direction,
                                       int offset, int limit, const SortSpec& sort = {},
                                       bool missingReceiptOnly = false) override;
    int countPayments(const std::string& startDate, const std::string& endDate,
                       int userId, int serviceId,
                       const std::string& status, const std::string& direction,
                       bool missingReceiptOnly = false) override;

    int issueReceipt(const Receipt& r, const std::vector<int>& paymentIds) override;
    int addPaymentsWithReceipt(const std::vector<Payment>& payments, const std::string& batchLabel, const Receipt* receipt, bool receiptPerLine = false) override;
    int nextReceiptNumber(const std::string& series) override;
    int recordPayment(const MoneyPayment& money, const std::vector<Allocation>& allocations, const Receipt* receipt) override;
    std::vector<MoneyPayment> getMoneyForUser(int userId) override;
    std::vector<MoneyPayment> getMoneyForCharge(int chargeId) override;
    std::vector<Allocation> getAllocationsForMoney(int paymentId) override;
    std::vector<Payment> getOpenCharges(int userId) override;
    double getClientCredit(int userId) override;
    int issueReceiptForMoney(const Receipt& r, const std::vector<int>& moneyIds) override;
    std::vector<Receipt> searchReceipts(const ReceiptQuery& q, int offset, int limit) override;
    long long countReceipts(const ReceiptQuery& q, double* total = nullptr) override;
    std::vector<ReceiptLine> getReceiptLines(int receiptId) override;
    std::vector<MoneyPayment> getReceiptMoney(int receiptId) override;
    int addAttachment(const std::string& entity, int entityId, const std::string& sourcePath, const std::string& note) override;
    std::vector<Attachment> getAttachments(const std::string& entity, int entityId, bool includeRemoved = false) override;
    bool removeAttachment(int id, const std::string& reason) override;
    std::string attachmentFullPath(const Attachment& a) override;
    bool editPaymentWithReceipt(int id, const Payment& payment, const Receipt& receipt) override;
    bool voidReceipt(int id, const std::string& reason) override;
    Receipt getReceiptById(int id) override;
    std::vector<Receipt> getReceipts(int offset, int limit) override;
    std::vector<Receipt> getReceiptsForUser(int userId) override;
    std::vector<int> getReceiptPaymentIds(int receiptId) override;
    int getReceiptIdForPayment(int paymentId) override;
    std::vector<MoneyPayment> pageMoney(const MoneyQuery& q, int offset, int limit) override;
    long long countMoney(const MoneyQuery& q, double* in = nullptr, double* out = nullptr) override;

    std::vector<Payment> findPayments(const PaymentFilter& f, PayOrder order, int offset, int limit) override;
    long long countPaymentsWhere(const PaymentFilter& f) override;
    MoneyTotals paymentTotals(const PaymentFilter& f) override;
    std::vector<GroupedTotals> paymentTotalsBy(PayGroup group, const PaymentFilter& f) override;
    long long dataVersion() override { return (long long)sqlite3_total_changes64(db); }

    // Paged phone-book pickers (dashboard filters) -- see DataConn.h for contract.
    std::vector<User> pickerUsers(const std::vector<std::string>& letters,
                                   const std::string& nameSearch,
                                   int offset, int limit,
                                   bool reverseSort) override;
    int pickerUsersCount(const std::vector<std::string>& letters,
                          const std::string& nameSearch) override;
    std::vector<Service> pickerServices(const std::string& nameSearch,
                                         int offset, int limit) override;
    int pickerServicesCount(const std::string& nameSearch) override;
};
