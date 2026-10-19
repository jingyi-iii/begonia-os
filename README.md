# begonia-os

A toy operating system for i686, built from source with its own cross toolchain and run under
qemu.

What exists today is one 32-bit Multiboot v1 image. GRUB or `qemu -kernel` hands control to it
in protected mode, it installs its own flat GDT, sets `CR0.PE` and prints to COM1 before asking
qemu to exit. The servers are empty; this is a startup image, a kernel entry point and a
toolchain so far.

Nothing is installed into the host to build it. `tools/build-toolchain.sh` compiles gcc 16.2.0,
binutils 2.46.0 and gdb 17.2 from the tarballs in `tools/packages/` into `tools/local`, a
directory inside the checkout. `$HOME` and `~/.bashrc` are never touched, and an `x86_64-elf`
toolchain can share the same prefix.

| Path | What it is |
| --- | --- |
| `Makefile` | builds `build/boot.elf` and the optional ISO from `startup/`, `arch/`, `include/` |
| `linker.ld` | load address, section order and entry point of the image |
| `startup/` | `boot.S`, the Multiboot entry point |
| `include/` | headers shared across directories |
| `kernel/` | the kernel: so far only the entry point |
| `lib/` | freestanding helpers: `memset`, `memcpy` |
| `servers/` | user-space servers, empty |
| `tools/` | the toolchain: build scripts, fetched sources, and the `local/` install prefix |
| `run.sh` | the entry point: picks a target, builds the toolchain if missing, boots in qemu |

## Install

Linux or WSL, `bash` 4.4 or newer, and a host compiler. The first run installs `build-essential`
and `m4` through apt unless you pass `--skip-deps`. qemu is needed to run the image, not to
build it; `--with-os-deps` adds qemu, `grub-pc-bin` and `xorriso` to that apt line.

```bash
git clone https://github.com/jingyi-iii/begonia-os.git
cd begonia-os

bash tools/fetch-packages.sh    # ~162 MB of toolchain sources, checked against sha256 pins
./run.sh                        # asks 32 or 64, builds the toolchain on first use, then boots
```

The first run is the slow one: gcc alone takes 8 to 15 minutes. Every run after that reuses the
toolchain, and `bash tools/build-toolchain.sh` skips when it is already installed. `--force`
rebuilds it, `--clean` removes the prefix, and `--only gmp,mpfr,mpc,isl,zstd,binutils,gdb,gcc`
rebuilds a subset.

## Use

`run.sh` passes any make target through to the top-level `Makefile`:

```bash
./run.sh            # build and boot in qemu, which exits by itself after the message
./run.sh all        # build only
./run.sh clean      # clean the image build
./run.sh iso        # bootable GRUB ISO, needs grub-mkrescue and xorriso
./run.sh run-iso    # boot that ISO from the emulated CD-ROM
./run.sh --64       # use the x86_64-elf toolchain, built on first use
```

`source tools/env.sh` puts the cross tools on `PATH` in the current shell, after which
`make` at the top of the tree works on its own. Details of the layout, the dependency layers and the
conventions live in [`AGENTS.md`](AGENTS.md).

## Notes

`kernel/` and `servers/` are placeholders with a `.gitkeep` each, so a fresh clone has the
directories the layout promises.

The toolchain sources are not committed. `gcc-16.2.0.tar.xz` alone is 107 MB, over GitHub's
100 MiB per-file limit, and the whole set would add 162 MB to every clone, so
`bash tools/fetch-packages.sh` fetches them from TUNA or upstream and checks every sha256.
`tools/local/` and `tools/build/` are untracked for the same reason: the prefix is 4 GB, and
the build directory is temporary and as large as the component being built.

The operating system code is MIT. The fetched sources keep their own licences: gcc, binutils
and gdb are GPL 3 or later, gmp, mpfr and mpc are LGPL 3 or later, isl is MIT and zstd is
BSD 3-Clause. Nothing is vendored.

It is developed under WSL (Debian) on Windows. The scripts assume bash 4.4, a host compiler and
m4, and nothing else.

## License

MIT
