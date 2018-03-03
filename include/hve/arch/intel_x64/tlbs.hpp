//
// Bareflank Extension : tlb-split

#pragma once
#ifndef TLBS_INTEL_X64_TLBSPLIT_H
#define TLBS_INTEL_X64_TLBSPLIT_H

#include <bfgsl.h>

#include <list>
#include <unordered_map>

#include <bfvmm/hve/arch/intel_x64/exit_handler/exit_handler.h>
#include <bfvmm/hve/arch/intel_x64/vmcs/vmcs.h>

// -----------------------------------------------------------------------------
// Exports
// -----------------------------------------------------------------------------

#include <bfexports.h>

#ifndef STATIC_TLBSPLIT_HVE
#ifdef SHARED_TLBSPLIT_HVE
#define EXPORT_TLBSPLIT_HVE EXPORT_SYM
#else
#define EXPORT_TLBSPLIT_HVE IMPORT_SYM
#endif
#else
#define EXPORT_TLBSPLIT_HVE
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

// -----------------------------------------------------------------------------
// Definitions
// -----------------------------------------------------------------------------

namespace tlbs {
namespace intel_x64 {

    namespace access_bit_t {
        constexpr const auto read = 0;
        constexpr const auto write = 1;
        constexpr const auto exec = 2;
    }  // namespace access_bit_t

    class EXPORT_TLBSPLIT_HVE tlbs {
    public:
        using u64_t = uint64_t;
        using addr_t = u64_t;
        using counter_t = int32_t;

        struct info_t {
            addr_t rip;    // In
            u64_t vcpuid;  // In
        };

        struct split_t {
            enum split_state_t { disabled, enabled };
            std::unique_ptr<uint8_t[]> c_page = nullptr;  // Code page owner
            addr_t c_va = 0;                              // Code page virtual address
            addr_t c_pa = 0;                              // Code page physical address
            addr_t d_va = 0;                              // Data page virtual address
            addr_t d_pa = 0;                              // Data page physical address
            std::list<addr_t> registered_addresses{};     // Addresses registered for this split
            counter_t split_no = 0;                       // Number of registered splits for this split
            u64_t cr3 = 0;                                // CR3 of requestee
            split_state_t active = disabled;              // State of the split
        };

#ifndef NDEBUG
        struct log_t {
            u64_t cr3;                                 // CR3 of requestee
            addr_t gpa;                                // Physical address of exit
            addr_t gva;                                // Virtual address of exit
            std::list<addr_t> registered_addresses{};  // Addresses registered for this split
            u64_t vcpuid;                              // VCPUID when exiting
        };
#endif

        // Delegates
        using access_handler_delegate_t = delegate<bool(info_t&)>;

        /// Constructor
        ///
        /// @expects
        /// @ensures
        ///
        tlbs(gsl::not_null<bfvmm::intel_x64::exit_handler*> a_exit_handler);

        /// Destructor
        ///
        /// @expects
        /// @ensures
        ///
        ~tlbs();

    public:
        /// Set handler for READ violation exits
        ///
        /// @expects
        /// @ensures
        ///
        /// @param d the handler to call for READ violation exits
        ///
        void set_read_handler_delegate(access_handler_delegate_t&& d);

        /// Set handler for WRITE violation exits
        ///
        /// @expects
        /// @ensures
        ///
        /// @param d the handler to call for WRITE violations
        ///
        void set_write_handler_delegate(access_handler_delegate_t&& d);

        /// Set handler for EXEC violation exits
        ///
        /// @expects
        /// @ensures
        ///
        /// @param d the handler to call for EXEC violations
        ///
        void set_exec_handler_delegate(access_handler_delegate_t&& d);

#ifndef NDEBUG
    public:
        /// Enable  Log
        ///
        /// Example:
        /// @code
        /// this->enable_log();
        /// @endcode
        ///
        /// @expects
        /// @ensures
        ///
        void enable_log();

        /// Disable  Log
        ///
        /// Example:
        /// @code
        /// this->disable_log();
        /// @endcode
        ///
        /// @expects
        /// @ensures
        ///
        void disable_log();

        /// Dump  Log
        ///
        /// Example:
        /// @code
        /// this->dump_log();
        /// @endcode
        ///
        /// @expects
        /// @ensures
        ///
        void dump_log();
#endif

    public:
        bool handle_ept(gsl::not_null<bfvmm::intel_x64::vmcs*> a_vmcs);
        bool handle_read(info_t& a_info);
        bool handle_write(info_t& a_info);
        bool handle_exec(info_t& a_info);

    private:
        bfvmm::intel_x64::exit_handler* m_exit_handler;

        access_handler_delegate_t read_handler_delegate;
        access_handler_delegate_t write_handler_delegate;
        access_handler_delegate_t exec_handler_delegate;

        std::unordered_map<addr_t, counter_t> remapped_pages;
        std::unordered_map<addr_t, split_t> split_pages;

#ifndef NDEBUG
        bool m_log_enabled{false};
        std::unordered_map<addr_t, std::list<log_t>> m_read_log;
        std::unordered_map<addr_t, std::list<log_t>> m_write_log;
        std::unordered_map<addr_t, std::list<log_t>> m_exec_log;
#endif

    public:
        /// @cond

        tlbs(tlbs&&) = default;
        tlbs& operator=(tlbs&&) = default;

        tlbs(const tlbs&) = delete;
        tlbs& operator=(const tlbs&) = delete;

        /// @endcond
    };

}  // namespace intel_x64
}  // namespace tlbs

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif  // TLB_INTEL_X64_TLBSPLIT_H
