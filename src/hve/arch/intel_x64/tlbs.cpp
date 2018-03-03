//
// Bareflank Extension : tlb-split

#include <bfdebug.h>
#include <memory_manager/arch/x64/map_ptr.h>
#include <intrinsics.h>

#include <eapis/hve/arch/intel_x64/ept/ept.h>
#include <hve/arch/intel_x64/tlbs.hpp>

namespace tlbs {
namespace intel_x64 {

    tlbs::tlbs(gsl::not_null<bfvmm::intel_x64::exit_handler*> a_exit_handler) : m_exit_handler{a_exit_handler} {
        using namespace ::intel_x64::vmcs;

        // Enable EPT and VPID
        secondary_processor_based_vm_execution_controls::enable_ept::enable();
        secondary_processor_based_vm_execution_controls::enable_vpid::enable();
        ::intel_x64::vmx::invept_global();

        // Set RWX handlers
        set_read_handler_delegate(access_handler_delegate_t::create<tlbs, &tlbs::handle_read>(this));
        set_write_handler_delegate(access_handler_delegate_t::create<tlbs, &tlbs::handle_write>(this));
        set_exec_handler_delegate(access_handler_delegate_t::create<tlbs, &tlbs::handle_exec>(this));

        // Add our EPT handler to the exit_handler
        m_exit_handler->add_handler(exit_reason::basic_exit_reason::ept_violation,
                                    handler_delegate_t::create<tlbs, &tlbs::handle_ept>(this));
    }

    tlbs::~tlbs() {
#ifndef NDEBUG
        if (m_log_enabled) { dump_log(); }
#endif
    }

    // -----------------------------------------------------------------------------
    // Handlers
    // -----------------------------------------------------------------------------

    bool tlbs::handle_ept(gsl::not_null<bfvmm::intel_x64::vmcs*> a_vmcs) {
        using namespace ::intel_x64;

        // Collect state information
        const auto& rip = a_vmcs->save_state()->rip;
        const auto& vcpuid = a_vmcs->save_state()->vcpuid;
        struct info_t info = {rip, vcpuid};

        // Check exit qualification bits and call the corresponding handler
        const auto access_bits = get_bits(vmcs::exit_qualification::ept_violation::get(), 0x7UL);
        if (is_bit_set(access_bits, access_bit_t::read)) {
            return read_handler_delegate(info);
        } else if (is_bit_set(access_bits, access_bit_t::write)) {
            return write_handler_delegate(info);
        } else if (is_bit_set(access_bits, access_bit_t::exec)) {
            return exec_handler_delegate(info);
        }

        // If we get here, something went wrong!
        // The hypervisor will halt.
        return false;
    }

    bool tlbs::handle_read(info_t& a_info) {
        using namespace ::intel_x64;

        //const auto cr3 = vmcs::guest_cr3::get();
        const auto mask = ~(ept::pt::size_bytes - 1);
        //const auto gva = vmcs::guest_linear_address::get();
        const auto gpa = vmcs::guest_physical_address::get();
        const auto d_pa = gpa & mask;

        const auto split_it = split_pages.find(d_pa);
        if (split_it != split_pages.end()) {
            return true;
        }

        // If we get here, we got an exit for an unregistered page.
        // Try to recover by resetting the access bits for the page
        // to pass-through.


        auto& entry = g_root_ept->gpa_to_epte(d_pa);

        return false;
    }
    bool tlbs::handle_write(info_t& a_info) { return true; }
    bool tlbs::handle_exec(info_t& a_info) { return true; }

}  // namespace intel_x64
}  // namespace tlbs
