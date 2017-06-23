#ifndef _H
#define _H

#include <memory_manager/map_ptr_x64.h>
#include <vmcs/root_ept_intel_x64.h>
#include <vmcs/ept_entry_intel_x64.h>
#include <vmcs/vmcs_intel_x64_eapis.h>
#include <vmcs/vmcs_intel_x64_16bit_control_fields.h>
#include <vmcs/vmcs_intel_x64_32bit_read_only_data_fields.h>
#include <vmcs/vmcs_intel_x64_64bit_guest_state_fields.h>
#include <vmcs/vmcs_intel_x64_64bit_read_only_data_fields.h>
#include <vmcs/vmcs_intel_x64_natural_width_guest_state_fields.h>
#include <vmcs/vmcs_intel_x64_natural_width_read_only_data_fields.h>
#include <exit_handler/exit_handler_intel_x64_eapis.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <map>
#include <mutex>

using namespace intel_x64;

using ptr_t = void*;
using int_t = uintptr_t;

/// Context structure for TLB splits
///
struct split_context {
    std::unique_ptr<uint8_t[]> c_page = nullptr;

    int_t c_va = 0;
    int_t c_pa = 0;

    int_t d_va = 0;
    int_t d_pa = 0;

    size_t num_hooks = 0;
    uint64_t cr3 = 0;
    bool active = false;
};

extern std::unique_ptr<root_ept_intel_x64> g_root_ept;
std::map<int_t  /*d_pa*/,         std::unique_ptr<split_context>> g_splits;
std::map<int_t /*aligned_2m_pa*/, size_t /*num_splits*/>          g_2m_pages;
static std::mutex g_mutex;

const std::string log_file{ R"(C:\flip_log.txt)" };

#define CONTEXT(d_pa) g_splits[d_pa]

class tlb_handler : public exit_handler_intel_x64_eapis
{
public:

    /// Default Constructor
    ///
    tlb_handler()
    { }

    /// Destructor
    ///
    ~tlb_handler() override
    { }

    /// Handle Exit
    ///
    void handle_exit(intel_x64::vmcs::value_type reason) override
    {
        // Check for EPT violation
        if (reason == vmcs::exit_reason::basic_exit_reason::ept_violation)
        {
            // WARNING: Do not use the invept or invvpid instructions in this
            //          function. Doing so will cause an intifinite loop. Intel
            //          specifically states not to invalidate as the hardware is
            //          doing this for you.

            // Get cr3, mask, gva and gpa
            auto &&cr3 = vmcs::guest_cr3::get();
            auto &&mask = ~(ept::pt::size_bytes - 1);
            auto &&gva = vmcs::guest_linear_address::get();
            auto &&gpa = vmcs::guest_physical_address::get();
            auto &&d_pa = gpa & mask;

            // Search for relevant entry in <map> m_splits.
            auto &&split_it = g_splits.find(d_pa);
            if (split_it == g_splits.end())
            {

                // Unexpected EPT violation for this page.
                // Check if it is a WRITE violation. If that's
                // the case, it likely is a different process,
                // so try to reset the access flags to pass-through.
                // (I don't get why they wouldn't be in the first place.)
                if (vmcs::exit_qualification::ept_violation::data_write::is_enabled())
                {
                    bfinfo << bfcolor_warning << "WRITE " << bfcolor_end << ": gva: " << view_as_pointer(gva)
                      << " gpa: " << view_as_pointer(gpa)
                      << " d_pa: " << view_as_pointer(d_pa)
                      << " cr3: " << view_as_pointer(cr3)
                      << " flags: "
                      << (vmcs::exit_qualification::ept_violation::data_read::is_enabled() ? "R" : "-")
                      << (vmcs::exit_qualification::ept_violation::data_write::is_enabled() ? "W" : "-")
                      << (vmcs::exit_qualification::ept_violation::instruction_fetch::is_enabled() ? "X" : "-")
                      << bfendl;

                    auto &&entry = g_root_ept->gpa_to_epte(gpa);
                    entry.pass_through_access();
                }
                else
                {
                    bfinfo << bfcolor_error << "UNX_V" << bfcolor_end << ": gva: " << view_as_pointer(gva)
                      << " gpa: " << view_as_pointer(gpa)
                      << " d_pa: " << view_as_pointer(d_pa)
                      << " cr3: " << view_as_pointer(cr3)
                      << " flags: "
                      << (vmcs::exit_qualification::ept_violation::data_read::is_enabled() ? "R" : "-")
                      << (vmcs::exit_qualification::ept_violation::data_write::is_enabled() ? "W" : "-")
                      << (vmcs::exit_qualification::ept_violation::instruction_fetch::is_enabled() ? "X" : "-")
                      << bfendl;
                }
            }
            else
            {
                // Lock guard
                std::lock_guard<std::mutex> guard(g_mutex);

                // Check exit qualifications
                if (vmcs::exit_qualification::ept_violation::data_write::is_enabled())
                {
                    // WRITE violation. Deactivate split and flip to data page.
                    //
                    bfinfo << bfcolor_warning << "WRITE " << bfcolor_end << ": gva: " << view_as_pointer(gva)
                      << " gpa: " << view_as_pointer(gpa)
                      << " d_pa: " << view_as_pointer(d_pa)
                      << " cr3: " << view_as_pointer(cr3)
                      << " flags: "
                      << (vmcs::exit_qualification::ept_violation::data_read::is_enabled() ? "R" : "-")
                      << (vmcs::exit_qualification::ept_violation::data_write::is_enabled() ? "W" : "-")
                      << (vmcs::exit_qualification::ept_violation::instruction_fetch::is_enabled() ? "X" : "-")
                      << bfendl;

                    deactivate_split(gva);
                }
                else if (vmcs::exit_qualification::ept_violation::data_read::is_enabled())
                {
                    // READ violation. Flip to data page.
                    //
                    bfinfo << bfcolor_func << "READ " << bfcolor_end << ": gva: " << view_as_pointer(gva)
                      << " gpa: " << view_as_pointer(gpa)
                      << " d_pa: " << view_as_pointer(d_pa)
                      << " cr3: " << view_as_pointer(cr3)
                      << " flags: "
                      << (vmcs::exit_qualification::ept_violation::data_read::is_enabled() ? "R" : "-")
                      << (vmcs::exit_qualification::ept_violation::data_write::is_enabled() ? "W" : "-")
                      << (vmcs::exit_qualification::ept_violation::instruction_fetch::is_enabled() ? "X" : "-")
                      << bfendl;

                    if (vmcs::exit_qualification::ept_violation::instruction_fetch::is_enabled())
                    {
                        bferror << "handle_exit: READ|EXEC violation. Not handled for now. gpa: " << view_as_pointer(d_pa) << bfendl;
                    }

                    auto &&entry = g_root_ept->gpa_to_epte(d_pa);
                    entry.trap_on_access();
                    entry.set_phys_addr(split_it->second->d_pa);
                    entry.set_read_access(true);
                }
                else if(vmcs::exit_qualification::ept_violation::instruction_fetch::is_enabled())
                {
                    // EXEC violation. Flip to code page.
                    //
                    bfinfo << bfcolor_func << "EXEC " << bfcolor_end << ": gva: " << view_as_pointer(gva)
                      << " gpa: " << view_as_pointer(gpa)
                      << " d_pa: " << view_as_pointer(d_pa)
                      << " cr3: " << view_as_pointer(cr3)
                      << " flags: "
                      << (vmcs::exit_qualification::ept_violation::data_read::is_enabled() ? "R" : "-")
                      << (vmcs::exit_qualification::ept_violation::data_write::is_enabled() ? "W" : "-")
                      << (vmcs::exit_qualification::ept_violation::instruction_fetch::is_enabled() ? "X" : "-")
                      << bfendl;

                    auto &&entry = g_root_ept->gpa_to_epte(d_pa);
                    entry.trap_on_access();
                    entry.set_phys_addr(split_it->second->c_pa);
                    entry.set_execute_access(true);
                }
                else
                {
                    bfinfo << bfcolor_error << "UNX_Q" << bfcolor_end << ": gva: " << view_as_pointer(gva)
                      << " gpa: " << view_as_pointer(gpa)
                      << " d_pa: " << view_as_pointer(d_pa)
                      << " cr3: " << view_as_pointer(cr3)
                      << " flags: "
                      << (vmcs::exit_qualification::ept_violation::data_read::is_enabled() ? "R" : "-")
                      << (vmcs::exit_qualification::ept_violation::data_write::is_enabled() ? "W" : "-")
                      << (vmcs::exit_qualification::ept_violation::instruction_fetch::is_enabled() ? "X" : "-")
                      << bfendl;
                }
            }

            // Resume the VM
            m_vmcs_eapis->resume();
        }

        exit_handler_intel_x64_eapis::handle_exit(reason);
    }

    /// Handle VMCall Registers
    ///
    void
    handle_vmcall_registers(vmcall_registers_t &regs) override
    {
        /// <r00> [RESERVED] vmcall mode (2)
        /// <r01> [RESERVED] magic number (0xB045EACDACD52E22)
        ///
        /// <r02> Method switch table
        ///
        /// 0 = hv_present()
        /// 1 = create_split_context(int_t gva)
        /// 2 = activate_split(int_t gva)
        /// 3 = deactivate_split(int_t gva)
        /// 4 = deactivate_all_splits()
        /// 5 = is_split(int_t gva)
        /// 6 = write_to_c_page(int_t from_va, int_t to_va, size_t size)
        ///
        /// <r03+> for args
        ///

        switch (regs.r02)
        {

            case 0: // hv_present()
                regs.r02 = static_cast<uintptr_t>(hv_present());
                break;
            case 1: // create_split_context(int_t gva)
                regs.r02 = static_cast<uintptr_t>(create_split_context(regs.r03));
                break;
            case 2: // activate_split(int_t gva)
                regs.r02 = static_cast<uintptr_t>(activate_split(regs.r03));
                break;
            case 3: // deactivate_split(int_t gva)
                regs.r02 = static_cast<uintptr_t>(deactivate_split(regs.r03));
                break;
            case 4: // deactivate_all_splits()
                regs.r02 = static_cast<uintptr_t>(deactivate_all_splits());
                break;
            case 5: // is_split(int_t gva)
                regs.r02 = static_cast<uintptr_t>(is_split(regs.r03));
                break;
            case 6: // write_to_c_page(int_t from_va, int_t to_va, size_t size)
                regs.r02 = static_cast<uintptr_t>(write_to_c_page(regs.r03, regs.r04, regs.r05));
                break;
            default:
                regs.r02 = -1u;
                break;

        }
    }

private:

    /// Returns a predefined value (1)
    ///
    /// @expects none
    ///
    /// @return 1
    ///
    int
    hv_present() noexcept
    {
        return 1;
    }

    /// Creates a split for gva
    ///
    /// @expects gva != 0
    ///
    /// @param gva the guest virtual address of the page to split
    ///
    /// @return 1 for success, 0 for failure
    ///
    int
    create_split_context(int_t gva)
    {
        // Sanity check for gva.
        if (gva == 0)
        {
            bfwarning << "create_split_context: gva has to be != 0" << bfendl;
            return 0;
        }

        // Get the physical aligned (4k) data page address.
        auto &&cr3 = vmcs::guest_cr3::get();
        auto &&mask_4k = ~(ept::pt::size_bytes - 1);
        auto &&d_va = gva & mask_4k;
        auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

        // Check whether we have already remapped the relevant **2m** page.
        auto &&mask_2m = ~(ept::pd::size_bytes - 1);
        auto &&aligned_2m_pa = d_pa & mask_2m;

        auto &&aligned_2m_it = g_2m_pages.find(aligned_2m_pa);
        if (aligned_2m_it == g_2m_pages.end())
        {
            // This (2m) page range has to be remapped to 4k.
            //
            bfdebug << "create_split_context: remapping page from 2m to 4k for: " << view_as_pointer(aligned_2m_pa) << bfendl;

            auto &&saddr = aligned_2m_pa;
            auto &&eaddr = aligned_2m_pa + ept::pd::size_bytes;
            g_root_ept->unmap(aligned_2m_pa);
            g_root_ept->setup_identity_map_4k(saddr, eaddr);
            g_2m_pages[aligned_2m_pa] = 0;

            // Invalidate/Flush TLB
            vmx::invvpid_all_contexts();
            vmx::invept_global();
        }
        else
            bfdebug << "create_split_context: page already remapped: " << view_as_pointer(aligned_2m_pa) << bfendl;

        // Check if we have already split the relevant **4k** page.
        auto &&split_it = g_splits.find(d_pa);
        if (split_it == g_splits.end())
        {
            // We haven't split this page yet, so do it now.
            //
            bfdebug << "create_split_context: splitting page for: " << view_as_pointer(d_pa) << bfendl;

            // Create and assign unqiue split_context.
            CONTEXT(d_pa) = std::make_unique<split_context>();
            CONTEXT(d_pa)->cr3 = cr3;
            CONTEXT(d_pa)->d_pa = d_pa;
            CONTEXT(d_pa)->d_va = d_va;

            // Allocate memory (4k) for new code page (host virtual).
            CONTEXT(d_pa)->c_page = std::make_unique<uint8_t[]>(ept::pt::size_bytes);
            CONTEXT(d_pa)->c_va = reinterpret_cast<int_t>(CONTEXT(d_pa)->c_page.get());
            CONTEXT(d_pa)->c_pa = g_mm->virtint_to_physint(CONTEXT(d_pa)->c_va);

            // Map data page into VMM (Host) memory.
            auto &&vmm_data = bfn::make_unique_map_x64<uint8_t>(d_va, cr3, ept::pt::size_bytes, vmcs::guest_ia32_pat::get());

            // Copy contents of data page (VMM copy) to code page.
            std::memmove(reinterpret_cast<ptr_t>(CONTEXT(d_pa)->c_va), reinterpret_cast<ptr_t>(vmm_data.get()), ept::pt::size_bytes);

            // Ensure that split is deactivated and increase split counter and set hook counter to 1.
            CONTEXT(d_pa)->active = false;
            CONTEXT(d_pa)->num_hooks = 1;
            g_2m_pages[aligned_2m_pa]++;
            bfdebug << "create_split_context: splits in this (2m) range: " << g_2m_pages[aligned_2m_pa] << bfendl;
        }
        else
        {
            // This page already got split. Just increase the hook counter.
            bfdebug << "create_split_context: page already split for: " << view_as_pointer(d_pa) << bfendl;
            CONTEXT(d_pa)->num_hooks++;
        }

        return 1;
    }

    /// Activates an already created split
    ///
    /// @expects gva != 0
    ///
    /// @param gva the guest virtual address of the page to activate
    ///
    /// @return 1 for success, 0 for failure
    ///
    int
    activate_split(int_t gva)
    {
        // Sanity check for gva.
        if (gva == 0)
        {
            bfwarning << "activate_split: gva has to be != 0" << bfendl;
            return 0;
        }

        // Get the physical aligned (4k) data page address.
        auto &&cr3 = vmcs::guest_cr3::get();
        auto &&mask_4k = ~(ept::pt::size_bytes - 1);
        auto &&d_va = gva & mask_4k;
        auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

        // Search for relevant entry in <map> m_splits.
        auto &&split_it = g_splits.find(d_pa);
        if (split_it != g_splits.end())
        {
            if (split_it->second->active == true)
            {
                // This split is already active, so don't do anything.
                //
                bfdebug << "activate_split: split already active for: " << view_as_pointer(d_pa) << bfendl;
                return 1;
            }

            // We have found the relevant split context.
            //
            bfdebug << "activate_split: activating split for: " << view_as_pointer(d_pa) << bfendl;

            std::lock_guard<std::mutex> guard(g_mutex);

            // We set a trap here, so that the correct page gets
            // set on next violation.
            auto &&entry = g_root_ept->gpa_to_epte(d_pa);
            entry.trap_on_access();
            //entry.set_phys_addr(split_it->second->d_pa);
            //entry.set_read_access(true);

            // Invalidate/Flush TLB
            vmx::invvpid_all_contexts();
            vmx::invept_global();

            split_it->second->active = true;
            return 1;
        }
        else
            bfwarning << "activate_split: no split found for: " << view_as_pointer(d_pa) << bfendl;

        return 0;
    }

    /// Deactivates (and frees) a split
    ///
    /// @expects gva != 0
    ///
    /// @param gva the guest virtual address of the page to deactivate
    ///
    /// @return 1 for success, 0 for failure
    ///
    int
    deactivate_split(int_t gva)
    {
        // Sanity check for gva.
        if (gva == 0)
        {
            bfwarning << "deactivate_split: gva has to be != 0" << bfendl;
            return 0;
        }

        // Get the physical aligned (4k) data page address.
        auto &&cr3 = vmcs::guest_cr3::get();
        auto &&mask_4k = ~(ept::pt::size_bytes - 1);
        auto &&d_va = gva & mask_4k;
        auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

        // Search for relevant entry in <map> m_splits.
        auto &&split_it = g_splits.find(d_pa);
        if (split_it != g_splits.end())
        {
            if (split_it->second->num_hooks > 1)
            {
                // We still have other hooks on this page,
                // so don't deactive the split yet.
                // Also decrease the hook counter.
                bfdebug << "deactivate_split: other hooks found on this page: " << view_as_pointer(d_pa) << bfendl;
                split_it->second->num_hooks--;
                return 1;
            }

            // We have found the relevant split context.
            //
            bfdebug << "deactivate_split: deactivating split for: " << view_as_pointer(d_pa) << bfendl;

            std::lock_guard<std::mutex> guard(g_mutex);

            // Flip to data page and restore to default flags
            auto &&entry = g_root_ept->gpa_to_epte(d_pa);
            entry.set_phys_addr(split_it->second->d_pa);
            entry.pass_through_access();

            // Invalidate/Flush TLB
            vmx::invvpid_all_contexts();
            vmx::invept_global();

            // Erase split context from <map> m_splits.
            g_splits.erase(d_pa);
            bfdebug << "deactivate_split: total num of splits: " << g_splits.size() << bfendl;

            // Check if we have an adjacent split.
            auto &&next_split_it = g_splits.find(d_pa + ept::pt::size_bytes);
            if (next_split_it != g_splits.end())
            {
                // We found an adjacent split.
                // Check if the hook counter is 0.
                if (next_split_it->second->num_hooks == 0)
                {
                    // This is likely a page which got split when writing
                    // to a code page while exceding the page bounds.
                    // Since this split isn't needed anymore, deactivate
                    // it too.
                    bfdebug << "deactivate_split: deactivating adjacent split for: " << view_as_pointer(next_split_it->second->d_pa) << bfendl;
                    deactivate_split(next_split_it->second->d_va);
                }
            }

            // Decrease the split counter.
            auto &&mask_2m = ~(ept::pd::size_bytes - 1);
            auto &&aligned_2m_pa = d_pa & mask_2m;
            g_2m_pages[aligned_2m_pa]--;
            bfdebug << "deactivate_split: splits in this (2m) range: " << g_2m_pages[aligned_2m_pa] << bfendl;
            /*
            // Check whether we have to remap the 4k pages to a 2m page.
            if (g_2m_pages[aligned_2m_pa] == 0)
            {
                // We need to remap the relevant 4k pages to a 2m page.
                //
                bfdebug << "deactivate_split: remapping pages from 4k to 2m for: " << view_as_pointer(aligned_2m_pa) << bfendl;

                auto &&saddr = aligned_2m_pa;
                auto &&eaddr = aligned_2m_pa + ept::pd::size_bytes;
                g_root_ept->unmap_identity_map_4k(saddr, eaddr);
                g_root_ept->map_2m(aligned_2m_pa, aligned_2m_pa, ept::memory_attr::pt_wb);

                // Invalidate/Flush TLB
                vmx::invvpid_all_contexts();
                vmx::invept_global();

                // Erase 2m page from map.
                g_2m_pages.erase(aligned_2m_pa);
            }
            //*/
            bfdebug << "deactivate_split: total num of remapped (2m) pages: " << g_2m_pages.size() << bfendl;

            return 1;
        }
        else
            bfwarning << "deactivate_split: no split found for: " << view_as_pointer(d_pa) << bfendl;

        return 0;
    }

    /// Deactivates (and frees) all splits
    ///
    int
    deactivate_all_splits()
    {
        bfdebug << "deactivate_all_splits: deactivating all splits. current num of splits: " << g_splits.size() << bfendl;

        for (auto & split : g_splits)
        {
            bfdebug << "deactivate_all_splits: deactivating split for: " << view_as_pointer(split.second->d_pa) << bfendl;
            deactivate_split(split.second->d_va);
        }

        return 1;
    }

    /// Check if page is split
    ///
    /// @expects gva != 0
    ///
    /// @param gva the guest virtual address of the page to check
    ///
    /// @return 1 if split, 0 if not
    ///
    int
    is_split(int_t gva)
    {
        // Sanity check for gva.
        if (gva == 0)
        {
            bfwarning << "is_split: gva has to be != 0" << bfendl;
            return 0;
        }

        // Get the physical aligned (4k) data page address.
        auto &&cr3 = vmcs::guest_cr3::get();
        auto &&mask_4k = ~(ept::pt::size_bytes - 1);
        auto &&d_va = gva & mask_4k;
        auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

        // Check for match in <map> m_splits.
        auto &&split_it = g_splits.find(d_pa);
        if (split_it != g_splits.end())
            return split_it->second->active ? 1 : 0;

        return 0;
    }

    /// Writes memory to code page
    ///
    /// @expects from_va != 0
    /// @expects to_va != 0
    /// @expects size >= 1
    ///
    /// @param from_va the guest virtual address of the memory to read from
    /// @param to_va the guest virtual address of the memory to write to
    /// @param size the size of the memory to read/write from/to
    ///
    /// @return 1 for success, 0 for failure
    ///
    int
    write_to_c_page(int_t from_va, int_t to_va, size_t size)
    {
        // Sanity check for from_va | to_va | size.
        if (from_va == 0 || to_va == 0 || size == 0)
        {
            bfwarning << "write_to_c_page: sanity check failure" << bfendl;
            return 0;
        }

        // Logging params
        bfdebug << "write_to_c_page: from_va: " << view_as_pointer(from_va) << ", to_va: " << view_as_pointer(to_va)<< ", size: " << view_as_pointer(size) << bfendl;

        // Get the physical aligned (4k) data page address.
        auto &&cr3 = vmcs::guest_cr3::get();
        auto &&mask_4k = ~(ept::pt::size_bytes - 1);
        auto &&d_va = to_va & mask_4k;
        auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

        // Search for relevant entry in <map> m_splits.
        auto &&split_it = g_splits.find(d_pa);
        if (split_it != g_splits.end())
        {
            // Check if we have to write to two consecutive pages.
            auto &&start_range = to_va;
            auto &&end_range = start_range + size - 1;
            if ((end_range >> 12) > (start_range >> 12))
            {
                // Get virt and phys address of second page.
                auto &&end_va = end_range & mask_4k;
                auto &&end_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

                bfdebug << "write_to_c_page: we are writing to two pages: " << view_as_pointer(d_pa) << " & " << view_as_pointer(end_pa) << bfendl;

                // Check if the second page is already split
                if (!is_split(end_va))
                {
                    // We have to split this page before writing to it.
                    //
                    bfdebug << "write_to_c_page: splitting second page: " << view_as_pointer(end_pa) << bfendl;

                    create_split_context(end_va);
                    activate_split(end_va);
                }

                // Get write offset.
                auto &&write_offset = to_va - d_va;

                // Get bytes for first page.
                auto &&bytes_1st_page = (d_va + ept::pt::size_bytes) - to_va - 1;

                // Get bytes for second page.
                auto &&bytes_2nd_page = end_range - end_va;

                if (bytes_1st_page + bytes_2nd_page != size)
                    bfwarning << "write_to_c_page: sum of bytes doesn't equal original size: " << size << ", bytes_1st_page: " << bytes_1st_page << ", bytes_2nd_page: " << bytes_2nd_page << bfendl;

                // Map <from_va> memory into VMM (Host) memory.
                auto &&vmm_data = bfn::make_unique_map_x64<uint8_t>(from_va, cr3, size, vmcs::guest_ia32_pat::get());

                // Write to first page.
                std::memmove(reinterpret_cast<ptr_t>(CONTEXT(d_pa)->c_va + write_offset), reinterpret_cast<ptr_t>(vmm_data.get()), bytes_1st_page);

                // Write to second page.
                std::memmove(reinterpret_cast<ptr_t>(CONTEXT(end_pa)->c_va), reinterpret_cast<ptr_t>(vmm_data.get() + bytes_1st_page + 1), bytes_2nd_page);
            }
            else
            {
                bfdebug << "write_to_c_page: we are writing to one page: " << view_as_pointer(d_pa) << bfendl;

                // Get write offset
                auto &&write_offset = to_va - d_va;

                // Map <from_va> memory into VMM (Host) memory.
                auto &&vmm_data = bfn::make_unique_map_x64<uint8_t>(from_va, cr3, size, vmcs::guest_ia32_pat::get());

                // Copy contents of <from_va> (VMM copy) to <to_va> memory.
                std::memmove(reinterpret_cast<ptr_t>(CONTEXT(d_pa)->c_va + write_offset), reinterpret_cast<ptr_t>(vmm_data.get()), size);
            }

            return 1;
        }
        else
            bfwarning << "write_to_c_page: no split found for: " << view_as_pointer(d_pa) << bfendl;

        return 0;
    }
};

#endif
