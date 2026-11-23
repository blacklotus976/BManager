# Graph Report - PropertyManager  (2026-11-23)

## Corpus Check
- 71 files · ~285,420 words
- Verdict: corpus is large enough that graph structure adds value.
- Unclassified: 9 file(s) not represented in the graph (top: .bat 3, (none) 2, .zip 2)

## Summary
- 3089 nodes · 8309 edges · 170 communities (158 shown, 12 thin omitted)
- Extraction: 86% EXTRACTED · 14% INFERRED · 0% AMBIGUOUS · INFERRED: 1165 edges (avg confidence: 0.85)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `8377cc49`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- DataConn
- string
- App.cpp
- ToCents
- vector
- vector
- SqliteConn
- DrawPager
- main
- UiTest.cpp
- Style
- DrawDashboardScreen
- main
- Money.h
- seedDemoDataIfEmpty
- MonthlyBill
- ListExportCtx
- Payment
- JointChargeState
- impl_
- MonthPerson
- DrawHistoryScreen
- SqliteConn::SqliteConn
- QualityRow
- Contract
- Pager
- MonthFund
- PaymentFilter
- Axes
- main.cpp
- SqliteConn.cpp
- Resident
- PartnerJob
- FundMove
- User
- DrawCube
- Face
- MediaViewer.cpp
- BuildingPartner
- ReceiptQuery
- ResidentQualityRow
- Receipt
- Page
- BManager — Architecture Review & Roadmap
- JointShare
- ListModel
- NavIcon
- SqResults
- Partner
- Residency
- StoreMove
- Offer
- Service
- Sha256
- UserColId
- algorithm
- CubeKey
- AppState
- ManageScore
- MoneyPayment
- ErrorLog.h
- ClientChargeRow
- CubeHit
- JointServiceSummary
- AptAgg
- StoreItem
- 1. Showing an edited payment (crucial)
- PartnerDoc
- Attachment
- Seed.cpp
- ImageView
- Distribution
- FundAction
- ProfileState
- BuildingPartnerLine
- FundActionRow
- MoneyTotals
- ModeState
- RowScreenRect
- BManager — Roadmap / TODO
- PaymentPlan
- ProfitRow
- SqContext
- SqMRow
- Rf.h
- EntityClientRow
- HomeFigures
- ManagedRow
- PackageRow
- TextBuf
- BuildingAgg
- SqARow
- Κοινόχρηστα — joint charges
- PendingConfirm
- CubeCell
- ExpenseShortcut
- PartnerStat
- RunApp
- QcPages
- ServiceItem
- DeletionRow
- ResidentOwing
- Service
- Blob
- QualityView
- seed_demo.cpp
- .Heat
- MoneyQuery
- CubeProf
- EntityState
- GraphNode
- TableGeo
- Promotions, offers and contracts — design (not implemented yet)
- UserColDef
- BillSuggestion
- CashNow
- ContractTotals
- DebtRow
- FundTotals
- IncomeSplit
- PackageBuyer
- PayGroup
- StoreMonth
- ColGroup
- OpenOwingRow
- ReceiptForm
- BManager — PropertyManager
- ClientOwing
- Note
- UsageState
- vector
- Installments (Πληρωμή σε δόσεις)
- BillReason
- ClientMix
- ContractItem
- ContractOwing
- MethodTotal
- ShareTotals
- HoveredRowLinks
- StaySeg
- main
- PayOrder
- BillWin
- NavItem
- SheetState
- JointRow
- string
- UI tests (PM_uitest)
- run.sh
- build_linux.sh
- XlsxWriter.h
- Cell
- Sheet
- ContractInstallment
- ScVerdict
- JointLine
- GridState
- DrawStorageScreen
- Allocation
- OfferDiscount
- JointDraft
- history_test.cpp
- ReceiptLine
- Has
- Plan
- Has

## God Nodes (most connected - your core abstractions)
1. `SqliteConn` - 364 edges
2. `DataConn` - 340 edges
3. `FromCents()` - 133 edges
4. `ToCents()` - 131 edges
5. `Eur()` - 120 edges
6. `CellText()` - 114 edges
7. `ColText()` - 82 edges
8. `main()` - 78 edges
9. `RunApp()` - 71 edges
10. `Payment` - 64 edges

## Surprising Connections (you probably didn't know these)
- `10. Money handling (implemented)` --references--> `MoneySub()`  [INFERRED]
  docs/ARCHITECTURE_REVIEW.md → src/Money.h
- `10. Money handling (implemented)` --references--> `ParseMoney()`  [INFERRED]
  docs/ARCHITECTURE_REVIEW.md → src/Money.h
- `10. Money handling (implemented)` --references--> `ToCents()`  [INFERRED]
  docs/ARCHITECTURE_REVIEW.md → src/Money.h
- `10. Money handling (implemented)` --references--> `FromCents()`  [INFERRED]
  docs/ARCHITECTURE_REVIEW.md → src/Money.h
- `10. Money handling (implemented)` --references--> `RoundMoney()`  [INFERRED]
  docs/ARCHITECTURE_REVIEW.md → src/Money.h

## Import Cycles
- None detected.

## Communities (170 total, 12 thin omitted)

### Community 1 - "string"
Cohesion: 0.05
Nodes (26): AddMonthsClamped(), AptResidentsFrom(), Bind, d, s, text, BuildUserPageWhere(), CanonSvcSql() (+18 more)

### Community 2 - "App.cpp"
Cohesion: 0.03
Nodes (140): AllDigits(), ApartmentName(), ApartmentOnDate(), AptDisplay(), AptOwing, owed, AxisLabel(), active (+132 more)

### Community 3 - "ToCents"
Cohesion: 0.12
Nodes (63): MoneySub(), ToCents(), BillData(), BpSetExtra(), BuildingPartnerCombo(), CardWithContracts(), CollectInstallments(), DateGr() (+55 more)

### Community 4 - "vector"
Cohesion: 0.05
Nodes (23): SortSpec, FromCents(), BindText(), C(), ColText(), SqliteConn::contractOwings(), SqliteConn::getAllPayments(), SqliteConn::getFuturePayments() (+15 more)

### Community 5 - "vector"
Cohesion: 0.09
Nodes (34): AddMoney4(), AptDateLabel(), AptTimeline(), BuildBuildings(), BuildingOpenJoints(), ColApply(), ColFilterPopup(), ColKey() (+26 more)

### Community 6 - "SqliteConn"
Cohesion: 0.04
Nodes (24): SqliteConn::getMasterServiceSummaries(), SqliteConn::getOffers(), ReadOffers(), SqliteConn, auditActor_, auditReason_, bgDb_, bgMessage_ (+16 more)

### Community 7 - "DrawPager"
Cohesion: 0.11
Nodes (82): FoldGreek(), BuildingClientId(), CellText(), Cmp3(), CmpText(), DrawAptResidentsTab(), DrawArchiveWindow(), DrawBillsClients() (+74 more)

### Community 8 - "main"
Cohesion: 0.09
Nodes (13): 10. Money handling (implemented), FormatMoney(), RoundMoney(), localtime_s(), SqliteConn::writeAuditAnchor(), FundDateOk(), PayDate(), TrimR() (+5 more)

### Community 9 - "UiTest.cpp"
Cohesion: 0.06
Nodes (51): OnFilesDropped(), Active(), AfterRender(), BeforeWindow(), Button(), Centre(), Chord(), Click() (+43 more)

### Community 10 - "Style"
Cohesion: 0.15
Nodes (13): Style, Date, Header, Int, Money, Muted, Number, SumInt (+5 more)

### Community 11 - "DrawDashboardScreen"
Cohesion: 0.60
Nodes (6): BeginCard(), DaysUntilDate(), DrawDashboardScreen(), DrawDaySectionHeader(), DrawTimelineRow(), EndCard()

### Community 12 - "main"
Cohesion: 0.10
Nodes (18): BindApartment(), SqliteConn::ensureFeeService(), MapUserRow(), ReadReceipt(), ReceiptKindSql(), ReceiptWhere(), userToJson(), main() (+10 more)

### Community 13 - "Money.h"
Cohesion: 0.18
Nodes (3): raw(), main(), Ago()

### Community 15 - "MonthlyBill"
Cohesion: 0.07
Nodes (31): BillShare, amount, apartment, apartment_id, balance, charges, name, paid (+23 more)

### Community 16 - "ListExportCtx"
Cohesion: 0.18
Nodes (11): ListExportCtx, cf, defs, distinct, enabled, frame, headerClick, order (+3 more)

### Community 17 - "Payment"
Cohesion: 0.07
Nodes (29): Payment, amount_due, amount_paid, apartment_id, balance, batch_id, contract_id, created_at (+21 more)

### Community 18 - "JointChargeState"
Cohesion: 0.07
Nodes (28): JointChargeState, buildingId, compCost, compPaid, compPaidManual, compPartner, comps, confirming (+20 more)

### Community 19 - "impl_"
Cohesion: 0.07
Nodes (21): impl_, clock, dec, duration, eof, fit, fmt, frame (+13 more)

### Community 20 - "MonthPerson"
Cohesion: 0.07
Nodes (27): MonthCell, apartment_id, charged, op, paid, person, MonthOp, date (+19 more)

### Community 21 - "DrawHistoryScreen"
Cohesion: 0.12
Nodes (28): DefaultFace(), DepthAx(), DrawHistoryScreen(), DrawSheetBar(), EaseTo(), FromQ(), GoToFocusAcrossTime(), Iso() (+20 more)

### Community 22 - "SqliteConn::SqliteConn"
Cohesion: 0.10
Nodes (20): AuditCheck, entries, ok, problems, ArchiveDeletion(), AuditCanonical(), AuditGenesis(), ColumnExists() (+12 more)

### Community 23 - "QualityRow"
Cohesion: 0.08
Nodes (24): QualityRow, chargesClient, chargesJoint, cost, daysClient, daysJoint, dueClient, dueJoint (+16 more)

### Community 25 - "Contract"
Cohesion: 0.08
Nodes (24): Contract, batch_id, cancelled, collected, cost, created_at, dates, end_date (+16 more)

### Community 26 - "Pager"
Cohesion: 0.18
Nodes (6): Pager, autoFit, autoKey, offset, pageSize, total

### Community 27 - "MonthFund"
Cohesion: 0.09
Nodes (23): MonthFund, feeOwedEnd, feeOwedStart, feePriorPaidIn, fundEnd, fundStart, items, otherIn (+15 more)

### Community 28 - "PaymentFilter"
Cohesion: 0.09
Nodes (23): PaymentFilter, apartmentId, buildingText, clientText, direction, fromDate, joint, jointOpId (+15 more)

### Community 29 - "Axes"
Cohesion: 0.16
Nodes (14): Axes, n, s, si, u, ui, BuildAxes(), BuildBlobs() (+6 more)

### Community 30 - "main.cpp"
Cohesion: 0.21
Nodes (17): ConfigPath(), DatabaseFileExists(), DirNextToExe(), IsAbsolutePath(), IsEmptyDb(), IsFruitsDb(), IsSmallDb(), main() (+9 more)

### Community 31 - "SqliteConn.cpp"
Cohesion: 0.06
Nodes (19): BindAll(), BuildingToJson(), BuildPaymentPageWhere(), BuildPayWhere(), SqliteConn::cashNow(), SqliteConn::getBuildings(), SqliteConn::refreshDueCharges(), SqliteConn::registerSqlFunctions() (+11 more)

### Community 32 - "Resident"
Cohesion: 0.09
Nodes (21): AptResidentRow, apartment, apartment_id, owed, r, Resident, apartment, apartment_id (+13 more)

### Community 33 - "PartnerJob"
Cohesion: 0.09
Nodes (20): ChargeEconomics, costs, incomeChargeId, incomeDue, incomePaid, PartnerJob, amount, balance (+12 more)

### Community 34 - "FundMove"
Cohesion: 0.10
Nodes (21): FundMove, amount, apartment, apartment_id, balance, building_id, charge_id, created_at (+13 more)

### Community 35 - "User"
Cohesion: 0.10
Nodes (20): User, address, afm, apartment, apartment_id, area, building_id, client_building_id (+12 more)

### Community 36 - "DrawCube"
Cohesion: 0.15
Nodes (27): CellBox(), DrawAxes(), DrawCube(), DrawFaceGrid(), DrawFlight(), DrawHL(), DrawNavIcon(), DrawSliceFrames() (+19 more)

### Community 37 - "Face"
Cohesion: 0.10
Nodes (21): Face, allM, allQ, C, colAx, csm, csq, depth (+13 more)

### Community 38 - "MediaViewer.cpp"
Cohesion: 0.09
Nodes (9): Clock(), VideoView::Available(), VideoView::Close(), VideoView::IsOpen(), DrawZoomable(), OpenWithSystem(), RevealInFolder(), UploadRGBA() (+1 more)

### Community 39 - "BuildingPartner"
Cohesion: 0.10
Nodes (20): BuildingPartner, active, address, afm, building, building_id, created_at, email (+12 more)

### Community 40 - "ReceiptQuery"
Cohesion: 0.10
Nodes (20): ReceiptCond, Field, Building, Client, Method, Package, Service, id (+12 more)

### Community 41 - "ResidentQualityRow"
Cohesion: 0.10
Nodes (19): ResidentQualityRow, active, apartment, building, building_id, delayDen, delayNum, due (+11 more)

### Community 42 - "Receipt"
Cohesion: 0.10
Nodes (18): Receipt, bank_ref, building_id, created_at, id, issue_date, kind, method (+10 more)

### Community 43 - "Page"
Cohesion: 0.10
Nodes (21): Page, PAGE_APARTMENTS, PAGE_BUILDINGS, PAGE_CASH, PAGE_CLIENTS, PAGE_CONTRACTS, PAGE_DASHBOARD, PAGE_HISTORY (+13 more)

### Community 44 - "BManager — Architecture Review & Roadmap"
Cohesion: 0.11
Nodes (18): 11. The client's business (expresshomeservice.gr, crawled 2026-10-11), 12. Receipts (implemented, migration v4), 13. Extensibility: oil (litres) and future service types without schema churn, 14. State after the v7/v8 migrations (2026-10-11), 15. State after migrations v9–v10, 1. What the client actually needs, 2. What exists today (good parts), 3. Problems found (verified on a freshly seeded DB) (+10 more)

### Community 45 - "JointShare"
Cohesion: 0.05
Nodes (39): JointOp, batch_id, building_address, building_id, created_at, excluded, id, included (+31 more)

### Community 46 - "ListModel"
Cohesion: 0.15
Nodes (14): Cached, key, val, ver, ListModel, allCache, cache, distinct (+6 more)

### Community 47 - "NavIcon"
Cohesion: 0.11
Nodes (19): NavIcon, IC_BOLT, IC_BOX, IC_BUILDING, IC_CHART, IC_COINS, IC_CONTRACT, IC_CUBE (+11 more)

### Community 48 - "SqResults"
Cohesion: 0.11
Nodes (17): SqResults, apartments, buildings, mgmt, notes, offers, packages, partners (+9 more)

### Community 49 - "Partner"
Cohesion: 0.11
Nodes (17): PartnerRow, jobs, last, owed, p, Partner, active, afm (+9 more)

### Community 50 - "Residency"
Cohesion: 0.11
Nodes (17): Apartment, building_id, id, label, Building, address, afm, area (+9 more)

### Community 51 - "StoreMove"
Cohesion: 0.14
Nodes (14): StoreMove, charge_id, client, date, id, item_id, kind, notes (+6 more)

### Community 52 - "Offer"
Cohesion: 0.12
Nodes (16): Offer, cancel_reason, created_at, given, id, kind, name, note (+8 more)

### Community 53 - "Service"
Cohesion: 0.12
Nodes (6): SqliteConn::getAllNotes(), SqliteConn::getJointServices(), NotHiddenSql(), ServiceOrderBy(), serviceToJson(), main()

### Community 54 - "Sha256"
Cohesion: 0.19
Nodes (7): Sha256, buf_, h_, len_, used_, Sha256Hex(), FileSha256()

### Community 55 - "UserColId"
Cohesion: 0.12
Nodes (17): UserColId, UCOL_ADDR, UCOL_AFM, UCOL_APT, UCOL_AREA, UCOL_CODE, UCOL_COUNT, UCOL_CREATED (+9 more)

### Community 56 - "algorithm"
Cohesion: 0.15
Nodes (10): DatePickerState, hasSelection, navMonth, navYear, selDay, selMonth, selYear, DayOfWeekLocal() (+2 more)

### Community 57 - "CubeKey"
Cohesion: 0.18
Nodes (11): CubeKey, address, area, code, id, money, name, phone (+3 more)

### Community 58 - "AppState"
Cohesion: 0.14
Nodes (11): AppState, conn, dbPath, goneNames, jointServices, notes, reloadGen, serviceIdx (+3 more)

### Community 59 - "ManageScore"
Cohesion: 0.12
Nodes (16): ManageScore, active, activeOwed, address, avgCollectDays, avgRefillDays, bills, building_id (+8 more)

### Community 60 - "MoneyPayment"
Cohesion: 0.12
Nodes (15): MoneyPayment, allocated, amount, batch_id, created_at, direction, id, method (+7 more)

### Community 61 - "ErrorLog.h"
Cohesion: 0.22
Nodes (9): CsvField(), Path(), ReadAll(), Row, message, source, time, SetFolder() (+1 more)

### Community 62 - "ClientChargeRow"
Cohesion: 0.13
Nodes (14): ClientChargeRow, balance, charge_id, client, contract, cost, costPaid, date (+6 more)

### Community 63 - "CubeHit"
Cohesion: 0.13
Nodes (15): CubeHit, client, date, due, id, kind, label, paid (+7 more)

### Community 64 - "JointServiceSummary"
Cohesion: 0.14
Nodes (13): JointServiceSummary, service, totalDue, totalNet, totalPaid, userNet, users, userTotalDue (+5 more)

### Community 65 - "AptAgg"
Cohesion: 0.13
Nodes (15): AptAgg, apt, collected, former, formerUserIds, owedIn, owedOut, paidOut (+7 more)

### Community 66 - "StoreItem"
Cohesion: 0.15
Nodes (13): StoreItem, bought, code, id, last_move, name, notes, partner_ids (+5 more)

### Community 67 - "1. Showing an edited payment (crucial)"
Cohesion: 0.14
Nodes (13): 1.1 In every list (Πληρωμές, timeline, client ledger), 1.2 Payment view window → new tab **«Ιστορικό αλλαγών»**, 1.3 Editing, 1.4 Towards the customer, 1. Showing an edited payment (crucial), 2.1 Receipt view, 2.2 Issuing, 2.3 Receipts tab (+5 more)

### Community 68 - "PartnerDoc"
Cohesion: 0.14
Nodes (14): PartnerDoc, amount, charge_id, date, id, method, notes, number (+6 more)

### Community 69 - "Attachment"
Cohesion: 0.14
Nodes (13): Attachment, created_at, entity, entity_id, file_name, id, kind, note (+5 more)

### Community 70 - "Seed.cpp"
Cohesion: 0.13
Nodes (11): Bundle, name, parts, CatSvc, amount, label, notes, pkg (+3 more)

### Community 71 - "ImageView"
Cohesion: 0.18
Nodes (9): ImageView, fitPending_, h_, panX_, panY_, rot_, tex_, w_ (+1 more)

### Community 72 - "Distribution"
Cohesion: 0.14
Nodes (13): BuildingFund, charged, collected, cost, costPaid, open, ops, BuildingFunds() (+5 more)

### Community 73 - "FundAction"
Cohesion: 0.14
Nodes (14): FundAction, balance, charged, collected, date, in, isOut, kindLabel (+6 more)

### Community 74 - "ProfileState"
Cohesion: 0.14
Nodes (14): ProfileState, charges, from, jfrom, jto, offers, owed, owedJ (+6 more)

### Community 75 - "BuildingPartnerLine"
Cohesion: 0.15
Nodes (13): BuildingPartnerLine, amount, bill_id, building, building_id, created_at, paid_date, partner (+5 more)

### Community 76 - "FundActionRow"
Cohesion: 0.15
Nodes (13): FundActionRow, balance, charged, collected, date, in, label, moveId (+5 more)

### Community 77 - "MoneyTotals"
Cohesion: 0.15
Nodes (13): GroupedTotals, key, t, MoneyTotals, collected, count, dueAll, futureIn (+5 more)

### Community 78 - "ModeState"
Cohesion: 0.15
Nodes (12): ModeState, focusAx, focusId, hadBefore, inverse, selInit, svcBeforeInverse, svcKey (+4 more)

### Community 79 - "RowScreenRect"
Cohesion: 0.50
Nodes (4): RowScreenRect, center, key, table

### Community 80 - "BManager — Roadmap / TODO"
Cohesion: 0.17
Nodes (11): BManager — Roadmap / TODO, Key idea: every money line knows *what kind of money* it is, Later — Server, client portal, Viva Wallet, Open questions (client / accountant), Step 1 — Show the money honestly (UI first, tiny schema change), Step 2 — Tax details (needed before any real document), Step 3 — VAT on charges, Step 4 — Payment → right document (+3 more)

### Community 81 - "PaymentPlan"
Cohesion: 0.17
Nodes (12): PaymentPlan, batch_id, created_at, every, id, installments, label, paid (+4 more)

### Community 82 - "ProfitRow"
Cohesion: 0.17
Nodes (12): ProfitRow, avgDays, charged, clients, collected, cost, costOwed, costPaid (+4 more)

### Community 83 - "SqContext"
Cohesion: 0.17
Nodes (11): SqContext, aptMoves, aptsOf, buildings, currentResidents, mgmt, pastResidents, pkgsOfService (+3 more)

### Community 84 - "SqMRow"
Cohesion: 0.17
Nodes (12): SqMRow, amount, bid, blob, building, date, key, kind (+4 more)

### Community 85 - "Rf.h"
Cohesion: 0.40
Nodes (7): Clean(), IsValidRfCode(), MakeRfCode(), Mod97(), NormalizeRfCode(), lastPaymentId(), main()

### Community 86 - "EntityClientRow"
Cohesion: 0.18
Nodes (10): EntityClientRow, balance, charges, cost, costPaid, due, name, paid (+2 more)

### Community 87 - "HomeFigures"
Cohesion: 0.18
Nodes (9): HomeFigures, charged, charges, collected, cost, costPaid, owed, scheduledIn (+1 more)

### Community 88 - "ManagedRow"
Cohesion: 0.18
Nodes (11): ManagedRow, address, afm, apartments, building_id, email, fund, owed (+3 more)

### Community 89 - "PackageRow"
Cohesion: 0.18
Nodes (10): PackageRow, components, parts, pkg, total, totalCost, MasterService, created_at (+2 more)

### Community 90 - "TextBuf"
Cohesion: 0.18
Nodes (10): AptNewResident, email, from, name, phone, ExtraRow, k, v (+2 more)

### Community 91 - "BuildingAgg"
Cohesion: 0.18
Nodes (11): BuildingAgg, active, apts, b, collected, displayName, owedIn, owedOut (+3 more)

### Community 92 - "SqARow"
Cohesion: 0.18
Nodes (9): SqARow, a, b, current, moves, SqBRow, apts, b (+1 more)

### Community 93 - "Κοινόχρηστα — joint charges"
Cohesion: 0.20
Nodes (9): Attachments (photos, videos, PDFs): how they are stored, Error log, Kinds (split logic) — add new ones without changing the database, One charge, many views — nothing counted twice, Residents over time (no overlaps), The rule, What gets stored (migration v11), Where it is in the app (+1 more)

### Community 94 - "PendingConfirm"
Cohesion: 0.18
Nodes (11): DrawConfirmModal(), FieldDiff, field, newVal, oldVal, PendingConfirm, diffs, onConfirm (+3 more)

### Community 95 - "CubeCell"
Cohesion: 0.13
Nodes (15): CubeCell, heat_money, heat_qty, money, qty, rank_money, rank_qty, service_id (+7 more)

### Community 96 - "ExpenseShortcut"
Cohesion: 0.20
Nodes (10): ExpenseShortcut, amount, building_id, id, label, last_used, partner, partner_id (+2 more)

### Community 97 - "PartnerStat"
Cohesion: 0.20
Nodes (10): PartnerStat, active, cost, jobs, lastJob, name, owedNow, paid (+2 more)

### Community 98 - "RunApp"
Cohesion: 0.06
Nodes (76): ParseMoney(), AddPeriodIso(), BeginGuardedModal(), BuildPlanLines(), CardContractsSwitch(), ChargeLabel(), ColLoad(), ColSaveIfChanged() (+68 more)

### Community 99 - "QcPages"
Cohesion: 0.24
Nodes (6): QcPages, done, key, kPage, rows, started

### Community 100 - "ServiceItem"
Cohesion: 0.33
Nodes (6): ServiceItem, code, item_id, name, qty, unit_cost

### Community 101 - "DeletionRow"
Cohesion: 0.22
Nodes (9): DeletionRow, deleted_at, entity, hidden, id, label, record_id, restored_at (+1 more)

### Community 102 - "ResidentOwing"
Cohesion: 0.22
Nodes (9): ResidentOwing, apartment, building, building_id, name, owed, to, user_id (+1 more)

### Community 103 - "Service"
Cohesion: 0.22
Nodes (8): Service, code, cost_price, created_at, extra_notes, id, label, suggested_price

### Community 104 - "Blob"
Cohesion: 0.22
Nodes (9): Blob, base, colM, colQ, half, hi, lo, rM (+1 more)

### Community 105 - "QualityView"
Cohesion: 0.22
Nodes (9): QualityView, blkJ, blkS, blkT, from, g, more, q (+1 more)

### Community 106 - "seed_demo.cpp"
Cohesion: 0.36
Nodes (5): Day(), Equal(), main(), NextMonth(), Rnd()

### Community 107 - ".Heat"
Cohesion: 0.48
Nodes (5): SqlAppActor(), SqlAppReason(), SqlAuditHash(), SqlFold(), SqlHeat()

### Community 108 - "MoneyQuery"
Cohesion: 0.25
Nodes (8): MoneyQuery, direction, fromDate, method, oursOnly, receipt, text, toDate

### Community 109 - "CubeProf"
Cohesion: 0.25
Nodes (8): CubeProf, build, builds, draw, frame, frames, load, loads

### Community 110 - "EntityState"
Cohesion: 0.25
Nodes (8): EntityState, charges, clients, from, ops, selClient, selOp, to

### Community 111 - "GraphNode"
Cohesion: 0.25
Nodes (8): GraphNode, cols, offset, pos, rows, table, total, vel

### Community 112 - "TableGeo"
Cohesion: 0.25
Nodes (8): TableGeo, cmax, cmin, cw, ok, rh, x0, y0

### Community 113 - "Promotions, offers and contracts — design (not implemented yet)"
Cohesion: 0.29
Nodes (6): 1. Principles, 2. Tables, 3. How a charge gets its price, 4. UI, 5. Phasing, Promotions, offers and contracts — design (not implemented yet)

### Community 114 - "UserColDef"
Cohesion: 0.29
Nodes (6): UserColDef, name, pinned, residence, value, width_flag

### Community 115 - "BillSuggestion"
Cohesion: 0.29
Nodes (7): BillSuggestion, charge_id, contract, date, due, label, owed

### Community 116 - "CashNow"
Cohesion: 0.33
Nodes (5): CashNow, moneyIn, moneyOut, owedIn, owedOut

### Community 117 - "ContractTotals"
Cohesion: 0.18
Nodes (9): ColFilters, in, text, ContractTotals, collected, count, owedNow, scheduled (+1 more)

### Community 118 - "DebtRow"
Cohesion: 0.29
Nodes (7): DebtRow, oldest, over90, total, upTo30, upTo90, user_id

### Community 119 - "FundTotals"
Cohesion: 0.29
Nodes (7): FundTotals, collections, contributions, expenses, in, ourBills, out

### Community 120 - "IncomeSplit"
Cohesion: 0.29
Nodes (7): IncomeSplit, contracts, contractsOwed, contractsScheduled, fees, feesOwed, servicesOwed

### Community 121 - "PackageBuyer"
Cohesion: 0.29
Nodes (7): PackageBuyer, count, due, name, paid, parts, user_id

### Community 122 - "PayGroup"
Cohesion: 0.29
Nodes (7): PayGroup, Apartment, ApartmentUser, Day, Service, User, UserService

### Community 123 - "StoreMonth"
Cohesion: 0.40
Nodes (5): StoreMonth, bought, month, spent, used

### Community 124 - "ColGroup"
Cohesion: 0.29
Nodes (7): ColGroup, col, first, label, n, open, tip

### Community 125 - "OpenOwingRow"
Cohesion: 0.29
Nodes (7): OpenOwingRow, balance, charge_id, client, date, due, label

### Community 126 - "ReceiptForm"
Cohesion: 0.29
Nodes (7): ReceiptForm, bank, date, issue, method, paper, rf

### Community 127 - "BManager — PropertyManager"
Cohesion: 0.33
Nodes (5): BManager — PropertyManager, Build — Linux, Build — Windows (Visual Studio 2022 + vcpkg), Data, Docs

### Community 128 - "ClientOwing"
Cohesion: 0.33
Nodes (6): ClientOwing, name, oldest, openCharges, owing, user_id

### Community 130 - "Note"
Cohesion: 0.33
Nodes (5): Note, content, created_at, id, title

### Community 131 - "UsageState"
Cohesion: 0.33
Nodes (6): UsageState, from, money, pick, to, unit

### Community 132 - "vector"
Cohesion: 0.18
Nodes (9): Ago(), Cents(), main(), Need(), Rnd(), Ago(), Day(), NextMonth() (+1 more)

### Community 133 - "Installments (Πληρωμή σε δόσεις)"
Cohesion: 0.40
Nodes (4): Installments (Πληρωμή σε δόσεις), Paying the installments, The one receipt, What is stored (migration v12)

### Community 134 - "BillReason"
Cohesion: 0.40
Nodes (5): BillReason, amount, partner, partner_id, reason

### Community 135 - "ClientMix"
Cohesion: 0.40
Nodes (5): ClientMix, contracts, packages, services, user_id

### Community 136 - "ContractItem"
Cohesion: 0.40
Nodes (5): ContractItem, amount, cost, label, service_id

### Community 137 - "ContractOwing"
Cohesion: 0.40
Nodes (5): ContractOwing, late, name, owed, user_id

### Community 138 - "MethodTotal"
Cohesion: 0.40
Nodes (5): MethodTotal, count, method, receipted, total

### Community 139 - "ShareTotals"
Cohesion: 0.40
Nodes (5): ShareTotals, feeCharged, feePaid, fundCharged, fundPaid

### Community 140 - "HoveredRowLinks"
Cohesion: 0.40
Nodes (5): HoveredRowLinks, active, originKey, table, targetKeys

### Community 141 - "StaySeg"
Cohesion: 0.40
Nodes (5): StaySeg, current, from, to, user_id

### Community 142 - "main"
Cohesion: 0.70
Nodes (4): check(), main(), parses(), rejects()

### Community 143 - "PayOrder"
Cohesion: 0.50
Nodes (4): PayOrder, DateAsc, DateDesc, ResolutionDesc

### Community 144 - "BillWin"
Cohesion: 0.50
Nodes (4): BillWin, building, id, ym

### Community 145 - "NavItem"
Cohesion: 0.50
Nodes (4): NavItem, icon, label, page

### Community 146 - "SheetState"
Cohesion: 0.50
Nodes (4): SheetState, money, winM, winQ

### Community 148 - "JointRow"
Cohesion: 0.20
Nodes (9): JointRow, amount, apartment_id, apt_label, included, paid, reason, user_id (+1 more)

### Community 149 - "string"
Cohesion: 0.42
Nodes (5): Crc(), Zip, central, count, out

### Community 154 - "XlsxWriter.h"
Cohesion: 0.44
Nodes (7): Col(), Esc(), SheetName(), SheetXml(), Utf8Len(), Widen(), Write()

### Community 155 - "Cell"
Cohesion: 0.25
Nodes (8): Cell, isNum, num, style, text, N(), ParseShown(), T()

### Community 156 - "Sheet"
Cohesion: 0.25
Nodes (7): Sheet, filterRow, freezeCols, freezeRows, name, rows, widths

### Community 157 - "ContractInstallment"
Cohesion: 0.29
Nodes (7): ContractInstallment, balance, charges, date, due, future, paid

### Community 158 - "ScVerdict"
Cohesion: 0.38
Nodes (7): ClientVerdict(), FundVerdict(), ResidentVerdict(), ScJudge(), ScVerdict, lv, why

### Community 159 - "JointLine"
Cohesion: 0.33
Nodes (6): JointLine, cost, costPaid, partner, service_id, total

### Community 160 - "GridState"
Cohesion: 0.33
Nodes (6): GridState, cf, order, pager, q, ListFilterKey()

### Community 161 - "DrawStorageScreen"
Cohesion: 0.70
Nodes (5): CostCell(), DrawStorageScreen(), DrawStoreCard(), DrawStorePurchase(), QtyStr()

### Community 162 - "Allocation"
Cohesion: 0.50
Nodes (4): Allocation, amount, charge_id, payment_id

### Community 163 - "OfferDiscount"
Cohesion: 0.50
Nodes (3): OfferDiscount(), main(), Q1()

### Community 164 - "JointDraft"
Cohesion: 0.50
Nodes (3): JointDraft, rows, total

### Community 165 - "history_test.cpp"
Cohesion: 1.00
Nodes (3): lastId(), main(), pay()

### Community 166 - "ReceiptLine"
Cohesion: 0.67
Nodes (3): ReceiptLine, amount, charge

## Knowledge Gaps
- **1291 isolated node(s):** `masterService`, `components`, `componentRosters`, `Client`, `Service` (+1286 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 1690 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **12 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `DataConn` connect `DataConn` to `ClientOwing`, `BillReason`, `ClientMix`, `ContractItem`, `ContractOwing`, `MethodTotal`, `ShareTotals`, `DrawPager`, `seedDemoDataIfEmpty`, `MonthlyBill`, `PayOrder`, `MonthPerson`, `SqliteConn::SqliteConn`, `QualityRow`, `string`, `Contract`, `MonthFund`, `PaymentFilter`, `ContractInstallment`, `Axes`, `Resident`, `PartnerJob`, `FundMove`, `ReceiptLine`, `BuildingPartner`, `ReceiptQuery`, `ResidentQualityRow`, `Partner`, `StoreMove`, `Service`, `CubeKey`, `AppState`, `ManageScore`, `ClientChargeRow`, `CubeHit`, `JointServiceSummary`, `StoreItem`, `PartnerDoc`, `BuildingPartnerLine`, `FundActionRow`, `MoneyTotals`, `PaymentPlan`, `ProfitRow`, `EntityClientRow`, `HomeFigures`, `ManagedRow`, `PackageRow`, `CubeCell`, `ExpenseShortcut`, `PartnerStat`, `RunApp`, `ServiceItem`, `DeletionRow`, `ResidentOwing`, `MoneyQuery`, `BillSuggestion`, `CashNow`, `ContractTotals`, `DebtRow`, `FundTotals`, `IncomeSplit`, `PackageBuyer`, `PayGroup`, `StoreMonth`?**
  _High betweenness centrality (0.269) - this node is a cross-community bridge._
- **Are the 12 inferred relationships involving `SqliteConn` (e.g. with `main()` and `main()`) actually correct?**
  _`SqliteConn` has 12 INFERRED edges - model-reasoned connections that need verification._
- **What connects `masterService`, `components`, `componentRosters` to the rest of the system?**
  _1291 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `DataConn` be split into smaller, more focused modules?**
  _Cohesion score 0.008733624454148471 - nodes in this community are weakly interconnected._
- **Why does `SqliteConn` connect `SqliteConn` to `string`, `vector`, `history_test.cpp`, `vector`, `main`, `Receipt`, `main`, `Money.h`, `Service`, `SqliteConn::SqliteConn`, `Rf.h`, `ErrorLog.h`, `SqliteConn.cpp`?**
  _High betweenness centrality (0.059) - this node is a cross-community bridge._
- **Should `string` be split into smaller, more focused modules?**
  _Cohesion score 0.046992481203007516 - nodes in this community are weakly interconnected._
- **Why does `JointShare` connect `JointShare` to `vector`, `DrawPager`, `main`, `ProfileState`, `seed_demo.cpp`, `string`?**
  _High betweenness centrality (0.036) - this node is a cross-community bridge._