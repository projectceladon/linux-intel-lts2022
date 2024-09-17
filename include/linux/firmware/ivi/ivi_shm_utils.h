#ifndef _LINUX_IVI_SHM_H
#define _LINUX_IVI_SHM_H

#ifdef CONFIG_IVI_SHARE_MEMORY
void __meminit ivi_shm_detect(uint64_t start, uint64_t size, int type);

#else
static inline void ivi_shm_detect(uint64_t start, uint64_t size, int type)
{
}

#endif

#endif
