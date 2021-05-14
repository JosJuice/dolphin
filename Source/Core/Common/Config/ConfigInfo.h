// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <utility>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Enums.h"

namespace Config
{
namespace detail
{
// std::underlying_type may only be used with enum types, so make sure T is an enum type first.
template <typename T>
using UnderlyingType = typename std::enable_if_t<std::is_enum<T>{}, std::underlying_type<T>>::type;

template <typename T>
constexpr bool HasAtomic = std::is_trivially_copyable_v<T>&& std::is_copy_constructible_v<T>&&
    std::is_move_constructible_v<T>&& std::is_copy_assignable_v<T>&& std::is_move_assignable_v<T>;

template <typename, typename = void>
constexpr bool HasLockFreeAtomic = false;

template <typename T>
constexpr bool HasLockFreeAtomic<T, std::enable_if<HasAtomic<T>>> =
    std::atomic<T>::is_always_lock_free;

}  // namespace detail

struct Location
{
  System system;
  std::string section;
  std::string key;

  bool operator==(const Location& other) const;
  bool operator!=(const Location& other) const;
  bool operator<(const Location& other) const;
};

template <typename T>
struct CachedValue
{
  T value;
  u32 config_version;
};

bool IsConfigVersionLess(u32 lhs, u32 rhs);

template <typename T>
class MutexCachedValue
{
public:
  MutexCachedValue() {}

  MutexCachedValue(T value, u32 config_version) : m_cached_value{value, config_version} {}

  MutexCachedValue(CachedValue<T> cached_value) : m_cached_value{cached_value} {}

  CachedValue<T> GetCachedValue()
  {
    std::shared_lock lock(m_mutex);
    return m_cached_value;
  }

  template <typename U>
  CachedValue<U> GetCachedValueCasted()
  {
    std::shared_lock lock(m_mutex);
    return CachedValue<U>{static_cast<U>(m_cached_value.value), m_cached_value.config_version};
  }

  void SetCachedValue(const CachedValue<T>& cached_value)
  {
    std::unique_lock lock(m_mutex);
    if (IsConfigVersionLess(m_cached_value.config_version, cached_value.config_version))
      m_cached_value = cached_value;
  }

  // Not thread-safe
  MutexCachedValue<T>& operator=(const CachedValue<T>& cached_value)
  {
    m_cached_value = cached_value;
    return *this;
  }

  // Not thread-safe
  MutexCachedValue<T>& operator=(CachedValue<T>&& cached_value)
  {
    m_cached_value = std::move(cached_value);
    return *this;
  }

private:
  CachedValue<T> m_cached_value;
  std::shared_mutex m_mutex;
};

template <typename T>
class AtomicCachedValue
{
public:
  AtomicCachedValue() {}

  AtomicCachedValue(T value, u32 config_version) : m_cached_value{value, config_version} {}

  AtomicCachedValue(CachedValue<T> cached_value) : m_cached_value{cached_value} {}

  CachedValue<T> GetCachedValue() { return m_cached_value.load(std::memory_order_relaxed); }

  template <typename U>
  CachedValue<U> GetCachedValueCasted()
  {
    CachedValue<T> cached_value = m_cached_value.load(std::memory_order_relaxed);
    return CachedValue<U>{static_cast<U>(cached_value.value), cached_value.config_version};
  }

  void SetCachedValue(const CachedValue<T>& cached_value)
  {
    CachedValue<T> old_cached_value = m_cached_value.load(std::memory_order_relaxed);

    while (true)
    {
      if (!IsConfigVersionLess(old_cached_value.config_version, cached_value.config_version))
        return;  // Already up to date

      if (m_cached_value.compare_exchange_weak(old_cached_value, cached_value,
                                               std::memory_order_relaxed))
      {
        return;  // Update succeeded
      }
    }
  }

  AtomicCachedValue<T>& operator=(CachedValue<T> cached_value)
  {
    m_cached_value.store(std::move(cached_value), std::memory_order_relaxed);
    return *this;
  }

private:
  std::atomic<CachedValue<T>> m_cached_value;
};

template <typename T>
using ThreadsafeCachedValue = std::conditional_t<detail::HasLockFreeAtomic<CachedValue<T>>,
                                                 AtomicCachedValue<T>, MutexCachedValue<T>>;

template <typename T>
class Info
{
public:
  constexpr Info(const Location& location, const T& default_value)
      : m_location{location}, m_default_value{default_value}, m_cached_value{default_value, 0}
  {
  }

  Info(const Info<T>& other) { *this = other; }

  // Not thread-safe
  Info(Info<T>&& other) { *this = std::move(other); }

  // Make it easy to convert Info<Enum> into Info<UnderlyingType<Enum>>
  // so that enum settings can still easily work with code that doesn't care about the enum values.
  template <typename Enum,
            std::enable_if_t<std::is_same<T, detail::UnderlyingType<Enum>>::value>* = nullptr>
  Info(const Info<Enum>& other)
  {
    *this = other;
  }

  // Not thread-safe
  Info<T>& operator=(const Info<T>& other)
  {
    m_location = other.GetLocation();
    m_default_value = other.GetDefaultValue();
    m_cached_value = other.GetCachedValue();
    return *this;
  }

  // Not thread-safe
  Info<T>& operator=(Info<T>&& other)
  {
    m_location = std::move(other.m_location);
    m_default_value = std::move(other.m_default_value);
    m_cached_value = std::move(other.m_cached_value);
    return *this;
  }

  // Make it easy to convert Info<Enum> into Info<UnderlyingType<Enum>>
  // so that enum settings can still easily work with code that doesn't care about the enum values.
  template <typename Enum,
            std::enable_if_t<std::is_same<T, detail::UnderlyingType<Enum>>::value>* = nullptr>
  Info<T>& operator=(const Info<Enum>& other)
  {
    m_location = other.GetLocation();
    m_default_value = static_cast<T>(other.GetDefaultValue());
    m_cached_value = other.template GetCachedValueCasted<T>();
    return *this;
  }

  constexpr const Location& GetLocation() const { return m_location; }
  constexpr const T& GetDefaultValue() const { return m_default_value; }

  CachedValue<T> GetCachedValue() const { return m_cached_value.GetCachedValue(); }

  template <typename U>
  CachedValue<U> GetCachedValueCasted() const
  {
    return m_cached_value.template GetCachedValueCasted<U>();
  }

  void SetCachedValue(const CachedValue<T>& cached_value) const
  {
    return m_cached_value.SetCachedValue(cached_value);
  }

private:
  Location m_location;
  T m_default_value;

  mutable ThreadsafeCachedValue<T> m_cached_value;
};
}  // namespace Config
