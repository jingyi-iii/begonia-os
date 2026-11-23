/*
 * lib/elf.c - minimal ELF32 parsing and segment loading.
 *
 * Nothing here knows about the PMM, the VMM or page tables: elf_parse() turns
 * an image into a segment list, and elf_load_into() copies those segments
 * into memory the caller has already reserved, mapped and translated.
 */

#include "elf.h"
#include "errno.h"
#include "string.h"

int elf_parse(const void* image, u32 image_size, elf_image* out)
{
    if (!image || !out || image_size < sizeof(elf32_ehdr))
        return E_INVAL;

    const elf32_ehdr* h = (const elf32_ehdr*)image;

    if (h->ident[0] != ELF_IDENT_MAGIC0 || h->ident[1] != 'E' ||
        h->ident[2] != 'L' || h->ident[3] != 'F')
        return E_INVAL;
    if (h->ident[4] != ELF_CLASS32 || h->ident[5] != ELF_DATA2LSB)
        return E_INVAL;
    if (h->type != ELF_ET_EXEC || h->machine != ELF_EM_386)
        return E_INVAL;
    if (h->phentsize != sizeof(elf32_phdr) || h->phnum == 0)
        return E_INVAL;
    if (h->phoff > image_size ||
        (u32)h->phnum * sizeof(elf32_phdr) > image_size - h->phoff)
        return E_INVAL;

    out->seg_count = 0;

    for (u32 i = 0; i < h->phnum; i++) {
        const elf32_phdr* ph = (const elf32_phdr*)((const u8*)image +
                                                   h->phoff + i * sizeof(*ph));

        if (ph->type != ELF_PT_LOAD || ph->memsz == 0)
            continue;
        if (ph->filesz > ph->memsz)
            return E_INVAL;
        if (ph->offset > image_size || ph->filesz > image_size - ph->offset)
            return E_INVAL;
        if (out->seg_count == ELF_MAX_LOAD_SEGMENTS)
            return E_2BIG;

        elf_segment* s = &out->seg[out->seg_count++];
        s->vaddr  = ph->vaddr;
        s->offset = ph->offset;
        s->filesz = ph->filesz;
        s->memsz  = ph->memsz;
        s->flags  = ph->flags;
    }

    if (out->seg_count == 0)
        return E_INVAL;

    out->entry = h->entry;
    return 0;
}

int elf_load_into(const void* image, u32 image_size, const elf_image* img,
                  void* const* dst)
{
    if (!image || !img || !dst)
        return E_INVAL;
    if (img->seg_count == 0 || img->seg_count > ELF_MAX_LOAD_SEGMENTS)
        return E_INVAL;

    for (u32 i = 0; i < img->seg_count; i++) {
        const elf_segment* s = &img->seg[i];
        u8* out;

        if (!dst[i])
            return E_INVAL;
        if (s->filesz > s->memsz)
            return E_INVAL;
        if (s->offset > image_size || s->filesz > image_size - s->offset)
            return E_INVAL;

        out = (u8*)dst[i];
        memcpy(out, (const u8*)image + s->offset, s->filesz);
        memset(out + s->filesz, 0, s->memsz - s->filesz);
    }

    return 0;
}
