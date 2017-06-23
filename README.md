# TLB splitting module for the [Bareflank Hypervisor](https://github.com/Bareflank/hypervisor)

## Description

This module adds TLB splitting to the [Bareflank Hypervisor](https://github.com/Bareflank/hypervisor) by providing
an IOCTL and VMCALL interface. This module also makes use of the [Extended APIs](https://github.com/Bareflank/extended_apis) module.
For further information about the [Bareflank Hypervisor](https://github.com/Bareflank/hypervisor) and how to create extensions,
please see the following documentation.

[API Documentation](http://bareflank.github.io/hypervisor/html/)

## Compilation / Usage

This example uses both the [Bareflank Hypervisor](https://github.com/Bareflank/hypervisor), as well as the [Extended APIs](https://github.com/Bareflank/extended_apis) module.
The instructions below are for Windows and should be executed from inside Cygwin64.

```
cd ~/
git clone https://github.com/Bareflank/hypervisor.git
cd hypervisor
git clone https://github.com/Bareflank/extended_apis.git
git clone https://github.com/Randshot/tlb-split.git src_tlb_split

./tools/scripts/setup_cygwin.sh --no-configure

cd ..
mkdir build
cd build

../hypervisor/configure -m ../hypervisor/src_tlb_split/bin/tlb_split.modules --compiler clang --linker $HOME/usr/bin/x86_64-elf-ld.exe
make
```

To run this example, we need to first load the hypervisor, and then run the
example app that will get hooked by the hypervisor.

```
make driver_load
make quick

makefiles/src_tlb_split/app/bin/native/hook.exe

make stop
make driver_unload
```
