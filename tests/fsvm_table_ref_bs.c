/*
 * Copyright (C) 2026 DiamaneOS
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * For tests/fsvm_table_ref.py on a Linux host only (LD_PRELOAD into sgdisk): the scratch image named by
 * FSVM_TABLE_BS4096 answers the block device ioctls sgdisk asks as a disk with 4096-byte logical and physical
 * blocks of the image's size would. Every other descriptor and request goes to the real ioctl.
 *
 *     cc -shared -fPIC -o fsvm_table_ref_bs.so fsvm_table_ref_bs.c -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    static int (*real)(int, unsigned long, ...);
    if (!real) real = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    const char* path = getenv("FSVM_TABLE_BS4096");
    struct stat a, b;
    if (path && fstat(fd, &a) == 0 && stat(path, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
        switch (request) {
            case BLKSSZGET:
            case BLKPBSZGET:
                *(int*)arg = 4096;
                return 0;
            case BLKGETSIZE:
                *(unsigned long*)arg = (unsigned long)(a.st_size / 512);
                return 0;
            case BLKGETSIZE64:
                *(uint64_t*)arg = (uint64_t)a.st_size;
                return 0;
        }
    }
    return real(fd, request, arg);
}
