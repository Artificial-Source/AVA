#include "sys.h"
#include "threadsafe/threadsafe.h"

#include <iostream>
#include <type_traits>
#include <utility>

namespace {

struct ProbeMutex
{
  static inline int lock_depth = 0;
  static inline int lock_calls = 0;
  static inline int unlock_calls = 0;

  void lock()
  {
    ++lock_depth;
    ++lock_calls;
  }

  void unlock()
  {
    --lock_depth;
    ++unlock_calls;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ProbeValue
{
  static inline bool copied_while_locked = false;
  static inline bool moved_while_locked = false;

  ProbeValue() = default;
  ProbeValue(ProbeValue const&) { copied_while_locked = ProbeMutex::lock_depth == 1; }
  ProbeValue(ProbeValue&&) noexcept { moved_while_locked = ProbeMutex::lock_depth == 1; }
  int value() const { return 0; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

template <class UNLOCKED>
struct DerivedPrimitiveAccess : public threadsafe::Access<UNLOCKED>
{
  using threadsafe::Access<UNLOCKED>::Access;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class DerivedPrimitivePolicy
{
 public:
  using reference_type = threadsafe::policy::PrimitiveRef<ProbeMutex>;

  template <class UNLOCKED>
  struct access_types
  {
    using const_read_access_type = threadsafe::AccessConst<UNLOCKED>;
    using read_access_type = threadsafe::ConstAccess<UNLOCKED>;
    using write_access_type = DerivedPrimitiveAccess<UNLOCKED>;
    using write_to_read_carry = threadsafe::policy::unsupported_w2rCarry<UNLOCKED>;
    using read_access_base_type = threadsafe::AccessConst<UNLOCKED>;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

 protected:
  template <class UNLOCKED>
  friend struct threadsafe::AccessConst;
  template <class UNLOCKED>
    requires utils::is_specialization_of_v<UNLOCKED, threadsafe::Unlocked> || utils::is_specialization_of_v<UNLOCKED, threadsafe::UnlockedBase>
  friend struct threadsafe::Access;

  mutable ProbeMutex mutex_;

  ProbeMutex& mutex() const { return mutex_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class ProtectedOneThreadOwner : protected threadsafe::Unlocked<ProbeValue, threadsafe::policy::OneThread>
{
 public:
  using unlocked_type = threadsafe::Unlocked<ProbeValue, threadsafe::policy::OneThread>;
  using crat = unlocked_type::crat;
  using rat = unlocked_type::rat;
  using wat = unlocked_type::wat;

 private:
  friend unlocked_type& threadsafe_access(ProtectedOneThreadOwner& owner, threadsafe::AccessKey<unlocked_type>)
  {
    return static_cast<unlocked_type&>(owner);
  }

  friend unlocked_type const& threadsafe_access(ProtectedOneThreadOwner const& owner, threadsafe::AccessKey<unlocked_type>)
  {
    return static_cast<unlocked_type const&>(owner);
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class UnadaptedOneThreadOwner : protected threadsafe::Unlocked<ProbeValue, threadsafe::policy::OneThread>
{
 public:
  using unlocked_type = threadsafe::Unlocked<ProbeValue, threadsafe::policy::OneThread>;
  using crat = unlocked_type::crat;
  using rat = unlocked_type::rat;
  using wat = unlocked_type::wat;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

static_assert(std::constructible_from<ProtectedOneThreadOwner::crat, ProtectedOneThreadOwner&>);
static_assert(std::constructible_from<ProtectedOneThreadOwner::crat, ProtectedOneThreadOwner const&>);
static_assert(std::constructible_from<ProtectedOneThreadOwner::rat, ProtectedOneThreadOwner&>);
static_assert(std::constructible_from<ProtectedOneThreadOwner::wat, ProtectedOneThreadOwner&>);

// Mutable OneThread access must not adapt a const owner.
static_assert(!std::constructible_from<ProtectedOneThreadOwner::rat, ProtectedOneThreadOwner const&>);
static_assert(!std::constructible_from<ProtectedOneThreadOwner::wat, ProtectedOneThreadOwner const&>);

// Protected inheritance alone must not expose the Unlocked base to access constructors.
static_assert(!std::constructible_from<UnadaptedOneThreadOwner::crat, UnadaptedOneThreadOwner&>);
static_assert(!std::constructible_from<UnadaptedOneThreadOwner::crat, UnadaptedOneThreadOwner const&>);
static_assert(!std::constructible_from<UnadaptedOneThreadOwner::rat, UnadaptedOneThreadOwner&>);
static_assert(!std::constructible_from<UnadaptedOneThreadOwner::wat, UnadaptedOneThreadOwner&>);

static_assert(std::move_constructible<ProtectedOneThreadOwner::wat>);

// Const wrappers must be valid badge arguments without instantiating forbidden mutable access types.
using const_one_thread_base = threadsafe::ConstUnlockedBase<ProbeValue, threadsafe::policy::OneThread>;
static_assert(sizeof(threadsafe::AccessKey<const_one_thread_base>) > 0);

void reset_probes()
{
  ProbeMutex::lock_depth = 0;
  ProbeMutex::lock_calls = 0;
  ProbeMutex::unlock_calls = 0;
  ProbeValue::copied_while_locked = false;
  ProbeValue::moved_while_locked = false;
}

bool expect(bool condition, char const* message)
{
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main()
{
  using unlocked_type = threadsafe::Unlocked<ProbeValue, DerivedPrimitivePolicy>;
  static_assert(std::is_base_of_v<threadsafe::Access<unlocked_type>, unlocked_type::wat>);

  bool passed = true;
  reset_probes();
  unlocked_type copy_source;
  unlocked_type copied(copy_source);
  passed &= expect(ProbeValue::copied_while_locked, "copy construction locks before copying the value");
  passed &= expect(ProbeMutex::lock_calls == 1 && ProbeMutex::unlock_calls == 1 && ProbeMutex::lock_depth == 0,
                   "copy construction balances the derived primitive access lock");

  reset_probes();
  unlocked_type move_source;
  unlocked_type moved(std::move(move_source));
  passed &= expect(ProbeValue::moved_while_locked, "move construction locks before moving the value");
  passed &= expect(ProbeMutex::lock_calls == 1 && ProbeMutex::unlock_calls == 1 && ProbeMutex::lock_depth == 0,
                   "move construction balances the derived primitive access lock");

  static_cast<void>(copied);
  static_cast<void>(moved);

  ProtectedOneThreadOwner one_thread_owner;
  ProtectedOneThreadOwner const& const_one_thread_owner = one_thread_owner;
  {
    ProtectedOneThreadOwner::crat read_access(const_one_thread_owner);
    passed &= expect(read_access->value() == 0, "const OneThread owner provides const read access");
  }
  {
    ProtectedOneThreadOwner::crat read_access(one_thread_owner);
    passed &= expect(read_access->value() == 0, "non-const OneThread owner provides const read access");
  }
  {
    ProtectedOneThreadOwner::rat read_access(one_thread_owner);
    ProtectedOneThreadOwner::crat const& const_read_reference = read_access;
    passed &= expect(&*const_read_reference == &*read_access, "OneThread read access preserves its const reference conversion");
  }
  {
    ProtectedOneThreadOwner::wat original_write_access(one_thread_owner);
    ProtectedOneThreadOwner::wat write_access(std::move(original_write_access));
    ProtectedOneThreadOwner::rat const& read_reference = write_access;
    ProtectedOneThreadOwner::crat const& const_read_reference = read_reference;
    passed &= expect(&*const_read_reference == &*write_access, "moved OneThread write access preserves read reference conversions");
  }

  threadsafe::Unlocked<ProbeValue, threadsafe::policy::OneThread> base_source;
  const_one_thread_base const_base(base_source);
  {
    const_one_thread_base::crat read_access(const_base);
    passed &= expect(read_access->value() == 0, "const OneThread base wrapper remains readable after owner adaptation");
  }

  return passed ? 0 : 1;
}
