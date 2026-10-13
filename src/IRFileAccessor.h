#ifndef IR_FILE_ACCESSOR_H
#define IR_FILE_ACCESSOR_H

#include <optional>
#include <string>
#include <vector>

struct sqlite3;

struct IRRecord {
  std::string name;
  std::string data;
  int repeat = 1;
  int gapMs = 100;
};

class IRFileAccessor {
 public:
  IRFileAccessor() = default;
  ~IRFileAccessor();

  IRFileAccessor(const IRFileAccessor&) = delete;
  IRFileAccessor& operator=(const IRFileAccessor&) = delete;

  void open(const std::string& dbPath);
  void close();
  void insert(const std::string& name, const std::string& data, int repeat, int gapMs);
  bool deleteByName(const std::string& name);
  void clearAll();
  bool update(const std::string& name, const std::string& data, int repeat, int gapMs);
  std::optional<IRRecord> fetchByName(const std::string& name) const;
  std::vector<IRRecord> fetchAll() const;
  void begin();
  void commit();
  void rollback();

 private:
  sqlite3* db_ = nullptr;
};

#endif  // IR_FILE_ACCESSOR_H
