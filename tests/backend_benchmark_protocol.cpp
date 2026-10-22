#include "tests/backend_benchmark_protocol.h"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <type_traits>

namespace ava::benchmark {
namespace {

std::string escape_json(std::string_view value)
{
  std::ostringstream output;
  for (unsigned char const character : value)
  {
    switch (character)
    {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20)
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(character) << std::dec;
        else
          output << static_cast<char>(character);
        break;
    }
  }
  return std::move(output).str();
}

void emit_scalar(JsonScalar const& value)
{
  std::visit(
      [](auto const& item) {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, bool>)
          std::cout << (item ? "true" : "false");
        else if constexpr (std::is_same_v<Item, std::string>)
          std::cout << '"' << escape_json(item) << '"';
        else if constexpr (std::is_same_v<Item, double>)
        {
          if (!std::isfinite(item))
          {
            std::cerr << "benchmark protocol refused a non-finite number\n";
            std::exit(2);
          }
          std::cout << std::setprecision(17) << item;
        }
        else
          std::cout << item;
      },
      value);
}

void emit_fields(JsonFields const& fields)
{
  std::cout << '{';
  bool first = true;
  for (auto const& [name, value] : fields)
  {
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << '"' << escape_json(name) << "\":";
    emit_scalar(value);
  }
  std::cout << '}';
}

void emit_prefix(std::string_view benchmark_case, std::string_view status, std::string_view primary_metric, std::string_view unit)
{
  std::cout << "{\"helper_schema_version\":\"ava.backend-benchmark-helper.v2\",\"case\":\"" << escape_json(benchmark_case) << "\",\"status\":\"" << status
            << "\",\"primary_metric\":\"" << escape_json(primary_metric) << "\",\"unit\":\"" << escape_json(unit) << "\",";
}

}  // namespace

void emit_helper_measurement(std::string_view benchmark_case, std::string_view primary_metric, std::string_view unit,
                             std::vector<Observation> const& observations, JsonFields const& case_metrics)
{
  emit_prefix(benchmark_case, "measured", primary_metric, unit);
  std::cout << "\"observations\":[";
  bool first = true;
  for (auto const& observation : observations)
  {
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << "{\"ordinal\":" << observation.ordinal << ",\"value\":";
    emit_scalar(observation.value);
    std::cout << ",\"metrics\":";
    emit_fields(observation.metrics);
    std::cout << ",\"checks\":";
    emit_fields(observation.checks);
    std::cout << '}';
  }
  std::cout << "],\"case_metrics\":";
  emit_fields(case_metrics);
  std::cout << "}\n";
}

void emit_helper_unsupported(std::string_view benchmark_case, std::string_view primary_metric, std::string_view unit, std::string_view reason_code,
                             std::string_view reason, JsonFields const& case_metrics)
{
  emit_prefix(benchmark_case, "unsupported", primary_metric, unit);
  std::cout << "\"reason_code\":\"" << escape_json(reason_code) << "\",\"reason\":\"" << escape_json(reason) << "\",\"observations\":[],\"case_metrics\":";
  emit_fields(case_metrics);
  std::cout << "}\n";
}

}  // namespace ava::benchmark
