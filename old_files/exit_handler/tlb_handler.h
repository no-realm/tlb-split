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
#include <serial/serial_port_intel_x64.h>

#include <limits.h>
#include <algorithm>
#include <string>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <vector>
#include <map>
#include <mutex>
#include <bitset>

using namespace intel_x64;

// Type aliases
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
    bool active = false;    // This defines whether this split is active or not.
};

struct flip_data {
    int_t rip = 0;
    int_t gva = 0;
    int_t orig_gva = 0;
    int_t gpa = 0;
    int_t d_pa = 0;
    int_t cr3 = 0;
    int_t bits = 0;
    int_t counter = 0;

    flip_data() = default;
    flip_data(int_t _rip, int_t _gva, int_t _orig_gva, int_t _gpa, int_t _d_pa, int_t _cr3, int_t _bits, int_t _counter)
    {
        rip = _rip;
        gva = _gva;
        orig_gva = _orig_gva;
        gpa = _gpa;
        d_pa = _d_pa;
        cr3 = _cr3;
        bits = _bits;
        counter = _counter;
    }

    ~flip_data() = default;
};

namespace access_t
{
    constexpr const auto read = 0;
    constexpr const auto write = 1;
    constexpr const auto exec = 2;
}

enum flip_access_t
{
    read,
    write,
    readwrite,
    exec,
    all
};

// EPTs
extern std::unique_ptr<root_ept_intel_x64> g_root_ept;
extern std::unique_ptr<root_ept_intel_x64> g_clean_ept;

// Global maps for splits and 2m pages
using split_map_t   = std::map<int_t  /*d_pa*/,         std::unique_ptr<split_context>>;
using page_map_t    = std::map<int_t /*aligned_2m_pa*/, size_t /*num_splits*/>;
split_map_t g_splits;
page_map_t g_2m_pages;

// Vector holding all the registered flip data
std::vector<flip_data> g_flip_log;

// Mutexes
static std::mutex g_mutex;
static std::mutex g_flip_mutex;

// Macros for easier access
#define CONTEXT(_d_pa) g_splits[_d_pa]
#define IT(_split_it) _split_it->second

// Debug/Logging switches
constexpr const auto flip_logging_disabled = false;
constexpr const auto flip_debug_disabled = true;
constexpr const auto debug_disabled = false;
#define _bfdebug            \
    if (debug_disabled) {}  \
    else bfdebug

class tlb_handler : public exit_handler_intel_x64_eapis
{
private:
    int_t prev_rip, rip_count;

public:

    /// Default Constructor
    ///
    tlb_handler ()
        : prev_rip(0)
        , rip_count(0)
    {
        _bfdebug << "tlb_handler instance initialized" << bfendl;
    }

    /// Destructor
    ///
    ~tlb_handler() override
    { }

    /// Monitor Trap Callback
    ///
    /// When the trap flag is set, and the VM is resumed, a VM exit is
    /// generated after the next instruction executes, providing a means
    /// to single step the execution of the VM. When this single step
    /// occurs, this callback is called.
    ///
    void
    monitor_trap_callback()
    {
        _bfdebug << "Resetting the trap" << bfendl;

        // Reset the trap.
        m_vmcs_eapis->set_eptp(g_root_ept->eptp());

        // Resume the VM
        this->resume();
    }

    /// Flip page (set physical address) and set respective access bits
    ///
    void
    flip_page(const int_t &phys_addr, const int_t &d_pa, const flip_access_t flip_access)
    {
        //std::lock_guard<std::mutex> guard(g_mutex);
        auto *m_epte = g_root_ept->gpa_to_epte(d_pa).epte();

        switch (flip_access) {
            case flip_access_t::read:
            { *m_epte = set_bits(*m_epte, 0xFFFFFFFFF007UL, phys_addr | 0x1UL); break; }
            case flip_access_t::write:
            { *m_epte = set_bits(*m_epte, 0xFFFFFFFFF007UL, phys_addr | 0x2UL); break; }
            case flip_access_t::readwrite:
            { *m_epte = set_bits(*m_epte, 0xFFFFFFFFF007UL, phys_addr | 0x3UL); break; }
            case flip_access_t::exec:
            { *m_epte = set_bits(*m_epte, 0xFFFFFFFFF007UL, phys_addr | 0x4UL); break; }
            case flip_access_t::all:
            { *m_epte = set_bits(*m_epte, 0xFFFFFFFFF007UL, phys_addr | 0x7UL); break; }
        }
    }

    /// Handle Exit
    ///
    void handle_exit(intel_x64::vmcs::value_type reason) override
    {
        // Check for EPT violation
        if (reason == vmcs::exit_reason::basic_exit_reason::ept_violation)
        {
            // WARNING: Do not use the invept or invvpid instructions in this
            //          function. Doing so will cause an infinite loop. Intel
            //          specifically states not to invalidate as the hardware is
            //          doing this for you.

            // Get cr3, mask, gva, gpa, d_pa, rip and vcpuid
            const auto &&cr3 = vmcs::guest_cr3::get();
            const auto &&mask = ~(ept::pt::size_bytes - 1);
            const auto &&gva = vmcs::guest_linear_address::get();
            const auto &&gpa = vmcs::guest_physical_address::get();
            const auto &&d_pa = gpa & mask;
            auto &&rip = m_state_save->rip;
            auto &&vcpuid = m_state_save->vcpuid;

            // Get the violation access bits directly from the bit structure
            // access_bits == (hex) 0x1 -> EXECUTE  -> (bin) 001
            // access_bits == (hex) 0x2 -> WRITE    -> (bin) 010
            // access_bits == (hex) 0x4 -> READ     -> (bin) 100
            //
            const auto &&access_bits = get_bits(vmcs::exit_qualification::ept_violation::get(), 0x7UL);
            //bfdebug << "violation access bits: " << hex_out_s(access_bits, 3) << bfendl;

            // Search for relevant entry in <map> m_splits.
            const auto &&split_it = g_splits.find(d_pa);
            if (split_it == g_splits.end())
            {
                // Unexpected EPT violation for this page.
                // Try to reset the access flags to pass-through.
                // (I don't get why they wouldn't be in the first place.)

                bfinfo << bfcolor_error << "UNX_V" << bfcolor_end << ": gva: " << hex_out_s(gva)
                  << " gpa: " << hex_out_s(gpa)
                  << " d_pa: " << hex_out_s(d_pa)
                  << " cr3: " << hex_out_s(cr3, 8)
                  << " bits: " << std::bitset<3>(access_bits)
                  << bfendl;

                auto &&entry = g_root_ept->gpa_to_epte(d_pa);
                flip_page(entry.phys_addr(), d_pa, flip_access_t::all);
            }
            else
            {
                if (flip_logging_disabled) {}
                else
                {
                    // Check for known RIPs.
                    auto &&flip_it = std::find_if(g_flip_log.begin(), g_flip_log.end(), [&rip, &access_bits](const flip_data & m) -> bool
                    {
                        return (m.rip == rip && m.bits == access_bits);
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
                        g_flip_log.emplace_back(rip, gva, IT(split_it)->gva, gpa, d_pa, cr3, access_bits, 1);
                    }
                }

                // Log entry
                ///* This seems to cause thrashing
                if (flip_debug_disabled) {}
                else
                {
                    bfinfo
                      << bfcolor_func << "["
                      << std::bitset<3>(access_bits)
                      << "]:"  << bfcolor_end
                      << " cr3: " << hex_out_s(cr3, 8)
                      << " rip: " << hex_out_s(rip)
                      << " gva: " << hex_out_s(gva)
                      //<< " gpa: " << hex_out_s(gpa)
                      //<< " d_pa: " << hex_out_s(d_pa)
                      //<< " bits: " << std::bitset<3>(access_bits)
                      << " vcpuid: " << vcpuid
                      << bfendl;
                }
                //*/

                // Compare the previous violation RIP to the current one and
                // increase the counter, if they are the same.
                // Else, just assign the current RIP to prev_rip and reset the
                // counter.
                if (rip == prev_rip)
                    rip_count++;
                else {
                    prev_rip = rip;
                    rip_count = 0;
                }

                // Check for TLB thrashing
                if (rip_count > 3)
                {
                    _bfdebug << bfcolor_error << "[" << vcpuid << "] " << bfcolor_end << "Thrashing detected at rip: " << hex_out_s(prev_rip) << bfendl;

                    // Reset prev_rip and rip_count
                    prev_rip = 0;
                    rip_count = 0;

                    // Single-step through the clean EPT
                    m_vmcs_eapis->set_eptp(g_clean_ept->eptp());
                    this->register_monitor_trap(&tlb_handler::monitor_trap_callback);
                    //this->resume();
                }

                // Check exit qualifications
                if (is_bit_set(access_bits, access_t::write))
                {
                    if (IT(split_it)->cr3 != cr3)
                    {
                        // WRITE violation. Deactivate split and flip to data page.
                        //
                        bfwarning << "[" << vcpuid << "] " << "handle_exit: deactivating page because of write violation from different cr3: " << hex_out_s(cr3, 8) << bfendl;
                        deactivate_split(gva);
                    }
                    else
                    {
                        // Switch to data page.
                        //_bfdebug << "[" << vcpuid << "] " << "handle_exit: switch to data for write: " << hex_out_s(cr3, 8) << '/' << hex_out_s(rip) << '/' << hex_out_s(gva) << bfendl;
                        flip_page(IT(split_it)->d_pa, d_pa, flip_access_t::readwrite);
                    }
                }
                else if (is_bit_set(access_bits, access_t::read))
                {
                    // READ violation. Flip to data page.
                    //
                    //_bfdebug << "[" << vcpuid << "] " << "handle_exit: switch to data for read: " << hex_out_s(cr3, 8) << '/' << hex_out_s(rip) << '/' << hex_out_s(gva) << bfendl;
                    flip_page(IT(split_it)->d_pa, d_pa, flip_access_t::readwrite);
                }
                else if(is_bit_set(access_bits, access_t::exec))
                {
                    // EXEC violation. Flip to code page.
                    //
                    //_bfdebug << "[" << vcpuid << "] " << "handle_exit: switch to code for exec: " << hex_out_s(cr3, 8) << '/' << hex_out_s(rip) << '/' << hex_out_s(gva) << bfendl;
                    flip_page(IT(split_it)->c_pa, d_pa, flip_access_t::exec);
                }
                else
                {
                    // This shouldn't even be possible...
                    //

                    bferror << "Unexpected exit qualifications: gva: " << hex_out_s(gva)
                      << " gpa: " << hex_out_s(gpa)
                      << " d_pa: " << hex_out_s(d_pa)
                      << " cr3: " << hex_out_s(cr3, 8)
                      << " bits: " << std::bitset<3>(access_bits)
                      << bfendl;
                }
            }

            // Resume the VM
            this->resume();
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
        const auto _switch = regs.r02;
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
    create_split_context(const int_t gva)
    {
        expects(gva != 0);

        // Get the physical aligned (4k) data page address.
        const auto &&cr3 = vmcs::guest_cr3::get();
        const auto &&mask_4k = ~(ept::pt::size_bytes - 1);
        const auto &&d_va = gva & mask_4k;
        const auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

        // Check whether we have already remapped the relevant **2m** page.
        const auto &&mask_2m = ~(ept::pd::size_bytes - 1);
        const auto &&aligned_2m_pa = d_pa & mask_2m;

        const auto &&aligned_2m_it = g_2m_pages.find(aligned_2m_pa);
        if (aligned_2m_it == g_2m_pages.end())
        {
            // This (2m) page range has to be remapped to 4k.
            //
            _bfdebug << "create_split_context: remapping page from 2m to 4k for: " << hex_out_s(aligned_2m_pa) << bfendl;

            std::lock_guard<std::mutex> guard(g_mutex);
            const auto saddr = aligned_2m_pa;
            const auto eaddr = aligned_2m_pa + ept::pd::size_bytes;
            g_root_ept->unmap(aligned_2m_pa);
            g_root_ept->setup_identity_map_4k(saddr, eaddr);
            g_2m_pages[aligned_2m_pa] = 0;

            // Invalidate/Flush TLB
            vmx::invvpid_all_contexts();
            vmx::invept_global();
        }
        else
            _bfdebug << "create_split_context: page already remapped: " << hex_out_s(aligned_2m_pa) << bfendl;

        // Check if we have already split the relevant **4k** page.
        const auto &&split_it = g_splits.find(d_pa);
        if (split_it == g_splits.end())
        {
            // We haven't split this page yet, so do it now.
            //
            _bfdebug << "create_split_context: splitting page for: " << hex_out_s(d_pa) << bfendl;

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
            const auto &&vmm_data = bfn::make_unique_map_x64<uint8_t>(d_va, cr3, ept::pt::size_bytes, vmcs::guest_ia32_pat::get());

            // Copy contents of data page (VMM copy) to code page.
            std::memmove(reinterpret_cast<ptr_t>(CONTEXT(d_pa)->c_va), reinterpret_cast<ptr_t>(vmm_data.get()), ept::pt::size_bytes);

            // Ensure that split is deactivated, increase split counter and set hook counter to 1.
            CONTEXT(d_pa)->active = false;
            CONTEXT(d_pa)->num_hooks = 1;
            g_2m_pages[aligned_2m_pa]++;
            _bfdebug << "create_split_context: splits in this (2m) range: " << g_2m_pages[aligned_2m_pa] << bfendl;
            _bfdebug << "create_split_context: # of hooks on this page: " << CONTEXT(d_pa)->num_hooks << bfendl;
        }
        else
        {
            // This page already got split. Just increase the hook counter.
            _bfdebug << "create_split_context: page already split for: " << hex_out_s(d_pa) << bfendl;
            std::lock_guard<std::mutex> guard(g_mutex);
            IT(split_it)->num_hooks++;
            _bfdebug << "create_split_context: # of hooks on this page: " << IT(split_it)->num_hooks << bfendl;
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
    activate_split(const int_t gva)
    {
        expects(gva != 0);

        // Get the physical aligned (4k) data page address.
        const auto &&cr3 = vmcs::guest_cr3::get();
        const auto &&mask_4k = ~(ept::pt::size_bytes - 1);
        const auto &&d_va = gva & mask_4k;
        const auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

        // Search for relevant entry in <map> m_splits.
        auto &&split_it = g_splits.find(d_pa);
        if (split_it != g_splits.end())
        {
            if (IT(split_it)->active == true)
            {
                // This split is already active, so don't do anything.
                //
                _bfdebug << "activate_split: split already active for: " << hex_out_s(d_pa) << bfendl;
                return 1;
            }

            // We have found the relevant split context.
            //
            _bfdebug << "activate_split: activating split for: " << hex_out_s(d_pa) << bfendl;

            // We assign the code page here, since that's the most
            // likely one to get used next.
            flip_page(IT(split_it)->c_pa, d_pa, flip_access_t::exec);

            // Invalidate/Flush TLB
            vmx::invvpid_all_contexts();
            vmx::invept_global();

            // Mark the split as active.
            IT(split_it)->active = true;
            return 1;
        }
        else
            bfwarning << "activate_split: no split found for: " << hex_out_s(d_pa) << bfendl;

        return 0;
    }

    /// Deactivates (and frees) a split for the given physical address
    ///
    /// @expects d_pa != 0
    ///
    /// @param d_pa the physical (4k) aligned address of the data page to deactivate
    ///
    /// @return 1 for success, 0 for failure
    ///
    int
    deactivate_split_pa(const int_t d_pa)
    {
        expects(d_pa != 0);

        // Search for relevant entry in <map> m_splits.
        auto &&split_it = g_splits.find(d_pa);
        if (split_it != g_splits.end())
        {
            if (IT(split_it)->num_hooks > 1)
            {
                // We still have other hooks on this page,
                // so don't deactive the split yet.
                // Just decrease the hook counter.
                _bfdebug << "deactivate_split_pa: other hooks found on this page: " << hex_out_s(d_pa) << bfendl;
                _bfdebug << "deactivate_split_pa: # of hooks on this page (before): " << IT(split_it)->num_hooks << bfendl;

                std::lock_guard<std::mutex> guard(g_mutex);
                IT(split_it)->num_hooks--;
                return 1;
            }

            // We have found the relevant split context.
            //
            _bfdebug << "deactivate_split_pa: deactivating split for: " << hex_out_s(d_pa) << bfendl;
            _bfdebug << "deactivate_split_pa: # of hooks on this page: " << IT(split_it)->num_hooks << bfendl;

            // Mutex block
            {
                // Flip to data page and restore to default (pass-through) flags
                flip_page(IT(split_it)->d_pa, d_pa, flip_access_t::all);

                // Erase split context from <map> m_splits.
                g_splits.erase(d_pa);
                _bfdebug << "deactivate_split_pa: total num of splits: " << g_splits.size() << bfendl;
            }

            // Invalidate/Flush TLB
            vmx::invvpid_all_contexts();
            vmx::invept_global();

            // Check if we have an adjacent split.
            const auto &&next_split_it = g_splits.find(d_pa + ept::pt::size_bytes);
            if (next_split_it != g_splits.end())
            {
                // We found an adjacent split.
                // Check if the hook counter is 0.
                if (IT(next_split_it)->num_hooks == 0)
                {
                    // This is likely a page which got split when writing
                    // to a code page while exceeding the page bounds.
                    // Since this split isn't needed anymore, deactivate
                    // it too.
                    _bfdebug << "deactivate_split_pa: deactivating adjacent split for: " << hex_out_s(IT(next_split_it)->d_pa) << bfendl;
                    deactivate_split(IT(next_split_it)->d_va);
                }
            }

            // Decrease the split counter.
            const auto &&mask_2m = ~(ept::pd::size_bytes - 1);
            const auto &&aligned_2m_pa = d_pa & mask_2m;
            g_2m_pages[aligned_2m_pa]--;
            _bfdebug << "deactivate_split_pa: splits in this (2m) range: " << g_2m_pages[aligned_2m_pa] << bfendl;
            /*
            // Check whether we have to remap the 4k pages to a 2m page.
            if (g_2m_pages[aligned_2m_pa] == 0)
            {
                // We need to remap the relevant 4k pages to a 2m page.
                //
                _bfdebug << "deactivate_split: remapping pages from 4k to 2m for: " << hex_out_s(aligned_2m_pa) << bfendl;

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
            _bfdebug << "deactivate_split_pa: total num of remapped (2m) pages: " << g_2m_pages.size() << bfendl;

            return 1;
        }
        else
            bfwarning << "deactivate_split_pa: no split found for: " << hex_out_s(d_pa) << bfendl;
        return 0;
    }

    /// Deactivates (and frees) a split for a given guest virtual address
    ///
    /// @expects gva != 0
    ///
    /// @param gva the guest virtual address of the page to deactivate
    ///
    /// @return 1 for success, 0 for failure
    ///
    int
    deactivate_split(const int_t gva)
    {
        expects(gva != 0);

        // Get the physical aligned (4k) data page address.
        const auto &&cr3 = vmcs::guest_cr3::get();
        const auto &&mask_4k = ~(ept::pt::size_bytes - 1);
        const auto &&d_va = gva & mask_4k;
        const auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

        return deactivate_split_pa(d_pa);
    }

    /// Deactivates (and frees) all splits
    ///
    int
    deactivate_all_splits()
    {
        if (g_splits.size() > 0)
        {
            _bfdebug << "deactivate_all_splits: deactivating all splits. current num of splits: " << g_splits.size() << bfendl;

            // Iterating thorugh <map> g_splits until it is empty.
            while (!g_splits.empty())
            {
                // Get the first split in the map.
                const auto &&split_it = g_splits.begin();
                _bfdebug << "deactivate_all_splits: deactivating split for: " << hex_out_s(IT(split_it)->d_pa) << bfendl;

                // Deactivating the split for a physical page address.
                deactivate_split_pa(IT(split_it)->d_pa);
            }
        }
        else
            _bfdebug << "deactivate_all_splits: no active splits found" << bfendl;

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
    is_split(const int_t gva)
    {
        expects(gva != 0);

        try
        {
            // Get the physical aligned (4k) data page address.
            const auto &&cr3 = vmcs::guest_cr3::get();
            const auto &&mask_4k = ~(ept::pt::size_bytes - 1);
            const auto &&d_va = gva & mask_4k;
            const auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

            // Check for match in <map> m_splits.
            const auto &&split_it = g_splits.find(d_pa);
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
    write_to_c_page(const int_t from_va, const int_t to_va, const size_t size)
    {
        expects(from_va != 0);
        expects(to_va != 0);
        expects(size >= 1);

        // Logging params
        _bfdebug << "write_to_c_page: from_va: " << hex_out_s(from_va) << ", to_va: " << hex_out_s(to_va)<< ", size: " << hex_out_s(size) << bfendl;

        // Get the physical aligned (4k) data page address.
        const auto &&cr3 = vmcs::guest_cr3::get();
        const auto &&mask_4k = ~(ept::pt::size_bytes - 1);
        const auto &&d_va = to_va & mask_4k;
        const auto &&d_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

        // Search for relevant entry in <map> m_splits.
        const auto &&split_it = g_splits.find(d_pa);
        if (split_it != g_splits.end())
        {
            // Check if we have to write to two consecutive pages.
            const auto start_range = to_va;
            const auto end_range = start_range + size - 1;
            if ((end_range >> 12) > (start_range >> 12))
            {
                // Get virt and phys address of second page.
                auto &&end_va = end_range & mask_4k;
                auto &&end_pa = bfn::virt_to_phys_with_cr3(d_va, cr3);

                _bfdebug << "write_to_c_page: we are writing to two pages: " << hex_out_s(d_pa) << " & " << hex_out_s(end_pa) << bfendl;

                // Check if the second page is already split
                if (is_split(end_va) == 0)
                {
                    // We have to split this page before writing to it.
                    //
                    _bfdebug << "write_to_c_page: splitting second page: " << hex_out_s(end_pa) << bfendl;

                    create_split_context(end_va);
                    activate_split(end_va);
                }

                // Get second split
                const auto &&second_split_it = g_splits.find(end_pa);
                if (second_split_it == g_splits.end())
                {
                    // For some reason, the second page didn't get split.
                    //
                    bfwarning << "write_to_c_page: split for the second page failed: " << hex_out_s(end_pa) << bfendl;

                    return 0;
                }

                // Get write offset.
                const auto &&write_offset = to_va - d_va;

                // Get bytes for first page.
                const auto &&bytes_1st_page = (d_va + ept::pt::size_bytes) - to_va - 1;

                // Get bytes for second page.
                const auto &&bytes_2nd_page = end_range - end_va;

                if (bytes_1st_page + bytes_2nd_page != size)
                    bfwarning << "write_to_c_page: sum of bytes doesn't equal original size: " << size << ", bytes_1st_page: " << bytes_1st_page << ", bytes_2nd_page: " << bytes_2nd_page << bfendl;

                std::lock_guard<std::mutex> guard(g_mutex);

                // Map <from_va> memory into VMM (Host) memory.
                auto &&vmm_data = bfn::make_unique_map_x64<uint8_t>(from_va, cr3, size, vmcs::guest_ia32_pat::get());

                // Write to first page.
                std::memmove(reinterpret_cast<ptr_t>(IT(split_it)->c_va + write_offset), reinterpret_cast<ptr_t>(vmm_data.get()), bytes_1st_page);

                // Write to second page.
                std::memmove(reinterpret_cast<ptr_t>(IT(second_split_it)->c_va), reinterpret_cast<ptr_t>(vmm_data.get() + bytes_1st_page + 1), bytes_2nd_page);
            }
            else
            {
                _bfdebug << "write_to_c_page: we are writing to one page: " << hex_out_s(d_pa) << bfendl;

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
    get_flip_data(const int_t out_addr, const int_t out_size)
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
        _bfdebug << "clear_flip_data: clearing flip data" << bfendl;

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
    remove_flip_entry(const int_t rip)
    {
        expects(rip != 0);

        _bfdebug << "remove_flip_entry: removing flip entry for: " << hex_out_s(rip) << bfendl;

        std::lock_guard<std::mutex> flip_guard(g_flip_mutex);

        // Find relevant entry/entries by provided RIP address.
        auto &&vec = g_flip_log; // Shorter name.
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&rip](const flip_data &o)
        {
            return o.rip == rip;
        }), vec.end());

        return 1;
    }
};

#endif
