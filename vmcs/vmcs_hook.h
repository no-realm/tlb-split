#ifndef VMCS_HOOK_H
#define VMCS_HOOK_H

#include <vmcs/root_ept_intel_x64.h>
#include <vmcs/vmcs_intel_x64_eapis.h>

using namespace intel_x64;
using namespace vmcs;

#ifndef MAX_PHYS_ADDR
#define MAX_PHYS_ADDR 0x2000000000
#endif

std::unique_ptr<root_ept_intel_x64> g_root_ept;

class vmcs_hook : public vmcs_intel_x64_eapis
{
public:

    /// Default Constructor
    ///
    vmcs_hook() = default;

    /// Destructor
    ///
    ~vmcs_hook() override = default;

    /// Write Fields
    ///
    /// We override this function so that we can setup the VMCS the way we
    /// want.
    void
    write_fields(gsl::not_null<vmcs_intel_x64_state *> host_state,
                 gsl::not_null<vmcs_intel_x64_state *> guest_state) override
    {
        static auto initialized = false;

        // Let Bareflank do it's thing before we setup the VMCS. This setups
        // up a lot of default fields for us, which we can always overwrite
        // if we want once this is done.
        vmcs_intel_x64_eapis::write_fields(host_state, guest_state);

        if (!initialized)
        {
            g_root_ept = std::make_unique<root_ept_intel_x64>();

            // Setup identity map (2m)
            g_root_ept->setup_identity_map_2m(0, MAX_PHYS_ADDR);

            // Since EPT in the Extended APIs is global, we should only set it
            // up once.
            initialized = true;

            bfdebug << "vmcs_hook: set up identity map (2m)" << bfendl;
        }

        // Enable EPT and VPID. If your going to use EPT, you really should be
        // using VPID as well, and Intel comes with TLB invalidation
        // instructions that leverage VPID, which provide per-line invalidation
        // which you don't get without VPID. We also need to set the eptp that
        // we plan to use.
        this->enable_vpid();
        this->enable_ept();
        this->set_eptp(g_root_ept->eptp());
    }
};

#endif
