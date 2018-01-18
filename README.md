# TLB splitting module for the [Bareflank Hypervisor](https://github.com/Bareflank/hypervisor)

## Description

This module adds TLB splitting to the [Bareflank Hypervisor](https://github.com/Bareflank/hypervisor) by providing
an IOCTL and VMCALL interface.<br/>
This module also makes use of the [Extended APIs](https://github.com/Bareflank/extended_apis) module.<br/>
For further information about the [Bareflank Hypervisor](https://github.com/Bareflank/hypervisor) and how to create extensions,
please see the following documentation.

[API Documentation](http://bareflank.github.io/hypervisor/html/)

## Compilation / Usage

This example uses both the [Bareflank Hypervisor](https://github.com/Bareflank/hypervisor), as well as a modified version of the [Extended APIs](https://github.com/Randshot/extended_apis) module.<br/>
The instructions below are for Windows and should be executed from inside Cygwin64.<br/>
*Replace `<bareflank path>` with your Bareflank path!*

```bash
# Change directory to the bareflank dir
cd <bareflank path>

# Clone the Bareflank hypervisor repo and cd into it
git clone https://github.com/Bareflank/hypervisor.git && cd hypervisor

# Clone both the extended_apis and the tlb-split repo
git clone https://github.com/Randshot/extended_apis.git
git clone https://github.com/Randshot/tlb-split.git src_tlb_split

# Setup Cygwin, but don't configure Bareflank yet
./tools/scripts/setup_cygwin.sh --no-configure

# Not needed, I think...
#./configure -m ./extended_apis/bin/extended_apis.modules

# Create the build directory and cd into it
cd .. && mkdir build && cd build

# Configure the Bareflank hypervisor and define the tlp-split repo as module
../hypervisor/configure -m ../hypervisor/src_tlb_split/bin/tlb_split.modules --compiler clang --linker $HOME/usr/bin/x86_64-elf-ld.exe

# Execute make
make
```

To run the monitor application, we need to first load the hypervisor and then
run the monitor app that will output information about page flips.

```bash
make driver_load
make quick

makefiles/src_tlb_split/app/bin/native/hook.exe

make stop
make driver_unload
```

For more information about the monitor application use the `--help` option.

```bash
makefiles/src_tlb_split/app/bin/native/hook.exe --help
```

## Aliases

These are the aliases that I have defined in my `.bashrc` (`/home/<username>/.bashrc`) file.<br/>
*Replace `<bareflank path>` with your Bareflank path!*

```bash
# Bareflank aliases
alias bfdir='cd <bareflank path>/build'
alias bfmake='make'
alias bfstart='make quick'
alias bfstop='make stop'
alias bfload='make driver_load'
alias bfunload='make driver_unload'
alias bfrestart='bfstop && bfstart'
alias bfrecompile='bfstop && bfunload && bfmake && bfload && bfstart'
alias bfmonitor='makefiles/src_tlb_split/app/bin/native/hook.exe'
```
