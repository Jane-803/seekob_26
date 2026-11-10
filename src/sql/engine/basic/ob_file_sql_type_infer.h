#ifndef OCEANBASE_SQL_OB_FILE_SQL_TYPE_INFER_H_
#define OCEANBASE_SQL_OB_FILE_SQL_TYPE_INFER_H_

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

namespace oceanbase {
namespace sql {

enum class ObFileSqlType {
  NULL_VALUE,
  BOOLEAN,
  INTEGER,
  DOUBLE,
  DATE,
  DATETIME,
  STRING
};

inline bool is_iso_date(const std::string &s)
{
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (int i = 0; i < 10; ++i) {
    if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return false;
  }
  int y = std::atoi(s.substr(0, 4).c_str());
  int m = std::atoi(s.substr(5, 2).c_str());
  int d = std::atoi(s.substr(8, 2).c_str());
  if (y == 0 || m < 1 || m > 12 || d < 1) return false;
  const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
  bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  return d <= days[m] + (m == 2 && leap ? 1 : 0);
}

inline bool is_iso_datetime(const std::string &s)
{
  if (s.size() != 19 || !is_iso_date(s.substr(0, 10)) ||
      (s[10] != ' ' && s[10] != 'T') ||
      s[13] != ':' || s[16] != ':') return false;
  for (int i = 11; i < 19; ++i) {
    if (i != 13 && i != 16 && (s[i] < '0' || s[i] > '9')) return false;
  }
  int h = std::atoi(s.substr(11, 2).c_str());
  int m = std::atoi(s.substr(14, 2).c_str());
  int sec = std::atoi(s.substr(17, 2).c_str());
  return h < 24 && m < 60 && sec < 60;
}

inline void deduplicate_csv_columns(std::vector<std::string> &names)
{
  std::vector<std::string> used;
  for (size_t i = 0; i < names.size(); ++i) {
    std::string base = names[i].empty() ? "column" : names[i];
    std::string name = base;
    int suffix = 2;
    while (std::find(used.begin(), used.end(), name) != used.end()) {
      name = base + "__" + std::to_string(suffix++);
    }
    names[i] = name;
    used.push_back(name);
  }
}

inline ObFileSqlType merge_csv_types(ObFileSqlType a, ObFileSqlType b)
{
  if (a == ObFileSqlType::NULL_VALUE) return b;
  if (b == ObFileSqlType::NULL_VALUE) return a;
  if (a == b) return a;
  if ((a == ObFileSqlType::INTEGER && b == ObFileSqlType::DOUBLE) ||
      (a == ObFileSqlType::DOUBLE && b == ObFileSqlType::INTEGER))
    return ObFileSqlType::DOUBLE;
  if ((a == ObFileSqlType::DATE && b == ObFileSqlType::DATETIME) ||
      (a == ObFileSqlType::DATETIME && b == ObFileSqlType::DATE))
    return ObFileSqlType::DATETIME;
  return ObFileSqlType::STRING;
}

inline ObFileSqlType infer_csv_value(const std::string &s)
{
  if (s.empty()) return ObFileSqlType::NULL_VALUE;
  if (s == "true" || s == "false") return ObFileSqlType::BOOLEAN;

  char *end = nullptr;
  errno = 0;
  std::strtoll(s.c_str(), &end, 10);
  if (errno == 0 && end != s.c_str() && *end == '\0') {
    return ObFileSqlType::INTEGER;
  }

  errno = 0;
  double value = std::strtod(s.c_str(), &end);
  if (errno == 0 && end != s.c_str() &&
      *end == '\0' && std::isfinite(value)) {
    return ObFileSqlType::DOUBLE;
  }

  if (is_iso_datetime(s)) return ObFileSqlType::DATETIME;
  if (is_iso_date(s)) return ObFileSqlType::DATE;
  return ObFileSqlType::STRING;
}

} // namespace sql
} // namespace oceanbase

#endif
