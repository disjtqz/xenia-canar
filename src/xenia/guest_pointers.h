/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GUEST_POINTERS_H_
#define XENIA_GUEST_POINTERS_H_

#define offsetof32(...) static_cast<uint32_t>(__builtin_offsetof(__VA_ARGS__))
namespace xe {
template <typename TBase, typename TAdj, size_t offset>
struct ShiftedPointer {
  using this_type = ShiftedPointer<TBase, TAdj, offset>;
  TBase* m_base;
  inline TBase* operator->() { return m_base; }

  inline TBase& operator*() { return *m_base; }
  inline this_type& operator=(TBase* base) {
    m_base = base;
    return *this;
  }
  ShiftedPointer(TBase* base) { m_base = base; }

  inline this_type& operator=(this_type other) {
    m_base = other.m_base;
    return *this;
  }

  TAdj* GetAdjacent() {
    return reinterpret_cast<TAdj*>(
        &reinterpret_cast<uint8_t*>(m_base)[-static_cast<ptrdiff_t>(offset)]);
  }
};

template <typename T>
struct TypedGuestPointer {
  xe::be<uint32_t> m_ptr;
  inline TypedGuestPointer<T>& operator=(uint32_t ptr) {
    m_ptr = ptr;
    return *this;
  }
  //inline bool operator==(uint32_t ptr) const { return m_ptr == ptr; }
//  inline bool operator!=(uint32_t ptr) const { return m_ptr != ptr; }
  // use value directly, no endian swap needed
  //inline bool operator!() const { return !m_ptr.value; }
  inline operator uint32_t() const { return m_ptr; }
};

//matches hexrays' ADJ operator
template<typename TShiftedPointer>
inline auto ADJ(TShiftedPointer ptr) {
  return ptr.GetAdjacent();
}

}  // namespace xe

#endif  // XENIA_GUEST_POINTERS_H_