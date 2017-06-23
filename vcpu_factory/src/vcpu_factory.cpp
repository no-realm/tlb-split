#include <vcpu/vcpu_factory.h>
#include <vcpu/vcpu_intel_x64.h>

#include <vmcs/vmcs_hook.h>
#include <exit_handler/tlb_handler.h>

std::unique_ptr<vcpu>
vcpu_factory::make_vcpu(vcpuid::type vcpuid, user_data *data)
{
    auto &&my_vmcs = std::make_unique<vmcs_hook>();
    auto &&my_tlb_handler = std::make_unique<tlb_handler>();

    (void) data;
    return std::make_unique<vcpu_intel_x64>(
               vcpuid,
               nullptr,                         // default debug_ring
               nullptr,                         // default vmxon
               std::move(my_vmcs),
               std::move(my_tlb_handler),
               nullptr,                         // default vmm_state
               nullptr);                        // default guest_state
}
