#pragma once

#include "ava/debug/print_members_on.h"

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace ava::tui::terminal {

enum class Attribute
{
  normal = 0,           // Normal display (no highlight).
  bold = 1,             // Extra bright or bold.
  underline = 2,        // Underlining.
  standout = 4,         // Best highlighting mode of the terminal.
  blink = 8             // Blinking.
};

class Attributes
{
 public:
  using attr_t = uint32_t;      // Internal type used to store a bitmask of Attributes.

 private:
  attr_t mask_;

 public:
  // Default constructor: no attributes.
  Attributes() : mask_(static_cast<attr_t>(Attribute::normal)) { }

  // Construct an Attributes from a single Attribute.
  Attributes(Attribute attr) : mask_(static_cast<attr_t>(attr)) { }

  // OR together Attribute's.
  Attributes& operator|=(Attribute attr)
  {
    mask_ |= static_cast<attr_t>(attr);
    return *this;
  }

  // OR together Attributes'.
  Attributes& operator|=(Attributes attributes)
  {
    mask_ |= attributes.mask_;
    return *this;
  }

  bool operator==(Attribute attr) const
  {
    return mask_ == static_cast<attr_t>(attr);
  }

  // Accessor.
  attr_t mask() const { return mask_; }
  attr_t& mask() { return mask_; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// clang-format off

template<typename T>
concept AttributesConcept =
  std::same_as<std::remove_cvref_t<T>, Attributes> ||
  std::same_as<std::remove_cvref_t<T>, Attribute>;

// clang-format on

// Free operator| to OR Attributes and Attribute types.
Attributes operator|=(AttributesConcept auto&& lhs, AttributesConcept auto&& rhs)
{
  Attributes result(lhs);
  result |= rhs;
  return result;
}

} // namespace ava::tui::terminal
