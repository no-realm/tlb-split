#include <ioctl.h>
#include <guard_exceptions.h>

#include <iostream>
#include <cstring>

void
hello_world()
{ std::cout << "hello world" << std::endl << std::endl; }

void
hooked_hello_world()
{ std::cout << "hooked hello world" << std::endl << std::endl; }

int
main(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;

    vmcall_registers_t regs;

    guard_exceptions([&]
    {
        // Open IOCTL connection.
        ioctl ctl;
        ctl.open();

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

        // VMCALL: Check if an hv is present.
        regs.r00 = VMCALL_REGISTERS;
        regs.r01 = VMCALL_MAGIC_NUMBER;
        regs.r02 = 0;
        ctl.call_ioctl_vmcall(&regs, 0);
        std::cout << "hv_present: " << (regs.r02 == 1 ? "yes" : "no") << std::endl;

        // VMCALL: Check if page is split.
        regs.r00 = VMCALL_REGISTERS;
        regs.r01 = VMCALL_MAGIC_NUMBER;
        regs.r02 = 5;
        regs.r03 = reinterpret_cast<uintptr_t>(hello_world);
        ctl.call_ioctl_vmcall(&regs, 0);
        std::cout << "is_split: " << (regs.r02 == 1 ? "yes" : "no") << std::endl;

        std::cout << "before create_split_context: ";
        hello_world();

        // VMCALL: Create new split.
        regs.r00 = VMCALL_REGISTERS;
        regs.r01 = VMCALL_MAGIC_NUMBER;
        regs.r02 = 1;
        regs.r03 = reinterpret_cast<uintptr_t>(hello_world);
        ctl.call_ioctl_vmcall(&regs, 0);
        std::cout << "create_split_context: " << (regs.r02 == 1 ? "success" : "failure") << std::endl;

        std::cout << "after create_split_context: ";
        hello_world();

        // VMCALL: Check if page is split.
        regs.r00 = VMCALL_REGISTERS;
        regs.r01 = VMCALL_MAGIC_NUMBER;
        regs.r02 = 5;
        regs.r03 = reinterpret_cast<uintptr_t>(hello_world);
        ctl.call_ioctl_vmcall(&regs, 0);
        std::cout << "is_split: " << (regs.r02 == 1 ? "yes" : "no") << std::endl;

        // VMCALL: Activate split.
        regs.r00 = VMCALL_REGISTERS;
        regs.r01 = VMCALL_MAGIC_NUMBER;
        regs.r02 = 2;
        regs.r03 = reinterpret_cast<uintptr_t>(hello_world);
        ctl.call_ioctl_vmcall(&regs, 0);
        std::cout << "activate_split: " << (regs.r02 == 1 ? "success" : "failure") << std::endl;

        std::cout << "after activate_split: ";
        unsigned char* v = new unsigned char[8];
        std::memmove(v, reinterpret_cast<void*>(hello_world), 8);
        delete v;
        hello_world();

        // VMCALL: Check if page is split.
        regs.r00 = VMCALL_REGISTERS;
        regs.r01 = VMCALL_MAGIC_NUMBER;
        regs.r02 = 5;
        regs.r03 = reinterpret_cast<uintptr_t>(hello_world);
        ctl.call_ioctl_vmcall(&regs, 0);
        std::cout << "is_split: " << (regs.r02 == 1 ? "yes" : "no") << std::endl;

        // VMCALL: Deactivate split.
        regs.r00 = VMCALL_REGISTERS;
        regs.r01 = VMCALL_MAGIC_NUMBER;
        regs.r02 = 3;
        regs.r03 = reinterpret_cast<uintptr_t>(hello_world);
        ctl.call_ioctl_vmcall(&regs, 0);
        std::cout << "deactivate_split: " << (regs.r02 == 1 ? "success" : "failure") << std::endl;

        std::cout << "after deactivate_split: ";
        hello_world();
    });

    return 0;
}
