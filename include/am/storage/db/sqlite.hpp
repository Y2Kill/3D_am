#pragma once
#include <filesystem>
#include <optional>

#include "am/foundation/result.hpp"

struct sqlite3;
struct sqlite3_stmt;
namespace am::db {
// Recognizes SQLite and native VFS disk-full errors without classifying other I/O failures.
std::optional<Error> insufficient_space_error(sqlite3*, int sqlite_code);
enum class Step { row, done };
class Statement {
 public:
  Statement(Statement&&) noexcept;
  Statement& operator=(Statement&&) noexcept;
  ~Statement();
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Result<void> bind(int index, std::string_view);
  Result<void> bind(int index, std::int64_t);
  Result<void> bind(int index, double);
  Result<void> bind_null(int index);
  Result<Step> step();
  bool is_null(int column) const noexcept;
  Result<std::string> text(int column) const;
  std::int64_t integer(int column) const noexcept;
  double real(int column) const noexcept;

 private:
  sqlite3_stmt* stmt_ = nullptr;
  explicit Statement(sqlite3_stmt*) noexcept;
  friend class Connection;
};
class Connection {
 public:
  static Result<Connection> open(const std::filesystem::path&, bool read_only = false);
  // The owner must outlive the borrowed connection and all its statements.
  static Connection borrow(sqlite3*) noexcept;
  Connection(Connection&&) noexcept;
  Connection& operator=(Connection&&) noexcept;
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Result<Statement> prepare(std::string_view) const;
  Result<void> exec(std::string_view) const;

 private:
  sqlite3* db_ = nullptr;
  bool owned_ = false;
  Connection(sqlite3*, bool) noexcept;
};
class Transaction {
 public:
  static Result<Transaction> begin(Connection&);
  Transaction(Transaction&&) noexcept;
  Transaction& operator=(Transaction&&) noexcept;
  ~Transaction();
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Result<void> commit();

 private:
  Connection* db_ = nullptr;
  explicit Transaction(Connection&) noexcept;
};
}  // namespace am::db
