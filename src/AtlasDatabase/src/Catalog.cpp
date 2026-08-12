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
    PRAGMA user_version = 2;
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
    check(sqlite3_exec(impl_->database, "PRAGMA user_version=2", nullptr, nullptr, nullptr),
          impl_->database, "record catalog migration");
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

std::uint64_t Catalog::assetCount() const {
  Statement statement(impl_->database, "SELECT COUNT(*) FROM assets");
  check(sqlite3_step(statement.get()), impl_->database, "count assets");
  return static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
}

}  // namespace atlas
