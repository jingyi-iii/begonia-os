# begonia-os

A toy OS project. Target architecture is i686 (i386); x86_64 (long mode) is planned. It is
built with the project's own cross toolchain and run under qemu.

## Layout

- `bootloader/` — Multiboot v1 entry (32-bit protected mode: `boot.S`, `protmode.c`,
  `interrupt.c`, `interrupt.S`, `com.c`, `linker.ld`, `Makefile`); builds to `build/boot.elf`,
  plus an optional GRUB ISO `build/boot.iso`
- `kernel/` — the kernel (not implemented yet; it needs a `Makefile`)
- `servers/` — servers (not implemented yet)
- `tools/` — cross toolchain sources and build scripts
  - `packages/` — toolchain source tarballs, fetched by `fetch-packages.sh`
    (not committed; see `packages/README.md`)
  - `local/` — build output (the toolchain install prefix, inside the project; never writes
    `$HOME` and never touches `~/.bashrc`)
  - `build-toolchain.sh` — script that builds the toolchain
  - `env.sh` — `source` it to put the toolchain on `PATH`
- `run.sh` — top-level entry: asks which toolchain to use (32/64), builds it if missing, then
  builds and runs the bootloader in `bootloader` via qemu

## Toolchain

- Default target triple: `i686-elf` (gcc 16.2.0 / binutils 2.46.0 / gdb 17.2)
- The target is configurable: `--target <triple>` (or `TARGET=<triple>`), e.g. `x86_64-elf` for
  long mode. Several targets can coexist in the same prefix; the host libraries are shared and
  each target's tools live under `bin/<triple>-*` and `lib/gcc/<triple>/`.
- Install prefix: `tools/local` (resolved relative to the script's own directory, so it
  runs from any cwd)
- Use: `source tools/env.sh` (exports `OS_TOOLS`/`I686_TOOLS` and prepends
  `$OS_TOOLS/bin` to `PATH`)
- `build-toolchain.sh` is idempotent: it skips when already installed; `--force` rebuilds,
  `--clean` removes the whole prefix.

## Dependency layers (self-containment strategy)

1. **Bootstrap seed (cannot be removed with pure source builds)**: `build-essential` + `m4`
   (plus core tools such as sh/sed/awk/tar).
   This is the minimum host dependency needed to compile the first cross compiler; getting rid of
   it entirely would require using a prebuilt compiler as the seed.
2. **Compiler-internal libraries (bundled and statically linked from source)**: `gmp 6.3.0`,
   `mpfr 4.2.2`, `mpc 1.3.1`, `isl 0.24`, `zstd 1.5.7`.
   All are built into `local/` with `--disable-shared --enable-static`, and gcc/gdb consume them
   via `--with-gmp/mpfr/mpc/isl/zstd`.
   Verified: `cc1` / `cc1plus` / `lto1` / `i686-elf-gdb` do not depend on the host
   `libgmp/libmpfr/libmpc/libisl/libzstd`.
3. **OS runtime side (installed only with `--with-os-deps`)**: `xorriso`, `grub-pc-bin`, and a
   qemu matching the target (`qemu-system-i386`, or `qemu-system-x86_64` for `x86_64-*`).
   Unrelated to building the toolchain; kept as a separate layer.

## Notes

- **Keep library versions in sync with GCC**: the gmp/mpfr/mpc/isl versions match GCC's official
  `contrib/download_prerequisites` (currently `gmp 6.3.0`, `mpfr 4.2.2`, `mpc 1.3.1`, `isl 0.24`);
  zstd is independent.
  When bumping GCC, re-check these to avoid API/ABI incompatibilities.
- **Release tarballs do not need `bison`/`flex`/`texinfo`** (pre-generated files are included);
  only git checkouts do.
- **A full rebuild is expensive** (GCC takes roughly 8-15 minutes). Prefer incremental rebuilds
  over `--force`:
  `bash tools/build-toolchain.sh --only zstd,gcc`
  Available components: `gmp,mpfr,mpc,isl,zstd,binutils,gdb,gcc` (in dependency order).
- **`tools/build/`** is a temporary directory, cleaned automatically after a successful
  build; do not put anything there.
- The scripts require **bash >= 4.4**.
- GNU's official mirrors are slow; `bash tools/fetch-packages.sh` restores the expected set from
  TUNA / GitHub and verifies every sha256. To add a package by hand, drop it into `packages/`
  named `name-version.tar.*` (the script matches them by glob) and list it in `fetch-packages.sh`.
- **`tools/local/`, `tools/build/`, `bootloader/build/` and the `packages/` tarballs are never
  committed** (`.gitignore` covers them): the prefix is several GB and `gcc-*.tar.xz` is over
  GitHub's 100 MiB per-file limit.
- To change the `local/` prefix or the `packages/` path, edit `PREFIX` and each component's
  `fpath` at the top of `build-toolchain.sh`.

## Bootloader

- `bootloader/` builds a 32-bit Multiboot v1 ELF (`build/boot.elf`) loaded at 1 MiB. `boot.S` is
  assembled with `$(TARGET)-as --32` and linked with `$(TARGET)-ld -m elf_i386`; there is no
  512-byte boot sector and no `objcopy` step. Both toolchains can build it (the kernel itself is
  still 32-bit, assembled with `--32`).
- `protmode.c` is compiled into the same image with `$(TARGET)-gcc -m32
  -ffreestanding -fno-pic -fno-stack-protector -fno-builtin`: it installs a flat
  4 GiB GDT of its own (`lgdt` + segment reload + far jump) and then sets
  `CR0.PE` (**bit 0**) to be in protected mode. Never set `CR0.PG` (**bit 31**)
  there: with no page tables in `CR3` the next instruction fetch faults and, with
  no IDT installed, the CPU triple-faults (qemu shows a silent reset loop) and
  control never returns to `boot.S`. Multiboot already hands over with `PE=1`, so
  the sequence is idempotent on that path; it stays explicit for a real-mode boot.
- The Multiboot header (magic/flags/checksum) lives in its own `.multiboot` section, which
  `linker.ld` places first so it stays within the first 8 KiB of the file, as GRUB requires.
  Validate it with `grub-file --is-x86-multiboot build/boot.elf`.
- GRUB/qemu enter `_start` directly in 32-bit protected mode, so BIOS calls are unavailable;
  the messages go to COM1 instead of through `int 14h`.  That is `com.c` (`com_init`,
  `com_putc`, `com_print`, `com_hex`, declared in `com.h`), shared by `boot.S` and
  `interrupt.c`.  It polls LSR at `0x3FD` before every byte - polling the data port at
  `0x3F8` for "transmitter empty" never succeeds and hangs the caller.
- After the message it writes to port `0x501`, so qemu exits by itself (`-device isa-debug-exit`;
  the exit code is `(value << 1) | 1`); on real hardware that write is ignored and it falls back
  to `cli; hlt`.
- `make run` boots the ELF with qemu's built-in Multiboot loader (`-kernel`) and `-nographic`;
  qemu exits automatically after the message, so no key press is needed.
- `make iso` builds a bootable GRUB ISO (`build/boot.iso`) with `grub-mkrescue`, and
  `make run-iso` boots it from the emulated CD-ROM (`-cdrom -boot d`).
- Output goes to serial only: under `-nographic` qemu mirrors COM1 to stdio, while direct VGA
  text writes would not be visible.

## Interrupts

- `interrupt.S` holds the stub table. `isr_tbl` is 256 slots of 0x90 bytes, one per IDT vector, so
  a vector's entry point is `isr_tbl + vector * 0x90` and the C side needs no per-vector symbols.
- Each slot is a 12-byte stub - `pushl $0` for the vectors the CPU brings no error code for,
  `pushl $vector`, `jmp isr_common` - followed by NOP padding. The padding is computed with `.org`
  from the slot index rather than from the stub length: gas relaxes `jmp` to its short form where it
  fits, which would leave that one slot three bytes short and break the stride.
- `isr_common` does `pusha`, hands the frame to `isr_dispatch()` and irets. The frame is
  `struct isr_frame` in interrupt.c (52 bytes, held by a `_Static_assert`): pusha, then vector,
  error code, eip, cs, eflags. Keep the two files in step.
- `interrupt_init()` points all 256 gates at their slot with DPL 0 and loads the table with `lidt`;
  `boot.S` calls it right after `switch_to_protect_mode`. A vector user mode must be able to call
  (a syscall gate) is re-installed later with `idt_set_gate(..., 3)`.
- `isr_dispatch()` logs `isr vector= err= eip= cs=` over COM1 and returns, so a trap (an `int n`,
  `#BP`, `#OF`) continues where it left off. **A fault must not be returned from blindly**: the CPU
  saves the faulting instruction, so `iret` runs it again and the log floods - #DE and #GP loop
  forever. Either fix the cause and retry (what #PF is for) or do not return at all.
- Only the pusha registers are saved, not DS/ES/FS/GS: everything still runs in ring 0 on the flat
  GDT. Add push/pop ds/es/fs/gs plus a reload of 0x10 before user mode arrives.
- `boot.S` raises `int 1` and `int 0x3F` on every boot, so the slots, the frame and `iret` stay
  exercised. Both are traps and return.

## grub / xorriso

- Both **can be built from source**, but they are only needed for the GRUB `grub-mkrescue`
  bootable-ISO path (the `bootloader` `iso` / `run-iso` targets):
  - `xorriso` depends on `libisofs` + `libisoburn` + `libburn` (plus `libacl`, etc.);
  - building GRUB from source pulls in even more (freetype/fontconfig/gettext/fuse, etc.).
- They are **host packaging tools**, not compiler-internal libraries, so they live on the OS
  runtime side and are better sourced from distro packages.
- Lighter alternatives for a toy OS (can avoid grub/xorriso entirely):
  - give the kernel a multiboot header and boot directly with `qemu-system-i386 -kernel os.elf`
    (this is what the `bootloader` `run` target does);
  - use **Limine** (official prebuilt binaries + the `limine` command);
  - use `genisoimage`/`mkisofs` instead of `xorriso`.
- If a completely apt-free setup is required, the libraries/GRUB above can also be added to
  `packages/` and built from source.

## Targeting x86_64 long mode

- A GCC build is locked to one target triple, so `i686-elf` cannot emit x86_64 code. To run in
  long mode, build a second toolchain with `--target x86_64-elf` (the host libraries are reused).
- Compiler flags (kernel side, not the toolchain):
  `-m64 -mcmodel=kernel -mno-red-zone -ffreestanding -fno-stack-protector`.
- The boot path decides whether a 32-bit toolchain is still needed:
  - Limine or GRUB2 (multiboot2, 64-bit): kernel is pure `x86_64-elf` — no 32-bit toolchain;
  - own trampoline: same x86_64 toolchain, switch with `.code32`/`.code64` in assembly;
  - 32-bit code in C: add `--enable-multilib` to gcc and `--enable-targets=i686-elf` to binutils.

## Language conventions

- Conversation with the user: Chinese.
- All generated artifacts and files (including this `AGENTS.md`): English.

## README style

`README.md` follows the house style of the other `jingyi-iii` repositories
(`tangerine-screenshot`, `tangerine-nudge`, `tangerine-github-access`, `begonia-pciesim`):

- the title is `# <repo-name>` alone, with no tagline on the same line;
- two or three short prose paragraphs open it: what it is, what works today, what is unusual
  about it. No bullet list and no bold `**Status:**` line up front;
- one table follows, here the layout table;
- then `## Install`, `## Use`, `## Notes`, `## License`, in that order. `## How it works` may be
  inserted between `Use` and `Notes` when there is something worth explaining;
- commands in a block carry `#` comments that state their cost (size, minutes);
- `## Notes` states the limitations, why the large artefacts are untracked, the licences of the
  dependencies, and the development environment, in the first person and plainly;
- `## License` is that heading followed by `MIT` and nothing else;
- no badges, no emoji, no table of contents, hard wrap at 100 columns.

## Commit messages

[Conventional Commits](https://www.conventionalcommits.org/), in English, with the subsystem as
the scope:

```
<type>(<scope>): <imperative summary, <= 50 chars, no trailing period>

<body: why the change is needed, not what the diff already shows; wrap at 72>

<footer: BREAKING CHANGE: ..., Closes #12, Refs: ...>
```

- **Keep it short**: subject plus two to four lines of body. Detail belongs in
  `AGENTS.md`, in the code comments, or in the file that changed - not in the log.
- `type`: `feat`, `fix`, `build`, `docs`, `refactor`, `perf`, `test`, `chore`, `ci`, `style`,
  `revert`
- `scope`: `bootloader`, `kernel`, `servers`, `tools`, `run`, `agents`, `docs`
- One logical change per commit, and every commit must build (`./run.sh --32 all`).
- Mark incompatible changes with `!` after the scope, or with a `BREAKING CHANGE:` footer.
- `.gitmessage` holds the template and `git config commit.template .gitmessage` is set in this
  checkout, so `git commit` opens the editor with it.

## Common commands

```bash
bash tools/build-toolchain.sh                    # minimal host deps (idempotent)
bash tools/build-toolchain.sh --with-os-deps     # additionally install qemu/grub/xorriso
bash tools/build-toolchain.sh --force            # full rebuild
bash tools/build-toolchain.sh --only zstd,gcc    # rebuild selected components only
bash tools/build-toolchain.sh --target x86_64-elf --only binutils,gdb,gcc  # add x86_64 tools
source tools/env.sh                              # use the toolchain in the current shell

./run.sh                  # ask for the toolchain, build the kernel, launch qemu
./run.sh all              # build only
./run.sh clean            # clean the bootloader build
./run.sh iso              # build the GRUB ISO (grub-mkrescue)
./run.sh run-iso          # boot the GRUB ISO from the emulated CD-ROM
```

> `./run.sh` builds the Multiboot ELF and boots it in qemu; it exits automatically after printing
> the message.
