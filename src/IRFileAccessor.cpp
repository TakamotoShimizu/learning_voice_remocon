#include "IRFileAccessor.h"

#include <sqlite3.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
std::vector<IRRecord> readLegacyTextFile(const std::string& path) {
  std::ifstream input(path);
  std::vector<IRRecord> records;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto separator = line.find('|');
    if (separator == std::string::npos || separator == 0 || separator + 1 == line.size()) continue;
    records.push_back({line.substr(0, separator), line.substr(separator + 1), 1, 100});
  }
  return records;
}

bool isLegacyTextFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  char signature[16]{};
  input.read(signature, sizeof(signature));
  if (input.gcount() == 0) return false;
  constexpr char sqliteSignature[] = "SQLite format 3\0";
  return input.gcount() < 16 || std::string(signature, sizeof(signature)) !=
                                  std::string(sqliteSignature, sizeof(sqliteSignature) - 1);
}

bool fileExists(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return input.good();
}

void check(sqlite3* db, int result, const char* operation) {
  if (result != SQLITE_OK) {
    throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(db));
  }
}

void execute(sqlite3* db, const char* sql) {
  char* error = nullptr;
  const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string message = error ? error : sqlite3_errmsg(db);
    sqlite3_free(error);
    throw std::runtime_error("SQLite: " + message);
  }
}

class Statement {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    check(db_, sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr), "SQLite文の準備に失敗しました");
  }
  ~Statement() { sqlite3_finalize(statement_); }

  sqlite3_stmt* get() const { return statement_; }

 private:
  sqlite3* db_;
  sqlite3_stmt* statement_ = nullptr;
};

void bindText(sqlite3* db, sqlite3_stmt* statement, int index, const std::string& value) {
  check(db, sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT), "SQLiteへの値設定に失敗しました");
}

IRRecord readRecord(sqlite3_stmt* statement) {
  const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
  const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
  return {name ? name : "", data ? data : "", sqlite3_column_int(statement, 2), sqlite3_column_int(statement, 3)};
}
}  // namespace

IRFileAccessor::~IRFileAccessor() {
  close();
}

void IRFileAccessor::open(const std::string& dbPath) {
  close();
  std::vector<IRRecord> legacyRecords;
  std::string legacyPath;
  std::string legacySourcePath = dbPath;
  if (dbPath == "codes.db" && !fileExists(dbPath) && isLegacyTextFile("codes.txt")) {
    legacySourcePath = "codes.txt";
  }
  if (isLegacyTextFile(legacySourcePath)) {
    legacyRecords = readLegacyTextFile(legacySourcePath);
    legacyPath = legacySourcePath + ".legacy";
    std::ifstream backupExists(legacyPath, std::ios::binary);
    if (backupExists) {
      throw std::runtime_error("旧形式のデータベースを移行できません。バックアップが既にあります: " + legacyPath);
    }
    if (std::rename(legacySourcePath.c_str(), legacyPath.c_str()) != 0) {
      throw std::runtime_error("旧形式のデータベースを退避できません: " + legacySourcePath);
    }
  }
  const int result = sqlite3_open(dbPath.c_str(), &db_);
  if (result != SQLITE_OK) {
    const std::string message = db_ ? sqlite3_errmsg(db_) : "SQLiteを初期化できません";
    close();
    if (!legacyPath.empty()) std::rename(legacyPath.c_str(), legacySourcePath.c_str());
    throw std::runtime_error("データベースを開けません: " + message);
  }
  try {
    execute(db_, "CREATE TABLE IF NOT EXISTS ir_signals ("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "name TEXT NOT NULL UNIQUE,"
                 "data TEXT NOT NULL,"
                 "repeat INTEGER NOT NULL DEFAULT 1,"
                 "gapMs INTEGER NOT NULL DEFAULT 100)");
    begin();
    for (const auto& record : legacyRecords) {
      insert(record.name, record.data, record.repeat, record.gapMs);
    }
    if (!legacyRecords.empty()) {
      commit();
      execute(db_, "BEGIN");
    }
  } catch (...) {
    if (!legacyRecords.empty()) {
      try { rollback(); } catch (...) {}
    }
    close();
    if (!legacyPath.empty()) {
      std::remove(dbPath.c_str());
      std::rename(legacyPath.c_str(), legacySourcePath.c_str());
    }
    throw;
  }
}

void IRFileAccessor::close() {
  if (db_) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

void IRFileAccessor::insert(const std::string& name, const std::string& data, int repeat, int gapMs) {
  Statement statement(db_, "INSERT INTO ir_signals(name, data, repeat, gapMs) VALUES(?, ?, ?, ?)");
  bindText(db_, statement.get(), 1, name);
  bindText(db_, statement.get(), 2, data);
  check(db_, sqlite3_bind_int(statement.get(), 3, repeat), "SQLiteへの値設定に失敗しました");
  check(db_, sqlite3_bind_int(statement.get(), 4, gapMs), "SQLiteへの値設定に失敗しました");
  check(db_, sqlite3_step(statement.get()) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(db_), "信号を登録できません");
}

bool IRFileAccessor::deleteByName(const std::string& name) {
  Statement statement(db_, "DELETE FROM ir_signals WHERE name = ?");
  bindText(db_, statement.get(), 1, name);
  const int result = sqlite3_step(statement.get());
  check(db_, result == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(db_), "信号を削除できません");
  return sqlite3_changes(db_) > 0;
}

void IRFileAccessor::clearAll() {
  execute(db_, "DELETE FROM ir_signals");
}

bool IRFileAccessor::update(const std::string& name, const std::string& data, int repeat, int gapMs) {
  Statement statement(db_, "UPDATE ir_signals SET data = ?, repeat = ?, gapMs = ? WHERE name = ?");
  bindText(db_, statement.get(), 1, data);
  check(db_, sqlite3_bind_int(statement.get(), 2, repeat), "SQLiteへの値設定に失敗しました");
  check(db_, sqlite3_bind_int(statement.get(), 3, gapMs), "SQLiteへの値設定に失敗しました");
  bindText(db_, statement.get(), 4, name);
  const int result = sqlite3_step(statement.get());
  check(db_, result == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(db_), "信号を更新できません");
  return sqlite3_changes(db_) > 0;
}

std::optional<IRRecord> IRFileAccessor::fetchByName(const std::string& name) const {
  Statement statement(db_, "SELECT name, data, repeat, gapMs FROM ir_signals WHERE name = ?");
  bindText(db_, statement.get(), 1, name);
  const int result = sqlite3_step(statement.get());
  if (result == SQLITE_DONE) return std::nullopt;
  if (result != SQLITE_ROW) check(db_, sqlite3_errcode(db_), "信号を取得できません");
  return readRecord(statement.get());
}

std::vector<IRRecord> IRFileAccessor::fetchAll() const {
  Statement statement(db_, "SELECT name, data, repeat, gapMs FROM ir_signals ORDER BY id");
  std::vector<IRRecord> records;
  while (true) {
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) break;
    if (result != SQLITE_ROW) check(db_, sqlite3_errcode(db_), "信号一覧を取得できません");
    records.push_back(readRecord(statement.get()));
  }
  return records;
}

void IRFileAccessor::commit() {
  execute(db_, "COMMIT");
}

void IRFileAccessor::begin() {
  execute(db_, "BEGIN");
}

void IRFileAccessor::rollback() {
  execute(db_, "ROLLBACK");
}
