//
// Bareflank Extension : tlb-split

#include <bfvmm/vcpu/vcpu_factory.h>
#include <tlb-split/vcpu/arch/intel_x64/vcpu.hpp>

namespace bfvmm {

std::unique_ptr<vcpu> vcpu_factory::make_vcpu(vcpuid::type vcpuid, bfobject* obj) {
    bfignored(obj);
    return std::make_unique<::tlbs::intel_x64::vcpu>(vcpuid);
}

}  // namespace bfvmm
