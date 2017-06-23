#ifndef EPT_ENTRY_EXT_INTEL_X64_H
#define EPT_ENTRY_EXT_INTEL_X64_H

#include <vmcs/ept_entry_intel_x64.h>

#include <gsl/gsl>

// -----------------------------------------------------------------------------
// Definition
// -----------------------------------------------------------------------------

class ept_entry_ext_intel_x64 : public ept_entry_intel_x64
{
public:

    using epte_ptr = uintptr_t *;
    using epte_value = uint64_t;

    /// Default Constructor
    ///
    /// @expects pte != nullptr
    /// @ensures none
    ///
    /// @param pte the pte that this page table entry encapsulates.
    ///
    ept_entry_ext_intel_x64(gsl::not_null<pointer> pte) noexcept :
      ept_entry_intel_x64(pte)
    { }

    /// Destructor
    ///
    /// @expects none
    /// @ensures none
    ///
    ~ept_entry_ext_intel_x64() override = default;

    /// EPTE pointer
    ///
    /// @expects none
    /// @ensures none
    ///
    epte_ptr epte() const noexcept
    { return m_epte; }

    /// Set EPTE pointer
    ///
    /// @expects none
    /// @ensures none
    ///
    /// @param val the pointer to replace m_epte with
    ///
    void set_epte(epte_ptr val) noexcept
    { m_epte = val; }

    /// EPTE value
    ///
    /// @expects none
    /// @ensures none
    ///
    epte_value epte_val() const noexcept
    { return *m_epte; }

    /// Set EPTE pointer
    ///
    /// @expects none
    /// @ensures none
    ///
    /// @param val the value to write to EPTE pointer
    ///
    void set_epte_val(epte_value val) noexcept
    { *m_epte = val; }
};

#endif
