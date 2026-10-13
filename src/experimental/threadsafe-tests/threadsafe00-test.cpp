#include "sys.h"
#include "threadsafe/AIReadWriteMutex.h"
#include "threadsafe/ObjectTracker.h"
#include "threadsafe/ObjectTracker.inl.h"
#include "utils/AIRefCount.h"

#include <cassert>
#include <concepts>
#include <iostream>
#include <type_traits>
#include <utility>

using namespace threadsafe;

template <int size, typename U>
void do_asserts()
{
#if THREADSAFE_DEBUG
  static_assert(std::max(alignof(typename U::data_type), alignof(policy::OneThread)) == alignof(U), "alignof(Unlocked<T, OneThread>) != alignof(T)!");
#else
  static_assert(alignof(typename U::data_type) == alignof(U), "alignof(Unlocked<T, OneThread>) != alignof(T)!");
#endif
  static_assert(alignof(Unlocked<typename U::data_type, policy::Primitive<std::mutex>>) % alignof(typename U::data_type) == 0,
                "alignof(Unlocked<T, Primitive<std::mutex>>) is not a multiple of alignof(T)!");
}

template <int size>
void do_size_test()
{
  struct T0
  {
    char a[size];
  };
  struct T1
  {
    char x;
    char a[size];
  };
  struct T2
  {
    short x;
    char a[size];
  };
  struct T4
  {
    int32_t x;
    char a[size];
  };
  struct T8
  {
    int64_t x;
    char a[size];
  };

  do_asserts<size, Unlocked<T1, policy::OneThread>>();
  do_asserts<size, Unlocked<T2, policy::OneThread>>();
  do_asserts<size, Unlocked<T4, policy::OneThread>>();
  do_asserts<size, Unlocked<T8, policy::OneThread>>();
}

enum state_type
{
  unlocked,
  readlocked,
  writelocked
};

class TestRWMutex
{
 private:
  state_type m_state;

 public:
  TestRWMutex() : m_state(unlocked) { }

  void rdlock()
  {
    assert(m_state == unlocked);
    m_state = readlocked;
  }
  void rdlock(bool& forwarded)
  {
    forwarded = true;
    rdlock();
  }
  void rdunlock()
  {
    assert(m_state == readlocked);
    m_state = unlocked;
  }
  void wrlock()
  {
    assert(m_state == unlocked);
    m_state = writelocked;
  }
  void wrlock(bool& forwarded)
  {
    forwarded = true;
    wrlock();
  }
  void wrunlock()
  {
    assert(m_state == writelocked);
    m_state = unlocked;
  }
  void rd2wrlock()
  {
    assert(m_state == readlocked);
    m_state = writelocked;
  }
  void wr2rdlock()
  {
    assert(m_state == writelocked);
    m_state = readlocked;
  }
  void rd2wryield() { }

 public:
  bool is_unlocked() const { return m_state == unlocked; }
  bool is_readlocked() const { return m_state == readlocked; }
  bool is_writelocked() const { return m_state == writelocked; }
};

class TestMutex
{
 private:
  state_type m_state;

 public:
  TestMutex() : m_state(unlocked) { }

  void lock()
  {
    assert(m_state == unlocked);
    m_state = writelocked;
  }
  void unlock()
  {
    assert(m_state == writelocked);
    m_state = unlocked;
  }

 public:
  bool is_unlocked() const { return m_state == unlocked; }
  bool is_readlocked() const { return m_state == writelocked; }
  bool is_writelocked() const { return m_state == writelocked; }
};

// Test-only owner that keeps the wrapped Unlocked base inaccessible while exposing the
// access types that form its public locking interface.
template <typename T, typename POLICY_MUTEX>
class TestUnlockedOwner : protected threadsafe::Unlocked<T, POLICY_MUTEX>
{
 public:
  using data_type = T;
  using policy_type = POLICY_MUTEX;
  using unlocked_type = threadsafe::Unlocked<T, policy_type>;
  using crat = unlocked_type::crat;
  using rat = unlocked_type::rat;
  using wat = unlocked_type::wat;
  using w2rCarry = unlocked_type::w2rCarry;

  TestUnlockedOwner() = default;

  // Preserve Unlocked's value-constructor forwarding without exposing an Unlocked conversion.
  template <typename FirstArg, typename... Rest>
    requires(!std::same_as<std::remove_cvref_t<FirstArg>, TestUnlockedOwner> && std::constructible_from<unlocked_type, FirstArg, Rest...>)
  TestUnlockedOwner(FirstArg&& first, Rest&&... rest) : unlocked_type(std::forward<FirstArg>(first), std::forward<Rest>(rest)...)
  {
  }

  // Copying and moving an owner delegate to Unlocked so the source is locked by the
  // library's normal copy/move machinery and the destination gets a fresh mutex.
  TestUnlockedOwner(TestUnlockedOwner const& other) : unlocked_type(as_unlocked(other)) { }
  TestUnlockedOwner(TestUnlockedOwner&& other) : unlocked_type(std::move(as_unlocked(other))) { }

  // Read-to-write retry support remains an explicit part of a custom owner's public interface.
  void rd2wryield()
    requires requires(unlocked_type& unlocked) { unlocked.rd2wryield(); }
  {
    as_unlocked(*this).rd2wryield();
  }

 private:
  static unlocked_type& as_unlocked(TestUnlockedOwner& owner) { return static_cast<unlocked_type&>(owner); }
  static unlocked_type const& as_unlocked(TestUnlockedOwner const& owner) { return static_cast<unlocked_type const&>(owner); }

  // These narrow probes expose only test mutex state, never the protected value.
  bool test_is_unlocked() const
  {
    if constexpr (std::same_as<policy_type, policy::OneThread> || std::same_as<policy_type, policy::OneThreadRef>)
      return true;
    else
      return this->mutex().is_unlocked();
  }

  bool test_is_readlocked() const
  {
    if constexpr (std::same_as<policy_type, policy::OneThread> || std::same_as<policy_type, policy::OneThreadRef>)
      return true;
    else
      return this->mutex().is_readlocked();
  }

  bool test_is_writelocked() const
  {
    if constexpr (std::same_as<policy_type, policy::OneThread> || std::same_as<policy_type, policy::OneThreadRef>)
      return true;
    else
      return this->mutex().is_writelocked();
  }

  // AccessKey can only be minted by threadsafe access classes. These hidden friends
  // therefore adapt this owner without adding a public getter for its Unlocked base.
  friend unlocked_type& threadsafe_access(TestUnlockedOwner& owner, threadsafe::AccessKey<unlocked_type>) { return as_unlocked(owner); }

  friend unlocked_type const& threadsafe_access(TestUnlockedOwner const& owner, threadsafe::AccessKey<unlocked_type>) { return as_unlocked(owner); }

  friend bool is_unlocked(TestUnlockedOwner const& owner) { return owner.test_is_unlocked(); }
  friend bool is_readlocked(TestUnlockedOwner const& owner) { return owner.test_is_readlocked(); }
  friend bool is_writelocked(TestUnlockedOwner const& owner) { return owner.test_is_writelocked(); }
};

// Hack access to TestRWMutex.
template <typename UNLOCKED>
class LockAccess : public UNLOCKED
{
 private:
  bool is_unlocked()
  {
    if constexpr (std::is_same_v<policy::OneThread, typename UNLOCKED::policy_type> || std::is_same_v<policy::OneThreadRef, typename UNLOCKED::policy_type>)
      return true;
    else
      return this->mutex().is_unlocked(); // Call protected member of base class.
  }
  bool is_readlocked()
  {
    if constexpr (std::is_same_v<policy::OneThread, typename UNLOCKED::policy_type> || std::is_same_v<policy::OneThreadRef, typename UNLOCKED::policy_type>)
      return true;
    else
      return this->mutex().is_readlocked();
  }
  bool is_writelocked()
  {
    if constexpr (std::is_same_v<policy::OneThread, typename UNLOCKED::policy_type> || std::is_same_v<policy::OneThreadRef, typename UNLOCKED::policy_type>)
      return true;
    else
      return this->mutex().is_writelocked();
  }

 public:
  bool is_unlocked() const { return const_cast<LockAccess<UNLOCKED>*>(this)->is_unlocked(); }
  bool is_readlocked() const { return const_cast<LockAccess<UNLOCKED>*>(this)->is_readlocked(); }
  bool is_writelocked() const { return const_cast<LockAccess<UNLOCKED>*>(this)->is_writelocked(); }
};

template <typename UNLOCKED>
  requires requires { typename UNLOCKED::policy_type; }
bool is_unlocked(UNLOCKED const& wrapper)
{
  return static_cast<LockAccess<UNLOCKED> const&>(wrapper).is_unlocked();
}

template <typename UNLOCKED>
  requires requires { typename UNLOCKED::policy_type; }
bool is_readlocked(UNLOCKED const& wrapper)
{
  return static_cast<LockAccess<UNLOCKED> const&>(wrapper).is_readlocked();
}

template <typename UNLOCKED>
  requires requires { typename UNLOCKED::policy_type; }
bool is_writelocked(UNLOCKED const& wrapper)
{
  return static_cast<LockAccess<UNLOCKED> const&>(wrapper).is_writelocked();
}

// Hack access to m_unlocked.
template <typename UNLOCKED>
class AccessUnlocked : public UNLOCKED::crat
{
 public:
  bool is_unlocked() const { return ::is_unlocked(*this->m_unlocked); }
  bool is_readlocked() const { return ::is_readlocked(*this->m_unlocked); }
  bool is_writelocked() const { return ::is_writelocked(*this->m_unlocked); }
};

template <typename UNLOCKED>
bool is_unlocked(typename UNLOCKED::crat const& access)
{
  AccessUnlocked<UNLOCKED> const& a = static_cast<AccessUnlocked<UNLOCKED> const&>(access);
  return a.is_unlocked();
}

template <typename UNLOCKED>
bool is_readlocked(typename UNLOCKED::crat const& access)
{
  AccessUnlocked<UNLOCKED> const& a = static_cast<AccessUnlocked<UNLOCKED> const&>(access);
  return a.is_readlocked();
}

template <typename UNLOCKED>
bool is_writelocked(typename UNLOCKED::crat const& access)
{
  AccessUnlocked<UNLOCKED> const& a = static_cast<AccessUnlocked<UNLOCKED> const&>(access);
  return a.is_writelocked();
}

template <typename UNLOCKED>
void func_read_const(typename UNLOCKED::crat const& access) // ConstAccess<UnlockedBase, .. --> ConstAccess<ConstUnlockedBase, ..
{
  std::cout << access->x << std::endl;
  assert(is_readlocked<UNLOCKED>(access) || is_writelocked<UNLOCKED>(access));
}

template <typename UNLOCKED>
void func_read_and_then_write(typename UNLOCKED::rat& access)
{
  static constexpr bool test_readwrite = std::is_same_v<typename UNLOCKED::policy_type, policy::ReadWrite<TestRWMutex>> ||
                                         std::is_same_v<typename UNLOCKED::policy_type, policy::ReadWriteRef<TestRWMutex>>;
  std::cout << access->x << std::endl;
  assert(is_readlocked<UNLOCKED>(access) || is_writelocked<UNLOCKED>(access));
  if constexpr (test_readwrite)
  {
    typename UNLOCKED::wat write_access(access);    // This might throw if is_readlocked(access).
    write_access->x = 6;
    assert(is_writelocked<UNLOCKED>(access));
  }
  else
  {
    typename UNLOCKED::wat const& write_access = wat_cast(access);
    write_access->x = 6;
    assert(is_writelocked<UNLOCKED>(access));
  }
}

template <typename UNLOCKED>
void func_write(typename UNLOCKED::wat const& access)
{
  access->x = 5;
  assert(is_writelocked<UNLOCKED>(access));
}

template <typename onethread_t, typename primitive_t, typename readwrite_t>
void do_const_access_test(onethread_t const& onethread, primitive_t const& primitive, readwrite_t const& readwrite)
{
  // Reading
  {
    typename onethread_t::crat onethread_r(onethread);
    assert(onethread_r->x == 111);
  }
  {
    typename primitive_t::crat primitive_r(primitive);
    assert(primitive_r->x == 222);
  }
  {
    typename readwrite_t::crat readwrite_r(readwrite);
    assert(readwrite_r->x == 333);
  }
}

template <typename onethread_t, typename primitive_t, typename readwrite_t>
void do_access_test(onethread_t& onethread, primitive_t& primitive, readwrite_t& readwrite)
{
  // Writing
  {
    typename onethread_t::wat onethread_w(onethread);
    onethread_w->x = 111;
  }
  {
    typename primitive_t::wat primitive_w(primitive);
    primitive_w->x = 222;
  }
  {
    typename readwrite_t::wat readwrite_w(readwrite);
    readwrite_w->x = 333;
  }

  // Reading
  {
    typename onethread_t::rat onethread_r(onethread);
    assert(onethread_r->x == 111);
  }
  {
    typename primitive_t::rat primitive_r(primitive);
    assert(primitive_r->x == 222);
  }
  {
    typename readwrite_t::rat readwrite_r(readwrite);
    assert(readwrite_r->x == 333);
  }

  do_const_access_test(onethread, primitive, readwrite);

  // Conversions, write to read access.
  {
    typename onethread_t::wat onethread_w(onethread);
    onethread_w->x = 444;
    typename onethread_t::rat const& onethread_r(onethread_w);
    assert(onethread_r->x == 444);
  }
  {
    typename primitive_t::wat primitive_w(primitive);
    primitive_w->x = 555;
    typename primitive_t::rat const& primitive_r(primitive_w);
    assert(primitive_r->x == 555);
  }
  {
    typename readwrite_t::wat readwrite_w(readwrite);
    readwrite_w->x = 666;
    typename readwrite_t::rat const& readwrite_r(readwrite_w);
    assert(readwrite_r->x == 666);
  }

  // Conversions, read to write access.
  {
    typename onethread_t::rat onethread_r(onethread);
    typename onethread_t::wat const& onethread_w = wat_cast(onethread_r);
    onethread_w->x = 777;
    assert(onethread_r->x == 777);
  }
  {
    typename primitive_t::rat primitive_r(primitive);
    typename primitive_t::wat const& primitive_w = wat_cast(primitive_r);
    primitive_w->x = 888;
    assert(primitive_r->x == 888);
  }
  {
    typename readwrite_t::rat readwrite_r(readwrite);
    typename readwrite_t::wat readwrite_w(readwrite_r);
    readwrite_w->x = 999;
    assert(readwrite_r->x == 999);
  }
}

template <typename UNLOCKED>
void do_unlocked_test(UNLOCKED& wrapper)
{
  static constexpr bool test_readwrite = std::is_same_v<typename UNLOCKED::policy_type, policy::ReadWrite<TestRWMutex>> ||
                                         std::is_same_v<typename UNLOCKED::policy_type, policy::ReadWriteRef<TestRWMutex>>;
  static constexpr bool test_readonly = utils::is_specialization_of<UNLOCKED, ConstUnlockedBase>;

  UNLOCKED const& const_wrapper(wrapper);

  if constexpr (!test_readonly)
  {
    // Things that should compile.
    {
      // Getting write access to non-const wrapper.
      typename UNLOCKED::wat write_access(wrapper);
      write_access->x = 3;
      assert(is_writelocked(wrapper));
    }
    assert(is_unlocked(wrapper));
  }
  {
    // Getting read only access to const wrapper.
    typename UNLOCKED::crat read_access(const_wrapper);
    std::cout << read_access->x << std::endl;
    assert(is_readlocked(wrapper));
  }
  assert(is_unlocked(wrapper));
  {
    // Creating a crat from a non-const wrapper.
    typename UNLOCKED::crat read_access(wrapper);
    std::cout << read_access->x << std::endl;
    assert(is_readlocked(wrapper));
  }
  assert(is_unlocked(wrapper));
  if constexpr (!test_readonly)
  {
    {
      // Getting first read access to non-const wrapper, and then write access.
      do
      {
        try
        {
          typename UNLOCKED::rat read_access(wrapper);
          std::cout << read_access->x << std::endl;
          assert(is_readlocked(wrapper));
          if constexpr (test_readwrite)
          {
            typename UNLOCKED::wat write_access(read_access);  // This might throw.
            write_access->x = 4;
            assert(is_writelocked(wrapper));
          }
          else
          {
            typename UNLOCKED::wat const& write_access = wat_cast(read_access);
            write_access->x = 4;
            assert(is_writelocked(wrapper));
          }
        }
        catch (std::exception const&)
        {
          if constexpr (test_readwrite)
          {
            wrapper.rd2wryield();    // Block until the other thread that tries to convert a read to write lock succeeded.
            // Try again.
            continue;
          }
        }
        break;
      }
      while (test_readwrite);
    }
    assert(is_unlocked(wrapper));
  }
  {
    // Passing a crat to func_read_const
    typename UNLOCKED::crat read_access_const(const_wrapper); // OK
    func_read_const<UNLOCKED>(read_access_const);
    assert(is_readlocked(wrapper));
  }
  assert(is_unlocked(wrapper));
  if constexpr (!test_readonly)
  {
    {
      // Passing a rat to func_read_const
      typename UNLOCKED::rat read_access(wrapper);   // OK
      func_read_const<UNLOCKED>(read_access);
      assert(is_readlocked(wrapper));
    }
    assert(is_unlocked(wrapper));
    {
      // Passing a wat to func_read_const
      typename UNLOCKED::wat write_access(wrapper);   // OK
      func_read_const<UNLOCKED>(write_access);
      assert(is_writelocked(wrapper));
    }
    assert(is_unlocked(wrapper));
    {
      do
      {
        try
        {
          // Passing a rat to func_read
          typename UNLOCKED::rat read_access(wrapper);  // OK
          func_read_and_then_write<UNLOCKED>(read_access); // This might throw.
          assert(is_readlocked(wrapper));
        }
        catch (std::exception const&)
        {
          if constexpr (test_readwrite)
          {
            wrapper.rd2wryield();
            continue;
          }
        }
        break;
      }
      while (test_readwrite);
    }
    if constexpr (test_readwrite)
    {
      assert(is_unlocked(wrapper));
      {
        typename UNLOCKED::w2rCarry carry(wrapper);
        assert(is_unlocked(wrapper));
        func_write<UNLOCKED>(typename UNLOCKED::wat(carry));
        assert(is_readlocked(wrapper));
        {
          typename UNLOCKED::rat read_access(carry);
          func_read_const<UNLOCKED>(read_access);
          assert(is_readlocked(wrapper));
        }
        assert(is_readlocked(wrapper));
      }
    }
    assert(is_unlocked(wrapper));
    {
      // Passing a wat to func_read
      typename UNLOCKED::wat write_access(wrapper);   // OK
      func_read_and_then_write<UNLOCKED>(write_access);
      assert(is_writelocked(wrapper));
    }
    assert(is_unlocked(wrapper));
    {
      // Passing a wat to func_write
      typename UNLOCKED::wat write_access(wrapper);   // OK
      func_write<UNLOCKED>(write_access);
      assert(is_writelocked(wrapper));
    }
    assert(is_unlocked(wrapper));
  }

  std::cout << "Success!" << std::endl;

  // Things that should not compile:
#ifdef TEST1
  {
    // Getting write access to a const wrapper.
    typename UNLOCKED::wat fail(const_wrapper);   // TEST1 FAIL (error: no matching constructor for initialization of 'UNLOCKED::wat' (aka
                                                // 'WriteAccess<threadsafe::Unlocked<Foo, threadsafe::policy::ReadWrite<TestRWMutex>>>')).
  }
#endif
#ifdef TEST2
  {
    // Creating a rat from a const wrapper.
    typename UNLOCKED::rat fail(const_wrapper); // TEST2 FAIL (error: no matching constructor for initialization of 'typename UNLOCKED::rat' (aka
                                                // 'ReadAccess<threadsafe::Unlocked<Foo, threadsafe::policy::ReadWrite<TestRWMutex>>>')).
  }
#endif
#ifdef TEST3
  {
    // Getting write access from wat.
    typename UNLOCKED::wat write_access(wrapper); // OK
    typename UNLOCKED::wat fail(write_access); // TEST3 FAIL (error: call to implicitly-deleted copy constructor of 'UNLOCKED::wat' (aka
                                               // 'WriteAccess<threadsafe::Unlocked<Foo, threadsafe::policy::ReadWrite<TestRWMutex>>>')).
  }
#endif
#ifdef TEST4
  {
    // Getting write access from crat.
    typename UNLOCKED::crat read_access_const(const_wrapper); // OK
    typename UNLOCKED::wat fail(read_access_const); // TEST4 FAIL (error: no matching constructor for initialization of 'UNLOCKED::wat' (aka
                                                    // 'WriteAccess<threadsafe::Unlocked<Foo, threadsafe::policy::ReadWrite<TestRWMutex>>>')).
  }
#endif
#ifdef TEST5
  {
    // Write to something that you only have read access too.
    typename UNLOCKED::crat read_access_const(const_wrapper); // OK
    read_access_const->x = -1; // TEST5 FAIL (error: cannot assign to return value because function 'operator->' returns a const value).
  }
#endif
#ifdef TEST6
  {
    // Write to something that you only have read access too.
    typename UNLOCKED::rat read_access(wrapper); // OK
    read_access->x = -1; // TEST6 FAIL (error: cannot assign to return value because function 'operator->' returns a const value).
  }
#endif
#ifdef TEST7
  {
    // Create crat from crat.
    typename UNLOCKED::crat read_access_const(const_wrapper); // OK
    typename UNLOCKED::crat fail(read_access_const); // TEST7 FAIL (error: call to deleted constructor of 'UNLOCKED::crat' (aka
                                                     // 'ConstReadAccess<threadsafe::Unlocked<Foo, threadsafe::policy::ReadWrite<TestRWMutex>>>')).
  }
#endif
#ifdef TEST8
  {
    // Create crat from rat.
    typename UNLOCKED::rat read_access(wrapper); // OK
    typename UNLOCKED::crat fail(read_access); // TEST8 FAIL (error: call to deleted constructor of 'UNLOCKED::crat' (aka
                                               // 'ConstReadAccess<threadsafe::Unlocked<Foo, threadsafe::policy::ReadWrite<TestRWMutex>>>')).
  }
#endif
#ifdef TEST9
  {
    // Create crat from wat.
    typename UNLOCKED::wat write_access(wrapper); // OK
    typename UNLOCKED::crat fail(write_access); // TEST9 FAIL (error: call to deleted constructor of 'UNLOCKED::crat' (aka
                                                // 'ConstReadAccess<threadsafe::Unlocked<Foo, threadsafe::policy::ReadWrite<TestRWMutex>>>')).
  }
#endif
#ifdef TEST10
  {
    // Create rat from crat.
    typename UNLOCKED::crat read_access_const(const_wrapper); // OK
    typename UNLOCKED::rat fail(read_access_const); // TEST10 FAIL (error: no matching constructor for initialization of 'UNLOCKED::rat' (aka
                                                    // 'ReadAccess<threadsafe::Unlocked<Foo, threadsafe::policy::ReadWrite<TestRWMutex>>>')).
  }
#endif
#ifdef TEST11
  {
    // Create rat from rat.
    typename UNLOCKED::rat read_access(wrapper); // OK
    typename UNLOCKED::rat fail(read_access); // TEST11 FAIL (error: call to implicitly-deleted copy constructor of 'UNLOCKED::rat' (aka
                                              // 'ReadAccess<threadsafe::Unlocked<Foo, threadsafe::policy::ReadWrite<TestRWMutex>>>')).
  }
#endif
#ifdef TEST12
  {
    // Create rat from wat.
    typename UNLOCKED::wat write_access(wrapper); // OK
    typename UNLOCKED::rat fail(write_access); // TEST12 FAIL (error: call to implicitly-deleted copy constructor of 'UNLOCKED::rat' (aka
                                               // 'ReadAccess<threadsafe::Unlocked<Foo, threadsafe::policy::ReadWrite<TestRWMutex>>>')).
  }
#endif
#ifdef TEST13
  {
    // Passing a crat to func_read.
    typename UNLOCKED::crat read_access_const(const_wrapper); // OK
    func_read_and_then_write(read_access_const); // TEST13 FAIL (error: no matching function for call to 'func_read_and_then_write').
  }
#endif
#ifdef TEST14
  {
    // Passing a crat to func_write.
    typename UNLOCKED::crat read_access_const(const_wrapper); // OK
    func_write(read_access_const); // TEST14 FAIL (error: no matching function for call to 'func_write').
  }
#endif
#ifdef TEST15
  {
    // Passing a rat to func_write.
    typename UNLOCKED::rat read_access(wrapper); // OK
    func_write(read_access); // TEST15 FAIL (error: no matching function for call to 'func_write').
  }
#endif
#ifdef TEST16
#error That was the last test
#endif
}

template <typename OWNER>
inline constexpr bool owner_access_constructors_supported =
    std::constructible_from<typename OWNER::crat, OWNER const&> && std::constructible_from<typename OWNER::rat, OWNER&> &&
    std::constructible_from<typename OWNER::wat, OWNER&>;

// Exercise the complete access matrix through protected-base owners without exposing
// a public conversion to their underlying Unlocked objects.
template <typename ONETHREAD_OWNER, typename PRIMITIVE_OWNER, typename READWRITE_OWNER>
void do_custom_owner_tests(ONETHREAD_OWNER& onethread, PRIMITIVE_OWNER& primitive, READWRITE_OWNER& readwrite)
{
  static_assert(owner_access_constructors_supported<ONETHREAD_OWNER>);
  static_assert(owner_access_constructors_supported<PRIMITIVE_OWNER>);
  static_assert(owner_access_constructors_supported<READWRITE_OWNER>);
  static_assert(std::constructible_from<typename READWRITE_OWNER::crat, READWRITE_OWNER&>);
  static_assert(!std::constructible_from<typename READWRITE_OWNER::rat, READWRITE_OWNER const&>);
  static_assert(!std::constructible_from<typename READWRITE_OWNER::wat, READWRITE_OWNER const&>);
  static_assert(std::constructible_from<typename READWRITE_OWNER::w2rCarry, READWRITE_OWNER&>);
  static_assert(!std::constructible_from<typename READWRITE_OWNER::w2rCarry, READWRITE_OWNER const&>);
  static_assert(std::constructible_from<typename READWRITE_OWNER::wat, typename READWRITE_OWNER::rat&>);
  static_assert(std::constructible_from<typename READWRITE_OWNER::wat, typename READWRITE_OWNER::w2rCarry&>);
  static_assert(std::constructible_from<typename READWRITE_OWNER::rat, typename READWRITE_OWNER::w2rCarry const&>);
  static_assert(!std::move_constructible<typename READWRITE_OWNER::crat>);
  static_assert(std::move_constructible<typename READWRITE_OWNER::rat>);
  static_assert(std::move_constructible<typename READWRITE_OWNER::wat>);

  do_access_test(onethread, primitive, readwrite);
  do_unlocked_test(readwrite);

  // Owner adapters delegate to the raw constructors, including their lock arguments.
  {
    bool forwarded = false;
    typename READWRITE_OWNER::crat access(std::as_const(readwrite), forwarded);
    assert(forwarded);
    assert(is_readlocked(readwrite));
  }
  {
    bool forwarded = false;
    typename READWRITE_OWNER::rat access(readwrite, forwarded);
    assert(forwarded);
    assert(is_readlocked(readwrite));
  }
  {
    bool forwarded = false;
    typename READWRITE_OWNER::wat access(readwrite, forwarded);
    assert(forwarded);
    assert(is_writelocked(readwrite));
  }

  // Moving owner-created access objects transfers responsibility for the existing lock.
  {
    typename READWRITE_OWNER::rat original(readwrite);
    typename READWRITE_OWNER::rat moved(std::move(original));
    assert(is_readlocked(readwrite));
  }
  {
    typename READWRITE_OWNER::wat original(readwrite);
    typename READWRITE_OWNER::wat moved(std::move(original));
    assert(is_writelocked(readwrite));
  }

  do_unlocked_test(onethread);
  do_unlocked_test(primitive);
}

struct Foo
{
  int x;
};

struct Doo : Foo
{
  int y;
};

struct FooRF : AIRefCount
{
  int x;
};

struct DooRF : FooRF
{
  int y;
};

struct locked_TFoo;

using TFoo = threadsafe::UnlockedTrackedObject<locked_TFoo, threadsafe::policy::ReadWrite<AIReadWriteMutex>>;
using TFooTracker = threadsafe::ObjectTracker<TFoo, locked_TFoo, threadsafe::policy::ReadWrite<AIReadWriteMutex>>;

struct locked_TFoo : threadsafe::TrackedObject<TFoo, TFooTracker>
{
  int x;
};

int main()
{
  std::cout << "Testing size and alignment... " << std::flush;
  do_size_test<1>();
  do_size_test<2>();
  do_size_test<3>();
  do_size_test<4>();
  do_size_test<5>();
  do_size_test<6>();
  do_size_test<7>();
  do_size_test<8>();
  do_size_test<9>();
  do_size_test<10>();
  do_size_test<11>();
  do_size_test<12>();
  do_size_test<13>();
  do_size_test<14>();
  do_size_test<15>();
  do_size_test<16>();
  do_size_test<17>();
  do_size_test<18>();
  do_size_test<19>();
  do_size_test<20>();
  do_size_test<21>();
  do_size_test<22>();
  do_size_test<23>();
  do_size_test<24>();
  do_size_test<25>();
  std::cout << "OK" << std::endl;

  // ThreadSafe compile tests.

  {
    using unlocked_Foo_onethread_t = Unlocked<Foo, policy::OneThread>;
    using unlocked_Foo_primitive_t = Unlocked<Foo, policy::Primitive<std::mutex>>;
    using unlocked_Foo_readwrite_t = Unlocked<Foo, policy::ReadWrite<AIReadWriteMutex>>;

    unlocked_Foo_onethread_t unlocked_Foo_onethread;
    unlocked_Foo_primitive_t unlocked_Foo_primitive;
    unlocked_Foo_readwrite_t unlocked_Foo_readwrite;

    // Copying is only possible using an access type.
    unlocked_Foo_readwrite_t unlocked_Foo_readwrite3(unlocked_Foo_readwrite_t::crat{unlocked_Foo_readwrite});
    // Moving is possible without.
    unlocked_Foo_onethread_t unlocked_Foo_onethread2(std::move(unlocked_Foo_onethread));
    unlocked_Foo_primitive_t unlocked_Foo_primitive2(std::move(unlocked_Foo_primitive));
    unlocked_Foo_readwrite_t unlocked_Foo_readwrite2(std::move(unlocked_Foo_readwrite));

    do_access_test(unlocked_Foo_onethread, unlocked_Foo_primitive, unlocked_Foo_readwrite);
  }

  {
    using owned_Doo_onethread_t = TestUnlockedOwner<Doo, policy::OneThread>;
    using owned_Doo_primitive_t = TestUnlockedOwner<Doo, policy::Primitive<TestMutex>>;
    using owned_Doo_readwrite_t = TestUnlockedOwner<Doo, policy::ReadWrite<TestRWMutex>>;

    using const_onethread_base_t = ConstUnlockedBase<Foo, owned_Doo_onethread_t::policy_type>;
    using const_primitive_base_t = ConstUnlockedBase<Foo, owned_Doo_primitive_t::policy_type>;
    using const_readwrite_base_t = ConstUnlockedBase<Foo, owned_Doo_readwrite_t::policy_type>;
    static_assert(sizeof(AccessKey<const_onethread_base_t>) > 0);
    static_assert(sizeof(AccessKey<const_primitive_base_t>) > 0);
    static_assert(sizeof(AccessKey<const_readwrite_base_t>) > 0);
    static_assert(std::constructible_from<typename const_onethread_base_t::crat, const_onethread_base_t const&>);
    static_assert(std::constructible_from<typename const_primitive_base_t::crat, const_primitive_base_t const&>);
    static_assert(std::constructible_from<typename const_readwrite_base_t::crat, const_readwrite_base_t const&>);

    // UnlockedBase currently accepts native Unlocked objects, not protected-base
    // owners. The native base/reference coverage below remains authoritative.
    static_assert(!std::constructible_from<UnlockedBase<Foo, owned_Doo_onethread_t::policy_type>, owned_Doo_onethread_t&>);
    static_assert(!std::constructible_from<ConstUnlockedBase<Foo, owned_Doo_onethread_t::policy_type>, owned_Doo_onethread_t const&>);

    owned_Doo_onethread_t owned_Doo_onethread_source;
    owned_Doo_onethread_t owned_Doo_onethread_copy(owned_Doo_onethread_source);
    owned_Doo_onethread_t owned_Doo_onethread(std::move(owned_Doo_onethread_copy));

    owned_Doo_primitive_t owned_Doo_primitive_source;
    owned_Doo_primitive_t owned_Doo_primitive_copy(owned_Doo_primitive_source);
    owned_Doo_primitive_t owned_Doo_primitive(std::move(owned_Doo_primitive_copy));

    // Owner copy/move itself remains independent from access adaptation.
    owned_Doo_readwrite_t owned_Doo_readwrite_source;
    owned_Doo_readwrite_t owned_Doo_readwrite_copy(owned_Doo_readwrite_source);
    owned_Doo_readwrite_t owned_Doo_readwrite(std::move(owned_Doo_readwrite_copy));

    do_custom_owner_tests(owned_Doo_onethread, owned_Doo_primitive, owned_Doo_readwrite);
  }

  {
    using unlocked_Doo_onethread_t = Unlocked<Doo, policy::OneThread>;
    using unlocked_Doo_primitive_t = Unlocked<Doo, policy::Primitive<TestMutex>>;
    using unlocked_Doo_readwrite_t = Unlocked<Doo, policy::ReadWrite<TestRWMutex>>;

    unlocked_Doo_onethread_t unlocked_Doo_onethread;
    unlocked_Doo_primitive_t unlocked_Doo_primitive;
    unlocked_Doo_readwrite_t unlocked_Doo_readwrite;
    // Test "copy constructor".
    unlocked_Doo_readwrite_t unlocked_Doo_readwrite2(unlocked_Doo_readwrite_t::crat{unlocked_Doo_readwrite});
    unlocked_Doo_readwrite_t unlocked_Doo_readwrite3(unlocked_Doo_readwrite_t::wat{unlocked_Doo_readwrite});

    do_unlocked_test(unlocked_Doo_onethread);
    do_unlocked_test(unlocked_Doo_primitive);
    do_unlocked_test(unlocked_Doo_readwrite);

    UnlockedBase<Foo, unlocked_Doo_onethread_t::policy_type> unlockedbase_Foo_onethread(unlocked_Doo_onethread);
    UnlockedBase<Foo, unlocked_Doo_primitive_t::policy_type> unlockedbase_Foo_primitive(unlocked_Doo_primitive);
    UnlockedBase<Foo, unlocked_Doo_readwrite_t::policy_type> unlockedbase_Foo_readwrite(unlocked_Doo_readwrite);
    // Test copy constructor.
    UnlockedBase<Foo, unlocked_Doo_readwrite_t::policy_type> unlockedbase_Foo_readwrite2(unlockedbase_Foo_readwrite);

    do_access_test(unlockedbase_Foo_onethread, unlockedbase_Foo_primitive, unlockedbase_Foo_readwrite);

    do_unlocked_test(unlockedbase_Foo_onethread);
    do_unlocked_test(unlockedbase_Foo_primitive);
    do_unlocked_test(unlockedbase_Foo_readwrite);

    ConstUnlockedBase<Foo, unlocked_Doo_onethread_t::policy_type> const_unlockedbase_Foo_onethread(unlocked_Doo_onethread);
    ConstUnlockedBase<Foo, unlocked_Doo_primitive_t::policy_type> const_unlockedbase_Foo_primitive(unlocked_Doo_primitive);
    ConstUnlockedBase<Foo, unlocked_Doo_readwrite_t::policy_type> const_unlockedbase_Foo_readwrite(unlocked_Doo_readwrite);
    // Test copy constructor.
    ConstUnlockedBase<Foo, unlocked_Doo_readwrite_t::policy_type> const_unlockedbase_Foo_readwrite2(const_unlockedbase_Foo_readwrite);

    do_unlocked_test(const_unlockedbase_Foo_onethread);
    do_unlocked_test(const_unlockedbase_Foo_primitive);
    do_unlocked_test(const_unlockedbase_Foo_readwrite);
  }

  using unlocked_DooRF_onethread_t = Unlocked<DooRF, policy::OneThread>;
  using unlocked_DooRF_primitive_t = Unlocked<DooRF, policy::Primitive<TestMutex>>;
  using unlocked_DooRF_readwrite_t = Unlocked<DooRF, policy::ReadWrite<TestRWMutex>>;

  boost::intrusive_ptr<unlocked_DooRF_onethread_t> unlocked_DooRF_onethread = new unlocked_DooRF_onethread_t;
  boost::intrusive_ptr<unlocked_DooRF_primitive_t> unlocked_DooRF_primitive = new unlocked_DooRF_primitive_t;
  boost::intrusive_ptr<unlocked_DooRF_readwrite_t> unlocked_DooRF_readwrite = new unlocked_DooRF_readwrite_t;

  using unlockedbase_FooRF_onethread_t = UnlockedBase<FooRF, unlocked_DooRF_onethread_t::policy_type>;
  using unlockedbase_FooRF_primitive_t = UnlockedBase<FooRF, unlocked_DooRF_primitive_t::policy_type>;
  using unlockedbase_FooRF_readwrite_t = UnlockedBase<FooRF, unlocked_DooRF_readwrite_t::policy_type>;

  unlockedbase_FooRF_onethread_t unlockedbase_FooRF_onethread(*unlocked_DooRF_onethread);
  unlockedbase_FooRF_primitive_t unlockedbase_FooRF_primitive(*unlocked_DooRF_primitive);
  unlockedbase_FooRF_readwrite_t unlockedbase_FooRF_readwrite(*unlocked_DooRF_readwrite);
  // Test copy constructor.
  unlockedbase_FooRF_readwrite_t unlockedbase_FooRF_readwrite2(unlockedbase_FooRF_readwrite);

  do_unlocked_test(unlockedbase_FooRF_onethread);
  do_unlocked_test(unlockedbase_FooRF_primitive);
  do_unlocked_test(unlockedbase_FooRF_readwrite);

  // Test thread-safe ObjectTracker.
  TFoo tfoo;
  TFooTracker& tfoo_tracker = tfoo.tracker();

  locked_TFoo* hack_access;

  {
    TFoo::wat tfoo_w{tfoo};
    tfoo_w->x = 1234;
    hack_access = &*tfoo_w;
  }

  TFoo tfoo2(std::move(tfoo));
  hack_access->x = 666;

  {
    auto tfoo_r{tfoo_tracker.tracked_rat()};
    std::cout << "tfoo_r->x = " << tfoo_r->x << std::endl;
    ASSERT(tfoo_r->x == 1234);
  }
  std::weak_ptr<TFooTracker> tfoo_tracker2 = tfoo2;
  {
    auto tracker = tfoo_tracker2.lock();
    auto tfoo_w{tracker->tracked_wat()};
    std::cout << "tfoo_w->x = " << tfoo_w->x << std::endl;
    ASSERT(tfoo_w->x == 1234);
    tfoo_w->x = 5678;
  }
  TFoo tfoo3(std::move(tfoo2));
  {
    TFoo::crat tfoo_r{tfoo3};
    std::cout << "tfoo_w->x = " << tfoo_r->x << std::endl;
    ASSERT(tfoo_r->x == 5678);
  }
}
