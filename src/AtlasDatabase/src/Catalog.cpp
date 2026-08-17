#include <AtlasDatabase/Catalog.h>

#include <sqlite3.h>

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace atlas {
namespace {

void check(int result, sqlite3* database, const char* operation) {
  if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW) {
    throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(database));
  }
}

class Statement {
 public:
  Statement(sqlite3* database, const char* sql) : database_(database) {
    check(sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr), database, "prepare SQL");
  }
  ~Statement() { sqlite3_finalize(statement_); }
  sqlite3_stmt* get() const { return statement_; }
 private:
  sqlite3* database_;
  sqlite3_stmt* statement_{};
};

std::int64_t milliseconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point fromMilliseconds(std::int64_t value) {
  return std::chrono::system_clock::time_point{std::chrono::milliseconds{value}};
}

ScanSession readScanSession(sqlite3_stmt* statement) {
  ScanSession session;
  session.id = sqlite3_column_int64(statement, 0);
  session.name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
  session.root = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
  session.driveScan = sqlite3_column_int(statement, 3) != 0;
  session.snapshot = sqlite3_column_int(statement, 4) != 0;
  if (sqlite3_column_type(statement, 5) != SQLITE_NULL) {
    session.sourceSessionId = sqlite3_column_int64(statement, 5);
  }
  session.dataVersion = static_cast<std::uint32_t>(sqlite3_column_int(statement, 6));
  session.organizationVersion = static_cast<std::uint32_t>(sqlite3_column_int(statement, 7));
  session.state = static_cast<ScanSessionState>(sqlite3_column_int(statement, 8));
  session.files = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 9));
  session.directories = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 10));
  session.bytes = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 11));
  session.inaccessible = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 12));
  session.startedAt = fromMilliseconds(sqlite3_column_int64(statement, 13));
  session.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 14));
  if (const auto* error = sqlite3_column_text(statement, 15)) {
    session.error = reinterpret_cast<const char*>(error);
  }
  session.pendingDirectories = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 16));
  return session;
}

constexpr auto scanSessionColumns =
    "s.id,s.name,s.root,s.drive_scan,s.is_snapshot,s.source_session_id,"
    "s.data_version,s.organization_version,s.state,s.files,s.directories,s.bytes,s.inaccessible,"
    "s.started_ms,s.updated_ms,s.error,"
    "(SELECT COUNT(*) FROM scan_directories d WHERE d.session_id=s.id AND d.completed=0)";

}  // namespace

struct Catalog::Impl {
  sqlite3* database{};
};

Catalog::Catalog(const std::filesystem::path& databasePath) : impl_(std::make_unique<Impl>()) {
  const auto filename = databasePath.string();
  if (sqlite3_open_v2(filename.c_str(), &impl_->database,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    const std::string message = impl_->database ? sqlite3_errmsg(impl_->database) : "unknown error";
    sqlite3_close(impl_->database);
    throw std::runtime_error("open catalog: " + message);
  }
  int version{};
  {
    Statement statement(impl_->database, "PRAGMA user_version");
    check(sqlite3_step(statement.get()), impl_->database, "read schema version");
    version = sqlite3_column_int(statement.get(), 0);
  }
  constexpr auto schema = R"sql(
    PRAGMA foreign_keys = ON;
    PRAGMA journal_mode = WAL;
    PRAGMA synchronous = NORMAL;
    PRAGMA busy_timeout = 5000;
    CREATE TABLE IF NOT EXISTS assets (
      id INTEGER PRIMARY KEY,
      path TEXT NOT NULL UNIQUE,
      size_bytes INTEGER NOT NULL CHECK(size_bytes >= 0),
      modified_ms INTEGER NOT NULL,
      is_directory INTEGER NOT NULL CHECK(is_directory IN (0, 1)),
      is_symlink INTEGER NOT NULL DEFAULT 0 CHECK(is_symlink IN (0, 1))
    );
    CREATE INDEX IF NOT EXISTS assets_modified_idx ON assets(modified_ms);
    CREATE INDEX IF NOT EXISTS assets_size_idx ON assets(size_bytes);
    CREATE INDEX IF NOT EXISTS assets_path_nocase_idx ON assets(path COLLATE NOCASE);
    CREATE TABLE IF NOT EXISTS bookmarks (
      path TEXT PRIMARY KEY,
      added_ms INTEGER NOT NULL
    );
  )sql";
  char* error{};
  if (sqlite3_exec(impl_->database, schema, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "unknown error";
    sqlite3_free(error);
    throw std::runtime_error("initialize catalog: " + message);
  }
  if (version == 1) {
    check(sqlite3_exec(impl_->database,
        "ALTER TABLE assets ADD COLUMN is_symlink INTEGER NOT NULL DEFAULT 0 CHECK(is_symlink IN (0,1))",
        nullptr, nullptr, nullptr), impl_->database, "migrate catalog to version 2");
  }
  constexpr auto scanSchema = R"sql(
    CREATE TABLE IF NOT EXISTS scan_sessions (
      id INTEGER PRIMARY KEY,
      root TEXT NOT NULL,
      drive_scan INTEGER NOT NULL CHECK(drive_scan IN (0,1)),
      state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),
      files INTEGER NOT NULL DEFAULT 0,
      directories INTEGER NOT NULL DEFAULT 0,
      bytes INTEGER NOT NULL DEFAULT 0,
      inaccessible INTEGER NOT NULL DEFAULT 0,
      started_ms INTEGER NOT NULL,
      updated_ms INTEGER NOT NULL,
      error TEXT NOT NULL DEFAULT ''
    );
    CREATE INDEX IF NOT EXISTS scan_sessions_updated_idx ON scan_sessions(updated_ms DESC);
    CREATE TABLE IF NOT EXISTS scan_directories (
      session_id INTEGER NOT NULL REFERENCES scan_sessions(id) ON DELETE CASCADE,
      path TEXT NOT NULL,
      completed INTEGER NOT NULL DEFAULT 0 CHECK(completed IN (0,1)),
      PRIMARY KEY(session_id,path)
    );
    CREATE INDEX IF NOT EXISTS scan_directories_pending_idx
      ON scan_directories(session_id,completed,path);
    CREATE TABLE IF NOT EXISTS scan_assets (
      session_id INTEGER NOT NULL REFERENCES scan_sessions(id) ON DELETE CASCADE,
      asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
      PRIMARY KEY(session_id,asset_id)
    );
    CREATE INDEX IF NOT EXISTS scan_assets_session_idx ON scan_assets(session_id);
  )sql";
  check(sqlite3_exec(impl_->database, scanSchema, nullptr, nullptr, nullptr),
        impl_->database, "initialize persistent scans");
  if (version < 4) {
    constexpr auto snapshotMigration = R"sql(
      ALTER TABLE scan_sessions ADD COLUMN name TEXT NOT NULL DEFAULT '';
      ALTER TABLE scan_sessions ADD COLUMN is_snapshot INTEGER NOT NULL DEFAULT 0
        CHECK(is_snapshot IN (0,1));
      ALTER TABLE scan_sessions ADD COLUMN source_session_id INTEGER;
      ALTER TABLE scan_sessions ADD COLUMN data_version INTEGER NOT NULL DEFAULT 1;
      ALTER TABLE scan_sessions ADD COLUMN organization_version INTEGER NOT NULL DEFAULT 0;
      UPDATE scan_sessions SET name=root WHERE name='';
      CREATE TABLE IF NOT EXISTS scan_snapshot_assets (
        session_id INTEGER NOT NULL REFERENCES scan_sessions(id) ON DELETE CASCADE,
        asset_id INTEGER NOT NULL,
        path TEXT NOT NULL,
        size_bytes INTEGER NOT NULL,
        modified_ms INTEGER NOT NULL,
        is_directory INTEGER NOT NULL CHECK(is_directory IN (0,1)),
        is_symlink INTEGER NOT NULL CHECK(is_symlink IN (0,1)),
        PRIMARY KEY(session_id,path)
      );
      CREATE INDEX IF NOT EXISTS scan_snapshot_assets_session_idx
        ON scan_snapshot_assets(session_id);
      PRAGMA user_version=4;
    )sql";
    check(sqlite3_exec(impl_->database, snapshotMigration, nullptr, nullptr, nullptr),
          impl_->database, "migrate catalog to named scan snapshots");
  }
}

Catalog::~Catalog() { if (impl_) sqlite3_close(impl_->database); }
Catalog::Catalog(Catalog&&) noexcept = default;
Catalog& Catalog::operator=(Catalog&&) noexcept = default;

AssetId Catalog::upsert(const AssetMetadata& metadata) {
  Statement statement(impl_->database, R"sql(
    INSERT INTO assets(path, size_bytes, modified_ms, is_directory, is_symlink) VALUES(?, ?, ?, ?, ?)
    ON CONFLICT(path) DO UPDATE SET size_bytes=excluded.size_bytes,
      modified_ms=excluded.modified_ms, is_directory=excluded.is_directory,
      is_symlink=excluded.is_symlink
  )sql");
  const auto path = normalizedPath(metadata.path);
  check(sqlite3_bind_text(statement.get(), 1, path.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind path");
  check(sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(metadata.sizeBytes)), impl_->database, "bind size");
  check(sqlite3_bind_int64(statement.get(), 3, milliseconds(metadata.modifiedAt)), impl_->database, "bind time");
  check(sqlite3_bind_int(statement.get(), 4, metadata.isDirectory ? 1 : 0), impl_->database, "bind type");
  check(sqlite3_bind_int(statement.get(), 5, metadata.isSymlink ? 1 : 0), impl_->database, "bind symlink");
  check(sqlite3_step(statement.get()), impl_->database, "upsert asset");
  Statement identifier(impl_->database, "SELECT id FROM assets WHERE path=?");
  check(sqlite3_bind_text(identifier.get(), 1, path.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind path");
  check(sqlite3_step(identifier.get()), impl_->database, "read asset id");
  return sqlite3_column_int64(identifier.get(), 0);
}

std::vector<AssetId> Catalog::upsertBatch(const std::vector<AssetMetadata>& metadata) {
  char* error{};
  if (sqlite3_exec(impl_->database, "BEGIN IMMEDIATE", nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "unknown error";
    sqlite3_free(error);
    throw std::runtime_error("begin catalog batch: " + message);
  }
  std::vector<AssetId> ids;
  ids.reserve(metadata.size());
  try {
    for (const auto& asset : metadata) ids.push_back(upsert(asset));
    check(sqlite3_exec(impl_->database, "COMMIT", nullptr, nullptr, nullptr), impl_->database, "commit catalog batch");
  } catch (...) {
    sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
  return ids;
}

std::optional<CatalogAsset> Catalog::findByPath(const std::filesystem::path& path) const {
  Statement statement(impl_->database,
      "SELECT id, path, size_bytes, modified_ms, is_directory, is_symlink FROM assets WHERE path=?");
  const auto normalized = normalizedPath(path);
  check(sqlite3_bind_text(statement.get(), 1, normalized.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind path");
  const int result = sqlite3_step(statement.get());
  if (result == SQLITE_DONE) return std::nullopt;
  check(result, impl_->database, "find asset");
  CatalogAsset asset;
  asset.id = sqlite3_column_int64(statement.get(), 0);
  asset.metadata.path = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
  asset.metadata.sizeBytes = static_cast<std::uintmax_t>(sqlite3_column_int64(statement.get(), 2));
  asset.metadata.modifiedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 3));
  asset.metadata.isDirectory = sqlite3_column_int(statement.get(), 4) != 0;
  asset.metadata.isSymlink = sqlite3_column_int(statement.get(), 5) != 0;
  return asset;
}

std::vector<CatalogAsset> Catalog::search(const AssetQuery& query) const {
  std::string sql = "SELECT id,path,size_bytes,modified_ms,is_directory,is_symlink FROM assets WHERE 1=1";
  std::vector<std::string> textValues;
  std::vector<sqlite3_int64> numberValues;
  if (!query.text.empty()) { sql += " AND path LIKE ? COLLATE NOCASE"; textValues.push_back("%" + query.text + "%"); }
  if (query.extension) {
    sql += " AND path LIKE ? COLLATE NOCASE";
    textValues.push_back("%" + (query.extension->starts_with('.') ? *query.extension : "." + *query.extension));
  }
  if (query.beneath) {
    sql += " AND path LIKE ? COLLATE NOCASE";
    auto root = normalizedPath(*query.beneath);
    textValues.push_back(root + (root.ends_with('/') ? "%" : "/%"));
  }
  if (query.minimumSize) { sql += " AND size_bytes>=?"; numberValues.push_back(static_cast<sqlite3_int64>(*query.minimumSize)); }
  if (query.maximumSize) { sql += " AND size_bytes<=?"; numberValues.push_back(static_cast<sqlite3_int64>(*query.maximumSize)); }
  if (query.modifiedAfter) { sql += " AND modified_ms>=?"; numberValues.push_back(milliseconds(*query.modifiedAfter)); }
  sql += " ORDER BY is_directory DESC, path COLLATE NOCASE LIMIT ? OFFSET ?";
  Statement statement(impl_->database, sql.c_str());
  int parameter = 1;
  for (const auto& value : textValues) check(sqlite3_bind_text(statement.get(), parameter++, value.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind search text");
  for (auto value : numberValues) check(sqlite3_bind_int64(statement.get(), parameter++, value), impl_->database, "bind search number");
  check(sqlite3_bind_int64(statement.get(), parameter++, static_cast<sqlite3_int64>(query.limit)), impl_->database, "bind limit");
  check(sqlite3_bind_int64(statement.get(), parameter, static_cast<sqlite3_int64>(query.offset)), impl_->database, "bind offset");
  std::vector<CatalogAsset> result;
  int step{};
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    CatalogAsset asset;
    asset.id = sqlite3_column_int64(statement.get(), 0);
    asset.metadata.path = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
    asset.metadata.sizeBytes = static_cast<std::uintmax_t>(sqlite3_column_int64(statement.get(), 2));
    asset.metadata.modifiedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 3));
    asset.metadata.isDirectory = sqlite3_column_int(statement.get(), 4) != 0;
    asset.metadata.isSymlink = sqlite3_column_int(statement.get(), 5) != 0;
    result.push_back(std::move(asset));
  }
  check(step, impl_->database, "search assets");
  return result;
}

std::vector<CatalogAsset> Catalog::largestFiles(std::size_t limit) const {
  Statement statement(impl_->database, "SELECT id,path,size_bytes,modified_ms,is_directory,is_symlink FROM assets WHERE is_directory=0 ORDER BY size_bytes DESC LIMIT ?");
  check(sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(limit)), impl_->database, "bind largest limit");
  std::vector<CatalogAsset> result;
  int step{};
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    CatalogAsset asset;
    asset.id = sqlite3_column_int64(statement.get(), 0);
    asset.metadata.path = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
    asset.metadata.sizeBytes = static_cast<std::uintmax_t>(sqlite3_column_int64(statement.get(), 2));
    asset.metadata.modifiedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 3));
    asset.metadata.isDirectory = sqlite3_column_int(statement.get(), 4) != 0;
    asset.metadata.isSymlink = sqlite3_column_int(statement.get(), 5) != 0;
    result.push_back(std::move(asset));
  }
  check(step, impl_->database, "read largest files");
  return result;
}

CatalogStatistics Catalog::statistics() const {
  Statement statement(impl_->database, "SELECT COALESCE(SUM(is_directory=0),0),COALESCE(SUM(is_directory=1),0),COALESCE(SUM(CASE WHEN is_directory=0 THEN size_bytes ELSE 0 END),0) FROM assets");
  check(sqlite3_step(statement.get()), impl_->database, "catalog statistics");
  return {static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0)),
          static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 1)),
          static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 2))};
}

std::uint64_t Catalog::removeMissingUnder(const std::filesystem::path& root,
                                          const std::vector<std::filesystem::path>& observedPaths) {
  std::unordered_set<std::string> observed;
  observed.reserve(observedPaths.size());
  for (const auto& path : observedPaths) observed.insert(normalizedPath(path));
  AssetQuery query;
  query.beneath = root;
  query.limit = static_cast<std::size_t>(assetCount() + 1);
  const auto existing = search(query);
  std::uint64_t removed{};
  Statement statement(impl_->database, "DELETE FROM assets WHERE path=?");
  for (const auto& asset : existing) {
    const auto path = normalizedPath(asset.metadata.path);
    if (observed.contains(path)) continue;
    sqlite3_reset(statement.get());
    sqlite3_clear_bindings(statement.get());
    check(sqlite3_bind_text(statement.get(), 1, path.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind stale path");
    check(sqlite3_step(statement.get()), impl_->database, "remove stale asset");
    ++removed;
  }
  return removed;
}

void Catalog::addBookmark(const std::filesystem::path& path) {
  Statement statement(impl_->database, "INSERT OR REPLACE INTO bookmarks(path,added_ms) VALUES(?,?)");
  const auto value = normalizedPath(path);
  check(sqlite3_bind_text(statement.get(), 1, value.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind bookmark");
  check(sqlite3_bind_int64(statement.get(), 2, milliseconds(std::chrono::system_clock::now())), impl_->database, "bind bookmark time");
  check(sqlite3_step(statement.get()), impl_->database, "add bookmark");
}

void Catalog::removeBookmark(const std::filesystem::path& path) {
  Statement statement(impl_->database, "DELETE FROM bookmarks WHERE path=?");
  const auto value = normalizedPath(path);
  check(sqlite3_bind_text(statement.get(), 1, value.c_str(), -1, SQLITE_TRANSIENT), impl_->database, "bind bookmark");
  check(sqlite3_step(statement.get()), impl_->database, "remove bookmark");
}

std::vector<std::filesystem::path> Catalog::bookmarks() const {
  Statement statement(impl_->database, "SELECT path FROM bookmarks ORDER BY added_ms");
  std::vector<std::filesystem::path> result;
  int step{};
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    result.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0)));
  }
  check(step, impl_->database, "read bookmarks");
  return result;
}

ScanSessionId Catalog::createScanSession(const std::filesystem::path& root, bool driveScan) {
  const auto path = normalizedPath(root);
  auto name = root.filename().string();
  if (name.empty()) name = path;
  const auto now = milliseconds(std::chrono::system_clock::now());
  Statement session(impl_->database, R"sql(
    INSERT INTO scan_sessions(name,root,drive_scan,state,started_ms,updated_ms)
    VALUES(?,?,?,?,?,?)
  )sql");
  check(sqlite3_bind_text(session.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT),
        impl_->database, "bind scan name");
  check(sqlite3_bind_text(session.get(), 2, path.c_str(), -1, SQLITE_TRANSIENT),
        impl_->database, "bind scan root");
  check(sqlite3_bind_int(session.get(), 3, driveScan ? 1 : 0), impl_->database,
        "bind scan kind");
  check(sqlite3_bind_int(session.get(), 4, static_cast<int>(ScanSessionState::Pending)),
        impl_->database, "bind scan state");
  check(sqlite3_bind_int64(session.get(), 5, now), impl_->database, "bind scan start");
  check(sqlite3_bind_int64(session.get(), 6, now), impl_->database, "bind scan update");
  check(sqlite3_step(session.get()), impl_->database, "create scan session");
  const auto id = sqlite3_last_insert_rowid(impl_->database);
  Statement pending(impl_->database,
      "INSERT INTO scan_directories(session_id,path,completed) VALUES(?,?,0)");
  check(sqlite3_bind_int64(pending.get(), 1, id), impl_->database, "bind scan session");
  check(sqlite3_bind_text(pending.get(), 2, path.c_str(), -1, SQLITE_TRANSIENT),
        impl_->database, "bind initial scan directory");
  check(sqlite3_step(pending.get()), impl_->database, "queue initial scan directory");
  return id;
}

ScanSessionId Catalog::saveScanSnapshot(ScanSessionId sourceId, std::string name) {
  if (name.empty()) throw std::invalid_argument("saved scan name is required");
  check(sqlite3_exec(impl_->database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr),
        impl_->database, "begin scan snapshot");
  try {
    const auto now = milliseconds(std::chrono::system_clock::now());
    Statement snapshot(impl_->database, R"sql(
      INSERT INTO scan_sessions(
        name,root,drive_scan,is_snapshot,source_session_id,data_version,
        organization_version,state,files,directories,bytes,inaccessible,
        started_ms,updated_ms,error)
      SELECT ?,s.root,s.drive_scan,1,s.id,s.data_version,0,3,
        COALESCE(SUM(CASE WHEN a.is_directory=0 THEN 1 ELSE 0 END),0),
        COALESCE(SUM(CASE WHEN a.is_directory=1 THEN 1 ELSE 0 END),0),
        COALESCE(SUM(CASE WHEN a.is_directory=0 THEN a.size_bytes ELSE 0 END),0),
        s.inaccessible,?,?,''
      FROM scan_sessions s
      LEFT JOIN scan_assets sa ON sa.session_id=s.id
      LEFT JOIN assets a ON a.id=sa.asset_id
      WHERE s.id=? GROUP BY s.id
    )sql");
    check(sqlite3_bind_text(snapshot.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT),
          impl_->database, "bind saved scan name");
    check(sqlite3_bind_int64(snapshot.get(), 2, now), impl_->database,
          "bind saved scan start");
    check(sqlite3_bind_int64(snapshot.get(), 3, now), impl_->database,
          "bind saved scan update");
    check(sqlite3_bind_int64(snapshot.get(), 4, sourceId), impl_->database,
          "bind source scan id");
    check(sqlite3_step(snapshot.get()), impl_->database, "create saved scan snapshot");
    if (sqlite3_changes(impl_->database) == 0) {
      throw std::invalid_argument("source scan does not exist");
    }
    const auto snapshotId = sqlite3_last_insert_rowid(impl_->database);
    Statement assets(impl_->database, R"sql(
      INSERT INTO scan_snapshot_assets(
        session_id,asset_id,path,size_bytes,modified_ms,is_directory,is_symlink)
      SELECT ?,a.id,a.path,a.size_bytes,a.modified_ms,a.is_directory,a.is_symlink
      FROM scan_assets sa JOIN assets a ON a.id=sa.asset_id
      WHERE sa.session_id=?
    )sql");
    check(sqlite3_bind_int64(assets.get(), 1, snapshotId), impl_->database,
          "bind saved scan id");
    check(sqlite3_bind_int64(assets.get(), 2, sourceId), impl_->database,
          "bind source scan id");
    check(sqlite3_step(assets.get()), impl_->database, "copy saved scan assets");
    check(sqlite3_exec(impl_->database, "COMMIT", nullptr, nullptr, nullptr),
          impl_->database, "commit scan snapshot");
    return snapshotId;
  } catch (...) {
    sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

std::optional<ScanSession> Catalog::scanSession(ScanSessionId id) const {
  const auto sql = std::string("SELECT ") + scanSessionColumns +
                   " FROM scan_sessions s WHERE s.id=?";
  Statement statement(impl_->database, sql.c_str());
  check(sqlite3_bind_int64(statement.get(), 1, id), impl_->database, "bind scan id");
  const auto step = sqlite3_step(statement.get());
  if (step == SQLITE_DONE) return std::nullopt;
  check(step, impl_->database, "read scan session");
  return readScanSession(statement.get());
}

std::vector<ScanSession> Catalog::scanSessions() const {
  const auto sql = std::string("SELECT ") + scanSessionColumns +
                   " FROM scan_sessions s ORDER BY s.updated_ms DESC";
  Statement statement(impl_->database, sql.c_str());
  std::vector<ScanSession> result;
  int step{};
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    result.push_back(readScanSession(statement.get()));
  }
  check(step, impl_->database, "read scan sessions");
  return result;
}

void Catalog::setScanSessionState(ScanSessionId id, ScanSessionState state,
                                  std::string error) {
  Statement statement(impl_->database,
      "UPDATE scan_sessions SET state=?,updated_ms=?,error=? WHERE id=?");
  check(sqlite3_bind_int(statement.get(), 1, static_cast<int>(state)), impl_->database,
        "bind scan state");
  check(sqlite3_bind_int64(statement.get(), 2,
                          milliseconds(std::chrono::system_clock::now())),
        impl_->database, "bind scan state time");
  check(sqlite3_bind_text(statement.get(), 3, error.c_str(), -1, SQLITE_TRANSIENT),
        impl_->database, "bind scan error");
  check(sqlite3_bind_int64(statement.get(), 4, id), impl_->database, "bind scan id");
  check(sqlite3_step(statement.get()), impl_->database, "update scan state");
}

void Catalog::setScanOrganizationVersion(ScanSessionId id, std::uint32_t version) {
  Statement statement(impl_->database,
      "UPDATE scan_sessions SET organization_version=?,updated_ms=? WHERE id=?");
  check(sqlite3_bind_int(statement.get(), 1, static_cast<int>(version)), impl_->database,
        "bind organizer version");
  check(sqlite3_bind_int64(statement.get(), 2,
                          milliseconds(std::chrono::system_clock::now())),
        impl_->database, "bind organizer update");
  check(sqlite3_bind_int64(statement.get(), 3, id), impl_->database,
        "bind scan id");
  check(sqlite3_step(statement.get()), impl_->database,
        "update organizer version");
}

std::optional<std::filesystem::path> Catalog::nextScanDirectory(ScanSessionId id) const {
  Statement statement(impl_->database, R"sql(
    SELECT path FROM scan_directories
    WHERE session_id=? AND completed=0 ORDER BY path COLLATE NOCASE LIMIT 1
  )sql");
  check(sqlite3_bind_int64(statement.get(), 1, id), impl_->database, "bind scan id");
  const auto step = sqlite3_step(statement.get());
  if (step == SQLITE_DONE) return std::nullopt;
  check(step, impl_->database, "read pending scan directory");
  return std::filesystem::path(
      reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0)));
}

void Catalog::recordScannedDirectory(ScanSessionId id,
                                     const std::filesystem::path& directory,
                                     const std::vector<AssetMetadata>& entries,
                                     std::uint64_t inaccessible) {
  check(sqlite3_exec(impl_->database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr),
        impl_->database, "begin scanned directory");
  std::uint64_t newFiles{};
  std::uint64_t newDirectories{};
  std::uint64_t newBytes{};
  try {
    Statement link(impl_->database,
        "INSERT OR IGNORE INTO scan_assets(session_id,asset_id) VALUES(?,?)");
    Statement queue(impl_->database, R"sql(
      INSERT OR IGNORE INTO scan_directories(session_id,path,completed) VALUES(?,?,0)
    )sql");
    for (const auto& entry : entries) {
      const auto assetId = upsert(entry);
      sqlite3_reset(link.get());
      sqlite3_clear_bindings(link.get());
      check(sqlite3_bind_int64(link.get(), 1, id), impl_->database, "bind scan id");
      check(sqlite3_bind_int64(link.get(), 2, assetId), impl_->database, "bind scan asset");
      check(sqlite3_step(link.get()), impl_->database, "link scanned asset");
      const bool newlyLinked = sqlite3_changes(impl_->database) != 0;
      if (newlyLinked) {
        if (entry.isDirectory) ++newDirectories;
        else {
          ++newFiles;
          newBytes += entry.sizeBytes;
        }
      }
      if (entry.isDirectory && !entry.isSymlink) {
        const auto path = normalizedPath(entry.path);
        sqlite3_reset(queue.get());
        sqlite3_clear_bindings(queue.get());
        check(sqlite3_bind_int64(queue.get(), 1, id), impl_->database, "bind scan id");
        check(sqlite3_bind_text(queue.get(), 2, path.c_str(), -1, SQLITE_TRANSIENT),
              impl_->database, "bind queued directory");
        check(sqlite3_step(queue.get()), impl_->database, "queue scan directory");
      }
    }
    Statement complete(impl_->database,
        "UPDATE scan_directories SET completed=1 WHERE session_id=? AND path=?");
    const auto normalizedDirectory = normalizedPath(directory);
    check(sqlite3_bind_int64(complete.get(), 1, id), impl_->database, "bind scan id");
    check(sqlite3_bind_text(complete.get(), 2, normalizedDirectory.c_str(), -1,
                            SQLITE_TRANSIENT), impl_->database, "bind completed directory");
    check(sqlite3_step(complete.get()), impl_->database, "complete scan directory");
    Statement totals(impl_->database, R"sql(
      UPDATE scan_sessions SET files=files+?,directories=directories+?,bytes=bytes+?,
        inaccessible=inaccessible+?,updated_ms=? WHERE id=?
    )sql");
    check(sqlite3_bind_int64(totals.get(), 1, static_cast<sqlite3_int64>(newFiles)),
          impl_->database, "bind scan files");
    check(sqlite3_bind_int64(totals.get(), 2, static_cast<sqlite3_int64>(newDirectories)),
          impl_->database, "bind scan directories");
    check(sqlite3_bind_int64(totals.get(), 3, static_cast<sqlite3_int64>(newBytes)),
          impl_->database, "bind scan bytes");
    check(sqlite3_bind_int64(totals.get(), 4, static_cast<sqlite3_int64>(inaccessible)),
          impl_->database, "bind scan inaccessible");
    check(sqlite3_bind_int64(totals.get(), 5,
                            milliseconds(std::chrono::system_clock::now())),
          impl_->database, "bind scan update");
    check(sqlite3_bind_int64(totals.get(), 6, id), impl_->database, "bind scan id");
    check(sqlite3_step(totals.get()), impl_->database, "update scan totals");
    check(sqlite3_exec(impl_->database, "COMMIT", nullptr, nullptr, nullptr),
          impl_->database, "commit scanned directory");
  } catch (...) {
    sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

std::vector<CatalogAsset> Catalog::scanAssets(ScanSessionId id, std::size_t limit) const {
  const auto session = scanSession(id);
  if (!session) return {};
  std::string sql = "SELECT a.";
  sql += session->snapshot ? "asset_id" : "id";
  sql += ",a.path,a.size_bytes,a.modified_ms,a.is_directory,a.is_symlink ";
  sql += session->snapshot
             ? "FROM scan_snapshot_assets a WHERE a.session_id=? "
             : "FROM scan_assets sa JOIN assets a ON a.id=sa.asset_id WHERE sa.session_id=? ";
  sql += R"sql(
    ORDER BY
      CASE
        WHEN a.is_directory=1 THEN 9
        WHEN lower(a.path) LIKE '%albedo%' OR lower(a.path) LIKE '%basecolor%'
          OR lower(a.path) LIKE '%base_color%' OR lower(a.path) LIKE '%diffuse%'
          OR lower(a.path) LIKE '%roughness%' OR lower(a.path) LIKE '%glossiness%'
          OR lower(a.path) LIKE '%gloss%' OR lower(a.path) LIKE '%metallic%'
          OR lower(a.path) LIKE '%normal%' OR lower(a.path) LIKE '%ambient_occlusion%'
          OR lower(a.path) LIKE '%_ao.%' OR lower(a.path) LIKE '%height%'
          OR lower(a.path) LIKE '%displacement%' OR lower(a.path) LIKE '%opacity%'
          OR lower(a.path) LIKE '%mask%' OR lower(a.path) LIKE '%translucen%'
          OR lower(a.path) LIKE '%transmissive%' OR lower(a.path) LIKE '%transfert%'
          OR lower(a.path) LIKE '%specular%' OR lower(a.path) LIKE '%reflection%'
          OR lower(a.path) LIKE '%thickness%' THEN 0
        WHEN lower(a.path) GLOB '*.fbx' OR lower(a.path) GLOB '*.obj'
          OR lower(a.path) GLOB '*.blend' OR lower(a.path) GLOB '*.max'
          OR lower(a.path) GLOB '*.gltf' OR lower(a.path) GLOB '*.glb'
          OR lower(a.path) GLOB '*.usd' OR lower(a.path) GLOB '*.stl' THEN 1
        WHEN lower(a.path) GLOB '*.png' OR lower(a.path) GLOB '*.jpg'
          OR lower(a.path) GLOB '*.jpeg' OR lower(a.path) GLOB '*.exr'
          OR lower(a.path) GLOB '*.tif' OR lower(a.path) GLOB '*.tiff' THEN 2
        WHEN lower(a.path) GLOB '*.mp4' OR lower(a.path) GLOB '*.mov'
          OR lower(a.path) GLOB '*.mkv' OR lower(a.path) GLOB '*.avi' THEN 3
        WHEN lower(a.path) GLOB '*.zip' OR lower(a.path) GLOB '*.7z'
          OR lower(a.path) GLOB '*.rar' THEN 4
        ELSE 5
      END,
      a.path COLLATE NOCASE LIMIT ?
  )sql";
  Statement statement(impl_->database, sql.c_str());
  check(sqlite3_bind_int64(statement.get(), 1, id), impl_->database, "bind scan id");
  check(sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(limit)),
        impl_->database, "bind scan result limit");
  std::vector<CatalogAsset> result;
  int step{};
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    CatalogAsset asset;
    asset.id = sqlite3_column_int64(statement.get(), 0);
    asset.metadata.path = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
    asset.metadata.sizeBytes = static_cast<std::uintmax_t>(sqlite3_column_int64(statement.get(), 2));
    asset.metadata.modifiedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 3));
    asset.metadata.isDirectory = sqlite3_column_int(statement.get(), 4) != 0;
    asset.metadata.isSymlink = sqlite3_column_int(statement.get(), 5) != 0;
    result.push_back(std::move(asset));
  }
  check(step, impl_->database, "read saved scan assets");
  return result;
}

std::vector<CatalogAsset> Catalog::scanPbrTextureAssets(ScanSessionId id,
                                                        std::size_t limit) const {
  const auto session = scanSession(id);
  if (!session) return {};
  std::string sql = "SELECT a.";
  sql += session->snapshot ? "asset_id" : "id";
  sql += ",a.path,a.size_bytes,a.modified_ms,a.is_directory,a.is_symlink ";
  sql += session->snapshot
             ? "FROM scan_snapshot_assets a WHERE a.session_id=? AND a.is_directory=0 "
             : "FROM scan_assets sa JOIN assets a ON a.id=sa.asset_id "
               "WHERE sa.session_id=? AND a.is_directory=0 ";
  sql += R"sql(
      AND (lower(a.path) GLOB '*.png' OR lower(a.path) GLOB '*.jpg'
        OR lower(a.path) GLOB '*.jpeg' OR lower(a.path) GLOB '*.tga'
        OR lower(a.path) GLOB '*.tif' OR lower(a.path) GLOB '*.tiff'
        OR lower(a.path) GLOB '*.bmp' OR lower(a.path) GLOB '*.exr')
      AND (lower(a.path) LIKE '%basecolor%' OR lower(a.path) LIKE '%base_color%'
        OR lower(a.path) LIKE '%albedo%' OR lower(a.path) LIKE '%diffuse%'
        OR lower(a.path) LIKE '%normal%' OR lower(a.path) LIKE '%roughness%'
        OR lower(a.path) LIKE '%glossiness%' OR lower(a.path) LIKE '%gloss%'
        OR lower(a.path) LIKE '%metallic%' OR lower(a.path) LIKE '%metalness%'
        OR lower(a.path) LIKE '%ambientocclusion%' OR lower(a.path) LIKE '%ambient_occlusion%'
        OR lower(a.path) LIKE '%height%' OR lower(a.path) LIKE '%displacement%'
        OR lower(a.path) LIKE '%opacity%' OR lower(a.path) LIKE '%mask%'
        OR lower(a.path) LIKE '%translucen%' OR lower(a.path) LIKE '%transmissive%'
        OR lower(a.path) LIKE '%transfert%' OR lower(a.path) LIKE '%specular%'
        OR lower(a.path) LIKE '%reflection%' OR lower(a.path) LIKE '%thickness%'
        OR lower(a.path) LIKE '%emissive%' OR lower(a.path) LIKE '%emission%'
        OR lower(a.path) LIKE '%_orm.%' OR lower(a.path) LIKE '%_rma.%')
    ORDER BY a.path COLLATE NOCASE LIMIT ?
  )sql";
  Statement statement(impl_->database, sql.c_str());
  check(sqlite3_bind_int64(statement.get(), 1, id), impl_->database, "bind PBR scan id");
  check(sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(limit)),
        impl_->database, "bind PBR scan result limit");
  std::vector<CatalogAsset> result;
  int step{};
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    CatalogAsset asset;
    asset.id = sqlite3_column_int64(statement.get(), 0);
    asset.metadata.path = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
    asset.metadata.sizeBytes = static_cast<std::uintmax_t>(sqlite3_column_int64(statement.get(), 2));
    asset.metadata.modifiedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 3));
    asset.metadata.isDirectory = sqlite3_column_int(statement.get(), 4) != 0;
    asset.metadata.isSymlink = sqlite3_column_int(statement.get(), 5) != 0;
    result.push_back(std::move(asset));
  }
  check(step, impl_->database, "read saved PBR scan assets");
  return result;
}

std::uint64_t Catalog::assetCount() const {
  Statement statement(impl_->database, "SELECT COUNT(*) FROM assets");
  check(sqlite3_step(statement.get()), impl_->database, "count assets");
  return static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
}

}  // namespace atlas
