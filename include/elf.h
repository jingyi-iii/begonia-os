#ifndef ELF_H
#define ELF_H

#include "typedefs.h"

/*
 * Minimal ELF32 support, split so that lib/elf.c stays free of any memory
 * management:
 *
 *   elf_parse()      reads an image and returns its loadable segments.
 *   elf_load_into()  copies the segment data into memory the caller already
 *                    prepared (allocated, mapped and translated).  It never
 *                    touches the PMM, the VMM or the page tables.
 *
 * The caller is expected to: parse, reserve and map pages for every segment
 * (pmm + vmm), then hand elf_load_into() one destination pointer per segment.
 */

#define ELF_MAX_LOAD_SEGMENTS   8

/* e_ident */
#define ELF_IDENT_MAGIC0    0x7F    /* ident[0] */
#define ELF_CLASS32         1       /* ident[4]: 32-bit objects */
#define ELF_DATA2LSB        1       /* ident[5]: little endian */

/* e_type / e_machine */
#define ELF_ET_EXEC         2       /* executable */
#define ELF_EM_386          3       /* Intel 80386 */

/* p_type */
#define ELF_PT_LOAD         1       /* loadable segment */

/* p_flags */
#define ELF_PF_X            (1 << 0)
#define ELF_PF_W            (1 << 1)
#define ELF_PF_R            (1 << 2)

typedef struct elf32_ehdr {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u32 entry;
    u32 phoff;
    u32 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} __attribute__((packed)) elf32_ehdr;

typedef struct elf32_phdr {
    u32 type;
    u32 offset;
    u32 vaddr;
    u32 paddr;
    u32 filesz;
    u32 memsz;
    u32 flags;
    u32 align;
} __attribute__((packed)) elf32_phdr;

/* One PT_LOAD segment, as handed to the caller by elf_parse(). */
typedef struct elf_segment {
    u32 vaddr;      /* linked virtual address, not page aligned */
    u32 offset;     /* offset of the data inside the image */
    u32 filesz;     /* bytes copied from the image */
    u32 memsz;      /* full size; [filesz, memsz) must be zero filled */
    u32 flags;      /* ELF_PF_* */
} elf_segment;

typedef struct elf_image {
    u32         entry;
    u32         seg_count;
    elf_segment seg[ELF_MAX_LOAD_SEGMENTS];
} elf_image;

/*
 * elf_parse - validate an ELF32 executable and list its loadable segments.
 *
 * Pure: reads only @image.  Where the segments are allowed to live is the
 * caller's policy, not this function's.
 *
 * Returns 0, E_INVAL for a malformed image, or E_2BIG when the image has
 * more than ELF_MAX_LOAD_SEGMENTS loadable segments.
 */
int elf_parse(const void* image, u32 image_size, elf_image* out);

/*
 * elf_load_into - copy the segment data into memory the caller prepared.
 *
 * @img must be the result of elf_parse() on @image.  @dst holds one pointer
 * per segment: dst[i] is the address that belongs to img->seg[i].vaddr, i.e.
 * the caller has already reserved pages for that segment, mapped them at the
 * linked virtual address and translated the result into something it can
 * write through (typically pa_to_va(pa) + (vaddr & (PAGE_SIZE - 1))).
 *
 * For each segment, filesz bytes are copied to dst[i] and the bss tail up to
 * memsz is zeroed.  Does no allocation and touches no page tables.
 *
 * Returns 0, or E_INVAL when the image, the segment list or a destination is
 * inconsistent.
 */
int elf_load_into(const void* image, u32 image_size, const elf_image* img,
                  void* const* dst);

#endif /* ELF_H */
