// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

// Extremely simple serialization framework.

// (mis)-features:
// + Super fast
// + Very simple
// + Same code is used for serialization and deserializaition (in most cases)
// - Zero backwards/forwards compatibility
// - Serialization code for anything complex has to be manually written.

#include <array>
#include <cstddef>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Compiler.h"
#include "Common/Flag.h"
#include "Common/Logging/Log.h"

// XXX: Replace this with std::is_trivially_copyable<T> once we stop using volatile
// on things that are put in savestates, as volatile types are not trivially copyable.
template <typename T>
constexpr bool IsTriviallyCopyable = std::is_trivially_copyable<std::remove_volatile_t<T>>::value;

// Wrapper class
class PointerWrap
{
public:
  enum Mode
  {
    MODE_READ = 1,  // load
    MODE_WRITE,     // save
  };

  PointerWrap(std::vector<u8>* buffer, Mode mode) : m_buffer(buffer), m_mode(mode)
  {
    if (mode == MODE_READ)
      buffer->clear();
  }

  Mode GetMode() const { return m_mode; }

  template <typename K, class V>
  void Do(std::map<K, V>& x)
  {
    u32 count = (u32)x.size();
    Do(count);

    if (m_mode == MODE_READ)
    {
      for (x.clear(); count != 0; --count)
      {
        std::pair<K, V> pair;
        Do(pair.first);
        Do(pair.second);
        x.insert(pair);
      }
    }
    else
    {
      for (auto& elem : x)
      {
        Do(elem.first);
        Do(elem.second);
      }
    }
  }

  template <typename V>
  void Do(std::set<V>& x)
  {
    u32 count = (u32)x.size();
    Do(count);

    if (m_mode == MODE_READ)
    {
      for (x.clear(); count != 0; --count)
      {
        V value;
        Do(value);
        x.insert(value);
      }
    }
    else
    {
      for (const V& val : x)
        Do(val);
    }
  }

  template <typename T>
  void Do(std::vector<T>& x)
  {
    DoContiguousContainer(x);
  }

  template <typename T>
  void Do(std::list<T>& x)
  {
    DoContainer(x);
  }

  template <typename T>
  void Do(std::deque<T>& x)
  {
    DoContainer(x);
  }

  template <typename T>
  void Do(std::basic_string<T>& x)
  {
    DoContiguousContainer(x);
  }

  template <typename T, typename U>
  void Do(std::pair<T, U>& x)
  {
    Do(x.first);
    Do(x.second);
  }

  template <typename T>
  void Do(std::optional<T>& x)
  {
    bool present = x.has_value();
    Do(present);

    if (m_mode == MODE_READ)
    {
      if (present)
      {
        x = std::make_optional<T>();
        Do(x.value());
      }
      else
      {
        x = std::nullopt;
      }
    }
    else
    {
      if (present)
        Do(x.value());
    }
  }

  template <typename T, std::size_t N>
  void DoArray(std::array<T, N>& x)
  {
    DoArray(x.data(), static_cast<u32>(x.size()));
  }

  template <typename T, typename std::enable_if_t<IsTriviallyCopyable<T>, int> = 0>
  void DoArray(T* x, u32 count)
  {
    DoVoid(x, count * sizeof(T));
  }

  template <typename T, typename std::enable_if_t<!IsTriviallyCopyable<T>, int> = 0>
  void DoArray(T* x, u32 count)
  {
    for (u32 i = 0; i < count; ++i)
      Do(x[i]);
  }

  template <typename T, std::size_t N>
  void DoArray(T (&arr)[N])
  {
    DoArray(arr, static_cast<u32>(N));
  }

  void Do(Common::Flag& flag)
  {
    bool s = flag.IsSet();
    Do(s);
    if (m_mode == MODE_READ)
      flag.Set(s);
  }

  template <typename T>
  void Do(std::atomic<T>& atomic)
  {
    T temp = atomic.load();
    Do(temp);
    if (m_mode == MODE_READ)
      atomic.store(temp);
  }

  template <typename T>
  void Do(T& x)
  {
    static_assert(IsTriviallyCopyable<T>, "Only sane for trivially copyable types");
    // Note:
    // Usually we can just use x = **ptr, etc.  However, this doesn't work
    // for unions containing BitFields (long story, stupid language rules)
    // or arrays.  This will get optimized anyway.
    DoVoid((void*)&x, sizeof(x));
  }

  template <typename T>
  void DoPOD(T& x)
  {
    DoVoid((void*)&x, sizeof(x));
  }

  void Do(bool& x)
  {
    // bool's size can vary depending on platform, which can
    // cause breakages. This treats all bools as if they were
    // 8 bits in size.
    u8 stable = static_cast<u8>(x);

    Do(stable);

    if (m_mode == MODE_READ)
      x = stable != 0;
  }

  template <typename T>
  void DoPointer(T*& x, T* const base)
  {
    // pointers can be more than 2^31 apart, but you're using this function wrong if you need that
    // much range
    ptrdiff_t offset = x - base;
    Do(offset);
    if (m_mode == MODE_READ)
      x = base + offset;
  }

  [[nodiscard]] bool DoMarker(const std::string& prevName, u32 arbitraryNumber = 0x42)
  {
    u32 cookie = arbitraryNumber;
    Do(cookie);

    if (m_mode == PointerWrap::MODE_READ && cookie != arbitraryNumber)
    {
      PanicAlertT("Error: After \"%s\", found %d (0x%X) instead of save marker %d (0x%X). Aborting "
                  "savestate load...",
                  prevName.c_str(), cookie, cookie, arbitraryNumber, arbitraryNumber);
      return false;
    }

    return true;
  }

  template <typename T, typename Functor>
  void DoEachElement(T& container, Functor member)
  {
    u32 size = static_cast<u32>(container.size());
    Do(size);
    container.resize(size);

    for (auto& elem : container)
      member(*this, elem);
  }

private:
  template <typename T>
  void DoContiguousContainer(T& container)
  {
    u32 size = static_cast<u32>(container.size());
    Do(size);
    container.resize(size);

    if (size > 0)
      DoArray(&container[0], size);
  }

  template <typename T>
  void DoContainer(T& x)
  {
    DoEachElement(x, [](PointerWrap& p, typename T::value_type& elem) { p.Do(elem); });
  }

  DOLPHIN_FORCE_INLINE void DoVoid(void* data, u32 size)
  {
    if (m_mode == MODE_READ)
    {
      memcpy(data, &(*m_buffer)[m_offset], size);
    }
    else
    {
      m_buffer->resize(m_buffer->size() + size);
      memcpy(&(*m_buffer)[m_offset], data, size);
    }

    m_offset += size;
  }

  u64 m_offset = 0;
  std::vector<u8>* m_buffer;
  const Mode m_mode;
};
