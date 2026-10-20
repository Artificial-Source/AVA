#pragma once

#include "ava/debug/print_members_on.h"
#include "threadsafe/threadsafe.h"

#include <mutex>
#include <utility>

namespace ava::app::frontend {

template <typename T>
class UnlockedData;

namespace detail {

// Publishes one possible data change after its associated write access has been destroyed.
template <typename T>
class DataNotifier
{
 public:
  // Start disarmed so a failure while constructing the lock-owning Access base does not publish a change.
  explicit DataNotifier(UnlockedData<T>& data) : data_(&data) { }
  DataNotifier(DataNotifier const&) = delete;
  DataNotifier& operator=(DataNotifier const&) = delete;
  // Transfer the one pending notification and leave the moved-from guard inert.
  DataNotifier(DataNotifier&& other) noexcept : data_(std::exchange(other.data_, nullptr)), armed_(std::exchange(other.armed_, false)) { }
  DataNotifier& operator=(DataNotifier&&) = delete;
  ~DataNotifier();

 protected:
  void arm() noexcept { armed_ = true; }

 private:
  UnlockedData<T>* data_;
  bool armed_ = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// DataNotifier is deliberately first and Access second so reverse base destruction unlocks before publishing.
template <typename T, class UNLOCKED>
struct NotifyingAccess : public DataNotifier<T>, public threadsafe::Access<UNLOCKED>
{
  using access_type = threadsafe::Access<UNLOCKED>;

  // Compatibility alias for callers that previously had to unwrap the composed access object.
  access_type& write_access;

  // Arm notification only after Access has successfully acquired the primitive lock.
  template <typename... Args>
  explicit NotifyingAccess(UnlockedData<T>& data, Args&&... args)
      : DataNotifier<T>(data), access_type(UnlockedData<T>::as_unlocked(data), std::forward<Args>(args)...), write_access(*this)
  {
    this->arm();
  }

  // Recover the owner only from its actual protected Unlocked base subobject.
  template <typename... Args>
  explicit NotifyingAccess(UNLOCKED& unlocked, Args&&... args) : NotifyingAccess(UnlockedData<T>::from_unlocked(unlocked), std::forward<Args>(args)...)
  {
  }

  NotifyingAccess(NotifyingAccess const&) = delete;
  NotifyingAccess& operator=(NotifyingAccess const&) = delete;
  NotifyingAccess(NotifyingAccess&& other) noexcept : DataNotifier<T>(std::move(other)), access_type(std::move(other)), write_access(*this) { }
  NotifyingAccess& operator=(NotifyingAccess&&) = delete;

  typename UNLOCKED::data_type* operator->() const { return this->m_unlocked->ptr(); }
  typename UNLOCKED::data_type& operator*() const { return *this->m_unlocked->ptr(); }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

template <typename T>
class PrimitiveDataPolicy
{
 public:
  using reference_type = threadsafe::policy::PrimitiveRef<std::mutex>;

  template <class UNLOCKED>
  struct access_types
  {
    using const_read_access_type = threadsafe::AccessConst<UNLOCKED>;
    using read_access_type = threadsafe::ConstAccess<UNLOCKED>;
    using write_access_type = NotifyingAccess<T, UNLOCKED>;
    using write_to_read_carry = threadsafe::policy::unsupported_w2rCarry<UNLOCKED>;
    using read_access_base_type = threadsafe::AccessConst<UNLOCKED>;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 protected:
  template <class UNLOCKED>
  friend struct threadsafe::AccessConst;
  template <class UNLOCKED>
    requires utils::is_specialization_of_v<UNLOCKED, threadsafe::Unlocked> || utils::is_specialization_of_v<UNLOCKED, threadsafe::UnlockedBase>
  friend struct threadsafe::Access;

  mutable std::mutex mutex_;

  std::mutex& mutex() const { return mutex_; }
};

} // namespace detail
} // namespace ava::app::frontend
