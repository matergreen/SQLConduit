#include "sqlconduit/api.h"
#include "sqlconduit/client.h"
#include "sqlconduit/common/context.h"
#include "sqlconduit/common/oracle_types.h"
#include "sqlconduit/common/sql_analyze.h"
#include "sqlconduit/core/query_cache.h"
#include "sqlconduit/core/sql_auditor.h"
#include "sqlconduit/mapping.h"
#include "sqlconduit/sqlconduit.h"
#include "sqlconduit/util.h"
#include "sqlconduit/version.h"

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <type_traits>

#ifndef SQLCONDUIT_DEPRECATED
#error "SQLCONDUIT_DEPRECATED must be part of the installed API"
#endif

SQLCONDUIT_DEPRECATED("compile-time contract probe") void deprecatedContractProbe();

using sqlconduit::Client;
using sqlconduit::common::ErrorCode;
using sqlconduit::common::Params;
using sqlconduit::common::ResultSet;
using sqlconduit::common::Status;

static_assert(SQLCONDUIT_VERSION_MAJOR == 0);
static_assert(SQLCONDUIT_VERSION_MINOR == 7);
static_assert(SQLCONDUIT_VERSION_PATCH == 0);

static_assert(std::is_final_v<Client>);
static_assert(std::is_default_constructible_v<Client>);
static_assert(std::is_move_constructible_v<Client>);
static_assert(std::is_nothrow_move_constructible_v<Client>);
static_assert(!std::is_copy_constructible_v<Client>);
static_assert(!std::is_copy_assignable_v<Client>);

using InitPath = Status (Client::*)(const std::string &);
using InitConfig = Status (Client::*)(const sqlconduit::config::GlobalConfig &);
using QueryDefault = Status (Client::*)(const std::string &, ResultSet &) const;
using QueryNamed = Status (Client::*)(const std::string &, const std::string &,
                                      const Params &, ResultSet &) const;
using AsyncQuery = std::future<sqlconduit::async::QueryResult>
(Client::*)(const std::string &) const;
using AsyncTransaction = std::future<sqlconduit::async::OpResult>
(Client::*)(sqlconduit::core::SessionFn) const;
using Shutdown = void (Client::*)(std::chrono::milliseconds) noexcept;

static_assert(std::is_same_v<decltype(static_cast<InitPath>(&Client::init)), InitPath>);
static_assert(std::is_same_v<decltype(static_cast<InitConfig>(&Client::init)), InitConfig>);
static_assert(std::is_same_v<decltype(static_cast<QueryDefault>(&Client::query)), QueryDefault>);
static_assert(std::is_same_v<decltype(static_cast<QueryNamed>(&Client::query)), QueryNamed>);
static_assert(std::is_same_v<decltype(static_cast<AsyncQuery>(&Client::queryAsync)), AsyncQuery>);
static_assert(std::is_same_v<
    decltype(static_cast<AsyncTransaction>(&Client::transactionAsync)), AsyncTransaction>);
static_assert(std::is_same_v<decltype(static_cast<Shutdown>(&Client::shutdown)), Shutdown>);

static_assert(std::is_default_constructible_v<sqlconduit::core::DatabaseManager>);
static_assert(std::is_same_v<decltype(sqlconduit::core::QueryCache::stats()),
    sqlconduit::core::QueryCache::Stats>);
static_assert(std::is_same_v<decltype(sqlconduit::core::SqlAuditor::stats()),
    sqlconduit::core::SqlAuditor::Stats>);

static_assert(static_cast<int>(ErrorCode::Ok) == 0);
static_assert(static_cast<int>(ErrorCode::ConfigError) == 1);
static_assert(static_cast<int>(ErrorCode::ConnectionFailed) == 2);
static_assert(static_cast<int>(ErrorCode::QueryError) == 3);
static_assert(static_cast<int>(ErrorCode::QueryTimeout) == 4);
static_assert(static_cast<int>(ErrorCode::Cancelled) == 5);
static_assert(static_cast<int>(ErrorCode::ConstraintViolation) == 6);
static_assert(static_cast<int>(ErrorCode::Deadlock) == 7);
static_assert(static_cast<int>(ErrorCode::PingFailed) == 8);
static_assert(static_cast<int>(ErrorCode::TxError) == 9);
static_assert(static_cast<int>(ErrorCode::PoolExhausted) == 10);
static_assert(static_cast<int>(ErrorCode::PoolClosed) == 11);
static_assert(static_cast<int>(ErrorCode::CircuitOpen) == 12);
static_assert(static_cast<int>(ErrorCode::NotConnected) == 13);
static_assert(static_cast<int>(ErrorCode::DriverDisabled) == 14);
static_assert(static_cast<int>(ErrorCode::UnknownDriver) == 15);
static_assert(static_cast<int>(ErrorCode::NotSupported) == 16);
static_assert(static_cast<int>(ErrorCode::RateLimited) == 17);
static_assert(static_cast<int>(ErrorCode::SqlBlocked) == 18);
static_assert(static_cast<int>(ErrorCode::Buffered) == 19);
static_assert(static_cast<int>(ErrorCode::CursorClosed) == 20);
static_assert(static_cast<int>(ErrorCode::CursorLimit) == 21);
static_assert(static_cast<int>(ErrorCode::CursorError) == 22);
static_assert(static_cast<int>(ErrorCode::Unknown) == 23);
static_assert(static_cast<int>(ErrorCode::Overloaded) == 24);
static_assert(static_cast<int>(ErrorCode::MappingError) == 25);
static_assert(static_cast<int>(ErrorCode::IoError) == 26);
static_assert(static_cast<int>(ErrorCode::NotInitialized) == 27);
static_assert(static_cast<int>(ErrorCode::AlreadyInitialized) == 28);
static_assert(static_cast<int>(ErrorCode::ClientClosed) == 29);

static_assert(static_cast<int>(sqlconduit::common::OperationType::Query) == 0);
static_assert(static_cast<int>(sqlconduit::common::OperationType::Routine) == 10);
static_assert(static_cast<int>(sqlconduit::common::LogLevel::Debug) == 0);
static_assert(static_cast<int>(sqlconduit::common::LogLevel::Error) == 3);
static_assert(static_cast<int>(sqlconduit::common::Idempotency::Unspecified) == 0);
static_assert(static_cast<int>(sqlconduit::common::Idempotency::NonIdempotent) == 2);
static_assert(static_cast<int>(sqlconduit::common::IsolationLevel::Default) == 0);
static_assert(static_cast<int>(sqlconduit::common::IsolationLevel::Serializable) == 4);
static_assert(static_cast<int>(sqlconduit::common::ParamDirection::In) == 0);
static_assert(static_cast<int>(sqlconduit::common::ParamDirection::InOut) == 2);
static_assert(static_cast<int>(sqlconduit::common::ValueType::Auto) == 0);
static_assert(static_cast<int>(sqlconduit::common::ValueType::RefCursor) == 17);
static_assert(static_cast<int>(sqlconduit::common::OracleTypeClass::Unknown) == 0);
static_assert(static_cast<int>(sqlconduit::common::OracleTypeClass::Rowid) == 9);
static_assert(static_cast<int>(sqlconduit::common::sql::StatementKind::Unknown) == 0);
static_assert(static_cast<int>(sqlconduit::common::sql::StatementKind::Other) == 6);
static_assert(static_cast<int>(sqlconduit::mapping::ExtraColumns::Ignore) == 0);
static_assert(static_cast<int>(sqlconduit::mapping::MissingColumns::Error) == 1);
static_assert(static_cast<int>(sqlconduit::mapping::WriteCols::PrimaryKey) == 2);
static_assert(static_cast<unsigned>(sqlconduit::mapping::FieldFlags::None) == 0u);
static_assert(static_cast<unsigned>(sqlconduit::mapping::FieldFlags::Textual) == 16u);
static_assert(static_cast<int>(sqlconduit::common::util::RoutineKind::Function) == 0);
static_assert(static_cast<int>(sqlconduit::common::util::RoutineKind::Procedure) == 1);
static_assert(static_cast<int>(sqlconduit::common::util::Dialect::Auto) == 0);
static_assert(static_cast<int>(sqlconduit::common::util::Dialect::Oracle) == 4);
static_assert(static_cast<int>(sqlconduit::core::Cursor::Binding::OwnsHandle) == 0);
static_assert(static_cast<int>(sqlconduit::core::Cursor::Binding::BorrowedInSession) == 1);

int main() {
    return 0;
}
