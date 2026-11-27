#ifndef SHM_H
#define SHM_H

#include "process.h"
#include "mm/vmm.h"

int shm_share(i32 target_pid, u32 src_va, size_t src_size, u32* out_va);
int shm_unshare(i32 target_pid, u32 target_va);

#endif
