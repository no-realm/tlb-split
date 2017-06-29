#ifndef TLB_HANDLER_H
#define TLB_HANDLER_H

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

#include <limits.h>
#include <algorithm>
#include <string>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <vector>
#include <map>
#include <mutex>

using namespace intel_x64;

using ptr_t = void*;
using int_t = uintptr_t;

namespace detail
{
    constexpr int HEX_DIGIT_BITS = 4;
    constexpr int HEX_BASE_CHARS = 2; // Optional.  See footnote #2.

    // Replaced CharCheck with a much simpler trait.
    template<typename T> struct is_char
      : std::integral_constant<bool,
        std::is_same<T, char>::value ||
        std::is_same<T, signed char>::value ||
        std::is_same<T, unsigned char>::value> {};
}

template<typename T>
std::string hex_out_s(T val, int width = (sizeof(T) * CHAR_BIT / detail::HEX_DIGIT_BITS))
{
    using namespace detail;

    std::stringstream ss;
    ss << std::hex
        << std::internal
        << std::showbase
        << std::setfill('0')
        << std::setw(width + HEX_BASE_CHARS)
        << (is_char<T>::value ? static_cast<unsigned int>(val) : val)
        ;

    return ss.str();
}

/// Context structure for TLB splits
///
struct split_context {
    std::unique_ptr<uint8_t[]> c_page = nullptr; // This is the owner of the code page memory.

    int_t c_va = 0; // This is the (host) virtual address of the code page.
    int_t c_pa = 0; // This is the (host) physical of the code page.

    int_t d_va = 0; // This is the (guest) virtual address of the data page.
    int_t d_pa = 0; // This is the (guest) physical address of the data page.

    int_t gva = 0;          // The (guest) physical address this split was requested for. (Only first request.)
    size_t num_hooks = 0;   // This holds the number of hooks for this split context.
    uint64_t cr3 = 0;       // This is the cr3 value of the process which requested the split.
    bool active = false;    // This defines whether this split is active.
};

struct flip_data {
    int_t rip = 0;
    int_t gva = 0;
    int_t orig_gva = 0;
    int_t gpa = 0;
    int_t d_pa = 0;
    int_t cr3 = 0;
    int_t flags = 0;
    int_t counter = 0;

    flip_data() = default;
    flip_data(int_t _rip, int_t _gva, int_t _orig_gva, int_t _gpa, int_t _d_pa, int_t _cr3, int_t _flags, int_t _counter)
    {
        rip = _rip;
        gva = _gva;
        orig_gva = _orig_gva;
        gpa = _gpa;
        d_pa = _d_pa;
        cr3 = _cr3;
        flags = _flags;
        counter = _counter;
    }

    ~flip_data() = default;
};

enum access_t {
    read = 0x001,
    write = 0x010,
    exec = 0x100,
};

extern std::unique_ptr<root_ept_intel_x64> g_root_ept;
std::map<int_t  /*d_pa*/,         std::unique_ptr<split_context>> g_splits;
std::map<int_t /*aligned_2m_pa*/, size_t /*num_splits*/>          g_2m_pages;
std::vector<flip_data> g_flip_log;
static std::mutex g_mutex;
static std::mutex g_flip_mutex;

#define CONTEXT(_d_pa) g_splits[_d_pa]
#define IT(_split_it) _split_it->second

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

            // Get cr3, mask, gva, gpa, d_pa and rip
            auto &&cr3 = vmcs::guest_cr3::get();
            auto &&mask = ~(ept::pt::size_bytes - 1);
            auto &&gva = vmcs::guest_linear_address::get();
            auto &&gpa = vmcs::guest_physical_address::get();
            auto &&d_pa = gpa & mask;
            auto &&rip = m_state_save->rip;

            // Get violation access flags.
            int_t flags = 0x000
                | (vmcs::exit_qualification::ept_violation::data_read::is_enabled()         ? access_t::read    : 0x000)
                | (vmcs::exit_qualification::ept_violation::data_write::is_enabled()        ? access_t::write   : 0x000)
                | (vmcs::exit_qualification::ept_violation::instruction_fetch::is_enabled() ? access_t::exec    : 0x000)
                ;

            // Search for relevant entry in <map> m_splits.
            auto &&split_it = g_splits.find(d_pa);
            if (split_it == g_splits.end())
            {
                // Unexpected EPT violation for this page.
                // Try to reset the access flags to pass-through.
                // (I don't get why they wouldn't be in the first place.)

                bfinfo << bfcolor_error << "UNX_V" << bfcolor_end << ": gva: " << hex_out_s(gva)
                  << " gpa: " << hex_out_s(gpa)
                  << " d_pa: " << hex_out_s(d_pa)
                  << " cr3: " << hex_out_s(cr3, 8)
                  << " flags: " << hex_out_s(flags, 3)
                  << bfendl;

                std::lock_guard<std::mutex> guard(g_mutex);
                auto &&entry = g_root_ept->gpa_to_epte(gpa);
                entry.pass_through_access();
            }
            else
            {
                // Check for known RIPs.
                auto &&flip_it = std::find_if(g_flip_log.begin(), g_flip_log.end(), [&rip, &flags](const flip_data & m) -> bool
                {
                    return (m.rip == rip && m.flags == flags);
                });
                if (flip_it != g_flip_log.end())
                {
                    // Increase counter and update addresses.
                    std::lock_guard<std::mutex> flip_guard(g_flip_mutex);
                    flip_it->counter++;
                    flip_it->gva = gva;
                    flip_it->gpa = gpa;
                    flip_it->d_pa = d_pa;
                }
                else
                {
                    // Add violation data to the flip log.
                    std::lock_guard<std::mutex> flip_guard(g_flip_mutex);
                    g_flip_log.emplace_back(rip, gva, split_it->second->gva, gpa, d_pa, cr3, flags, 1);
                }

                // Check exit qualifications
                if (access_t::write == (flags & access_t::write))
                {
                    if (split_it->second->cr3 != cr3)
                    {
                        // WRITE violation. Deactivate split and flip to data page.
                        //
                        bfwarning << "handle_exit: deactivating page because of write violation from different cr3: " << hex_out_s(cr3, 8) << bfendl;
                        deactivate_split(gva);
                    }
                    else
                    {
                        // Switch to data page.
                        std::lock_guard<std::mutex> guard(g_mutex);
                        auto &&entry = g_root_ept->gpa_to_epte(d_pa);
                        entry.trap_on_access();
                        entry.set_phys_addr(split_it->second->d_pa);
                        entry.set_read_access(true);
                        entry.set_write_access(true);
                    }
                }
                else if (access_t::read == (flags & access_t::read))
                {
                    // READ violation. Flip to data page.
                    //

                    std::lock_guard<std::mutex> guard(g_mutex);
                    auto &&entry = g_root_ept->gpa_to_epte(d_pa);
                    entry.trap_on_access();
                    entry.set_phys_addr(split_it->second->d_pa);
                    entry.set_read_access(true);
                    entry.set_write_access(true);
                }
                else if(access_t::exec == (flags & access_t::exec))
                {
                    // EXEC violation. Flip to code page.
                    //

                    std::lock_guard<std::mutex> guard(g_mutex);
                    auto &&entry = g_root_ept->gpa_to_epte(d_pa);
                    entry.trap_on_access();
                    entry.set_phys_addr(split_it->second->c_pa);
                    entry.set_execute_access(true);
                }
                else
                {
                    // This shouldn't even be possible...
                    //

                    bfinfo << bfcolor_error << "UNX_Q" << bfcolor_end << ": gva: " << hex_out_s(gva)
                      << " gpa: " << hex_out_s(gpa)
                      << " d_pa: " << hex_out_s(d_pa)
                      << " cr3: " << hex_out_s(cr3, 8)
                      << " flags: " << hex_out_s(flags, 3)
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
        /// 7 = get_flip_num()
        /// 8 = get_flip_data(int_t out_addr, int_t out_size)
        /// 9 = clear_flip_data()
        /// 10 = remove_flip_entry(int_t rip)
        ///
        /// <r03+> for args
        ///

        // Default to failed operation.
        auto _switch = regs.r02;
        regs.r02 = 0;
        switch (_switch)
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
            case 7: // get_flip_num()
                regs.r02 = get_flip_num();
                break;
            case 8: // get_flip_data(int_t out_addr, int_t out_size)
                regs.r02 = static_cast<uintptr_t>(get_flip_data(regs.r03, regs.r04));
                break;
            case 9: // clear_flip_data()
                regs.r02 = static_cast<uintptr_t>(clear_flip_data());
                break;
            case 10: // remove_flip_entry(int_t rip)
                regs.r02 = static_cast<uintptr_t>(remove_flip_entry(regs.r03));
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
        expects(gva != 0);

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
            bfdebug << "create_split_context: remapping page from 2m to 4k for: " << hex_out_s(aligned_2m_pa) << bfendl;

            std::lock_guard<std::mutex> guard(g_mutex);
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
            bfdebug << "create_split_context: page already remapped: " << hex_out_s(aligned_2m_pa) << bfendl;

        // Check if we have already split the relevant **4k** page.
        auto &&split_it = g_splits.find(d_pa);
        if (split_it == g_splits.end())
        {
            // We haven't split this page yet, so do it now.
            //
            bfdebug << "create_split_context: splitting page for: " << hex_out_s(d_pa) << bfendl;

            // Create and assign unqiue split_context.
            std::lock_guard<std::mutex> guard(g_mutex);
            CONTEXT(d_pa) = std::make_unique<split_context>();
            CONTEXT(d_pa)->gva = gva;
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
            bfdebug << "create_split_context: # of hooks on this page: " << CONTEXT(d_pa)->num_hooks << bfendl;
        }
        else
        {
            // This page already got split. Just increase the hook counter.
            bfdebug << "create_split_context: page already split for: " << hex_out_s(d_pa) << bfendl;
            std::lock_guard<std::mutex> guard(g_mutex);
            IT(split_it)->num_hooks++;
            bfdebug << "create_split_context: # of hooks on this page: " << IT(split_it)->num_hooks << bfendl;
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
        expects(gva != 0);

        // Get the physical aligned (4k) data page address.
        auto &&cr3 = vmcs::guest_cr3::get();
        auto &&mask_4k = ~(ept::pt::size_bytes - 1);
        auto &&d_va = gva & mask_4k;
        auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

        // Search for relevant entry in <map> m_splits.
        auto &&split_it = g_splits.find(d_pa);
        if (split_it != g_splits.end())
        {
            if (IT(split_it)->active == true)
            {
                // This split is already active, so don't do anything.
                //
                bfdebug << "activate_split: split already active for: " << hex_out_s(d_pa) << bfendl;
                return 1;
            }

            // We have found the relevant split context.
            //
            bfdebug << "activate_split: activating split for: " << hex_out_s(d_pa) << bfendl;

            // We assign the code page here, since that's the most
            // likely one to get used next.
            std::lock_guard<std::mutex> guard(g_mutex);
            auto &&entry = g_root_ept->gpa_to_epte(d_pa);
            entry.set_phys_addr(IT(split_it)->c_pa);
            entry.trap_on_access();
            entry.set_execute_access(true);

            // Invalidate/Flush TLB
            vmx::invvpid_all_contexts();
            vmx::invept_global();

            IT(split_it)->active = true;
            return 1;
        }
        else
            bfwarning << "activate_split: no split found for: " << hex_out_s(d_pa) << bfendl;

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
        expects(gva != 0);

        // Get the physical aligned (4k) data page address.
        auto &&cr3 = vmcs::guest_cr3::get();
        auto &&mask_4k = ~(ept::pt::size_bytes - 1);
        auto &&d_va = gva & mask_4k;
        auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

        // Search for relevant entry in <map> m_splits.
        auto &&split_it = g_splits.find(d_pa);
        if (split_it != g_splits.end())
        {
            if (IT(split_it)->num_hooks > 1)
            {
                // We still have other hooks on this page,
                // so don't deactive the split yet.
                // Also decrease the hook counter.
                bfdebug << "deactivate_split: other hooks found on this page: " << hex_out_s(d_pa) << bfendl;
                bfdebug << "deactivate_split: # of hooks on this page: " << IT(split_it)->num_hooks << bfendl;

                std::lock_guard<std::mutex> guard(g_mutex);
                IT(split_it)->num_hooks--;
                return 1;
            }

            // We have found the relevant split context.
            //
            bfwarning << "deactivate_split: deactivating split for: " << hex_out_s(d_pa) << bfendl;
            bfdebug << "deactivate_split: # of hooks on this page: " << IT(split_it)->num_hooks << bfendl;

            // Flip to data page and restore to default flags
            std::lock_guard<std::mutex> guard(g_mutex);
            auto &&entry = g_root_ept->gpa_to_epte(d_pa);
            entry.set_phys_addr(IT(split_it)->d_pa);
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
                if (IT(next_split_it)->num_hooks == 0)
                {
                    // This is likely a page which got split when writing
                    // to a code page while exceding the page bounds.
                    // Since this split isn't needed anymore, deactivate
                    // it too.
                    bfdebug << "deactivate_split: deactivating adjacent split for: " << hex_out_s(IT(next_split_it)->d_pa) << bfendl;
                    deactivate_split(IT(next_split_it)->d_va);
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
                bfdebug << "deactivate_split: remapping pages from 4k to 2m for: " << hex_out_s(aligned_2m_pa) << bfendl;

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
            bfwarning << "deactivate_split: no split found for: " << hex_out_s(d_pa) << bfendl;

        return 0;
    }

    /// Deactivates (and frees) all splits
    ///
    int
    deactivate_all_splits()
    {
        bfdebug << "deactivate_all_splits: deactivating all splits. current num of splits: " << g_splits.size() << bfendl;

        for (const auto & split : g_splits)
        {
            bfdebug << "deactivate_all_splits: deactivating split for: " << hex_out_s(split.second->d_pa) << bfendl;
            deactivate_split(split.second->gva);
        }

        return 1;
    }

    /// Check if page is split
    ///
    /// @expects gva != 0
    ///
    /// @param gva the guest virtual address of the page to check
    ///
    /// @return 1 if split, 0 if not and -1 if page is not present
    ///
    int
    is_split(int_t gva)
    {
        expects(gva != 0);

        try
        {
            // Get the physical aligned (4k) data page address.
            auto &&cr3 = vmcs::guest_cr3::get();
            auto &&mask_4k = ~(ept::pt::size_bytes - 1);
            auto &&d_va = gva & mask_4k;
            auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

            // Check for match in <map> m_splits.
            auto &&split_it = g_splits.find(d_pa);
            if (split_it != g_splits.end())
                return IT(split_it)->active ? 1 : 0;
        }
        catch (std::exception&)
        {
            bfwarning << "is_split: " << "page doesn't seem to be present" << bfendl;
            return -1;
        }

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
        expects(from_va != 0);
        expects(to_va != 0);
        expects(size >= 1);

        // Logging params
        bfdebug << "write_to_c_page: from_va: " << hex_out_s(from_va) << ", to_va: " << hex_out_s(to_va)<< ", size: " << hex_out_s(size) << bfendl;

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

                bfdebug << "write_to_c_page: we are writing to two pages: " << hex_out_s(d_pa) << " & " << hex_out_s(end_pa) << bfendl;

                // Check if the second page is already split
                if (is_split(end_va) == 0)
                {
                    // We have to split this page before writing to it.
                    //
                    bfdebug << "write_to_c_page: splitting second page: " << hex_out_s(end_pa) << bfendl;

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

                std::lock_guard<std::mutex> guard(g_mutex);

                // Map <from_va> memory into VMM (Host) memory.
                auto &&vmm_data = bfn::make_unique_map_x64<uint8_t>(from_va, cr3, size, vmcs::guest_ia32_pat::get());

                // Write to first page.
                std::memmove(reinterpret_cast<ptr_t>(IT(split_it)->c_va + write_offset), reinterpret_cast<ptr_t>(vmm_data.get()), bytes_1st_page);

                // Write to second page.
                std::memmove(reinterpret_cast<ptr_t>(IT(split_it)->c_va), reinterpret_cast<ptr_t>(vmm_data.get() + bytes_1st_page + 1), bytes_2nd_page);
            }
            else
            {
                bfdebug << "write_to_c_page: we are writing to one page: " << hex_out_s(d_pa) << bfendl;

                // Get write offset
                auto &&write_offset = to_va - d_va;

                std::lock_guard<std::mutex> guard(g_mutex);

                // Map <from_va> memory into VMM (Host) memory.
                auto &&vmm_data = bfn::make_unique_map_x64<uint8_t>(from_va, cr3, size, vmcs::guest_ia32_pat::get());

                // Copy contents of <from_va> (VMM copy) to <to_va> memory.
                std::memmove(reinterpret_cast<ptr_t>(IT(split_it)->c_va + write_offset), reinterpret_cast<ptr_t>(vmm_data.get()), size);
            }

            return 1;
        }
        else
            bfwarning << "write_to_c_page: no split found for: " << hex_out_s(d_pa) << bfendl;

        return 0;
    }

    /// Returns the number of elements in the flip log.
    ///
    size_t
    get_flip_num()
    {
        std::lock_guard<std::mutex> flip_guard(g_flip_mutex);
        return g_flip_log.size();
    }

    /// Writes the flip data to the passed <out_addr>.
    ///
    /// @expects out_addr != 0
    /// @expects out_size != 0
    ///
    /// @return 1
    ///
    int
    get_flip_data(int_t out_addr, int_t out_size)
    {
        expects(out_addr != 0);
        expects(out_size != 0);

        std::lock_guard<std::mutex> flip_guard(g_flip_mutex);

        // Map the required memory.
        auto &&omap = bfn::make_unique_map_x64<char>(out_addr, vmcs::guest_cr3::get(), out_size, vmcs::guest_ia32_pat::get());

        // Copy the flip data to the mapped memory region.
        std::memmove(omap.get(), g_flip_log.data(), out_size);

        return 1;
    }

    /// Clears the flip data log.
    ///
    int
    clear_flip_data()
    {
        std::lock_guard<std::mutex> flip_guard(g_flip_mutex);
        g_flip_log.clear();
        return 1;
    }

    /// Remove an entry from the flip data log by
    /// providing a RIP address.
    ///
    /// @expects rip != 0
    ///
    /// @return 1
    ///
    int
    remove_flip_entry(int_t rip)
    {
        expects(rip != 0);

        std::lock_guard<std::mutex> flip_guard(g_flip_mutex);

        // Find relevant entry/entries by provided RIP address.
        auto &&vec = g_flip_log; // Shorter name.
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&rip](const flip_data & o)
        {
            return o.rip == rip;
        }), vec.end());

        return 1;
    }
};

#endif
