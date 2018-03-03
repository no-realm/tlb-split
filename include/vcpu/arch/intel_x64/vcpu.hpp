//
// Bareflank Extension : tlb-split

#include <bfvmm/vcpu/arch/intel_x64/vcpu.h>

#include "../../../hve/arch/intel_x64/tlbs.hpp"

namespace tlbs {
namespace intel_x64 {

    class vcpu : public bfvmm::intel_x64::vcpu {
    public:
        /// Default Constructor
        ///
        /// @expects
        /// @ensures
        ///
        vcpu(vcpuid::type id) : bfvmm::intel_x64::vcpu{id} {}

        /// Destructor
        ///
        /// @expects
        /// @ensures
        ///
        ~vcpu() = default;

    public:
        //--------------------------------------------------------------------------
        // CRs
        //--------------------------------------------------------------------------

        /// Enable TLB Trapping
        ///
        /// @expects
        /// @ensures
        ///
        void enable_tlb_trapping() { m_tlbs = std::make_unique<::tlbs::intel_x64::tlbs>(this->exit_handler()); }

        /// Get TLB Object
        ///
        /// @expects
        /// @ensures
        ///
        /// @return Returns the TLB object stored in the vCPU if TLB trapping is
        ///         enabled, otherwise a nullptr is returned.
        ///
        auto* tlbs() { return m_tlbs.get(); }

    private:
        std::unique_ptr<::tlbs::intel_x64::tlbs> m_tlbs;
    };

}  // namespace intel_x64
}  // namespace tlbs
