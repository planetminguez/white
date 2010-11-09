#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <getopt.h>

struct regs {
    uint32_t ttbr0;
    uint32_t ttbr1;
    uint32_t ttbcr;
    uint32_t contextidr;
    uint32_t sctlr;
    uint32_t scr;
    uint32_t dbgdidr;
    uint32_t dbgdrar;
    uint32_t dbgdsar;
    uint32_t id_dfr0;
    uint32_t dbgdscr;
};

int copy_phys(uint32_t paddr, uint32_t size, void *buf) {
    return syscall(8, 1, paddr, size, buf);
}

int kread(uint32_t addr, uint32_t size, void *buf) {
    return syscall(8, 2, addr, size, buf);
}

uint32_t read32(uint32_t addr) {
    return syscall(8, 3, addr);
}

uint32_t read32_phys(uint32_t paddr) {
    return syscall(8, 16, paddr);
}

static const char *cacheable(uint32_t flags) {
    const char *descs[] = {
        "Non-cacheable",
        "Write-Back, Write-Allocate",
        "Write-Through, no Write-Allocate",
        "Write-Back, no Write-Allocate"
    };
    return descs[flags];
}

static const char *tex(uint32_t tex, uint32_t c, uint32_t b) {
    if(tex & 4) {
        static char buf[1024];
        sprintf(buf, "Cacheable, Outer:%s, Inner:%s", cacheable(tex & 3), cacheable(((c & 1) << 1) | (b & 1)));
        return buf;
    }

    const char *descs[] = {
        "Strongly-ordered",
        "Shareable Device",
        "Outer and Inner Write-Through, no Write-Allocate",
        "Outer and Inner Write-Back, no Write-Allocate",
        "Outer and Inner Non-cacheable",
        "Reserved! 00101",
        "Implementation defined! 00110",
        "Outer and Inner Write-Back, Write-Allocate",
        "Non-shareable Device",
        "Reserved! 01001",
        "Reserved! 01010",
        "Reserved! 01011",
    };
    uint32_t thing = ((tex & 7) << 2) |
                     ((c & 1) << 1) |
                     (b & 1);
    return descs[thing];
}

static const char *ap(uint32_t ap) {
    const char *descs[] = {
        "X/X",
        "RW/X",
        "RW/RO",
        "RW/RW",
        "100!",
        "RO/X",
        "RO/RO [deprec.]",
        "RO/RO",
    };
    return descs[ap & 3];
}

static void dump_pagetable(uint32_t ttbr, uint32_t baseaddr, uint32_t size) {
    unsigned int *data = malloc(size);
    assert(!copy_phys(ttbr & ~0x3f, size, data));
    for(int i = 0; i < (size / 4); i++) {
        unsigned int l1desc = data[i];
        if((l1desc & 3) == 0) continue; // fault
        printf("%08x: ", baseaddr + i * 0x100000);
        switch(l1desc & 3) {
        case 1: {
            printf("page table base=%x P=%d domain=%d\n", l1desc & ~0x3ff, (l1desc & (1 << 9)) & 1, (l1desc >> 5) & 0xf);
            unsigned int data2[256];
            memset(data2, 0xff, sizeof(data2));
            assert(!copy_phys(l1desc & ~0x3ff, sizeof(data2), data2));
            for(int j = 0; j < 256; j++) {
                unsigned int l2desc = data2[j];
                if((l2desc & 3) == 0) continue; // fault
                printf("  %08x: ", baseaddr + (i * 0x100000) + (j * 0x1000));
                switch(l2desc & 3) {
                case 0:
                    printf("fault\n");
                    break;
                case 1:
                    printf("large base=%x [%s] XN=%x nG=%x S=%x AP=%s\n",
                        l2desc & 0xffff0000,
                        tex(l2desc >> 12, l2desc >> 3, l2desc >> 2),
                        (l2desc >> 15) & 1,
                        (l2desc >> 11) & 1,
                        (l2desc >> 10) & 1,
                        ap(((l2desc >> 7) & 4) | ((l2desc >> 4) & 3)));
                    break;
                case 2:
                case 3:
                    printf("small base=%x [%s] XN=%x nG=%x S=%x AP=%s\n",
                        l2desc & 0xfffff000,
                        tex(l2desc >> 6, l2desc >> 3, l2desc >> 2),
                        l2desc & 1,
                        (l2desc >> 11) & 1,
                        (l2desc >> 10) & 1,
                        ap(((l2desc >> 7) & 4) | ((l2desc >> 4) & 3)));
                    break;
                }
            }
            } break;
        case 2:
            if(l1desc & (1 << 18)) {
                printf("supersection base=%x extbase=%x NS=%d nG=%d [%s] AP=%s extbase2=%x XN=%x\n",
                    l1desc & 0xff000000,
                    (l1desc >> 20) & 0xf,
                    (l1desc >> 19) & 1,
                    (l1desc >> 17) & 1,
                    tex(l1desc >> 12, l1desc >> 3, l1desc >> 2),
                    ap(((l1desc >> 15) ? 4 : 0) | ((l1desc >> 10) & 3)),
                    (l1desc >> 5) & 0xf,
                    (l1desc >> 4) & 1);

            } else {
                printf("section!!\n");
            }
            break;
        case 3:
            printf("fine page table!!\n");
            break;
        }
    }
}

static void dump_creep() {
    struct rec {
        uint32_t address;
        uint32_t value;
    } __attribute__((packed));
    struct rec *buf = calloc(1048576, sizeof(struct rec));
    assert(!syscall(8, 11, buf, 1048576 * sizeof(struct rec)));
    for(int i = 0; i < 1048576; i++) {
        if(buf[i].address != 0 || buf[i].value != 0) {
            printf("[%d] %08x: %08x\n", i, buf[i].address, buf[i].value);
        }
    }
    free(buf);
}

static void dump_protoss() {
    size_t size = 0x4000 * sizeof(uint32_t);
    uint32_t *buf = malloc(size);
    memset(buf, 0xff, size);
    assert(!syscall(8, 14, buf, size));
    for(int i = 0; i < (size / sizeof(uint32_t)); i++) {
        printf("%.6d %08x\n", i, buf[i]);
    }
    free(buf);
}

uint32_t parse_hex(const char *optarg) {
    errno = 0;
    char *end;
    long long ret = strtoll(optarg, &end, 16);
    if(errno) {
        printf("Can't parse %s: %s\n", optarg, strerror(errno));
        abort();
    } else if(*end) {
        printf("Invalid hex string: %s\n", optarg);
        abort();
    } else {
        return (uint32_t) ret;
    }
}

int main(int argc, char **argv) {
    int c;
    bool did_something = false;
    struct regs regs;
    assert(!syscall(8, 0, &regs));
    
    struct option options[] = {
        {"ioreg", required_argument, 0, 128},
        {"crash-kernel", no_argument, 0, 129},
        {"test-protoss", no_argument, 0, 130},
        {0, 0, 0, 0}
    };
    int idx;
    while((c = getopt_long(argc, argv, "r012sl:p:uh:v:w:c:CPUd:D", options, &idx)) != -1) {
        did_something = true;
        switch(c) {
        case 'r':
            printf("ttbr0=%x ttbr1=%x ttbcr=%x contextidr=%x sctlr=%x scr=%x\n", regs.ttbr0, regs.ttbr1, regs.ttbcr, regs.contextidr, regs.sctlr, regs.scr);
            printf("dbgdidr=%x dbgdrar=%x dbgdsar=%x id_dfr0=%x dbgdscr=%x\n", regs.dbgdidr, regs.dbgdrar, regs.dbgdsar, regs.id_dfr0, regs.dbgdscr);
            break;
        case '0':
            dump_pagetable(regs.ttbr0, 0, 0x1000);
            break;
        case '1':
            dump_pagetable(regs.ttbr1, 0, 0x4000);
            break;
        case '2':
            dump_pagetable(regs.ttbr1 - 0x4000, 0, 0x4000);
            break;
        case 's':
            assert(!syscall(8, 5));
            break;
        case 'l':
            printf("%08x\n", read32(parse_hex(optarg)));
            break;
        case 'p':
            printf("%08x\n", read32_phys(parse_hex(optarg)));
            break;
        case 'u':
            assert(!syscall(8, 6));
            break;
        case 'h':
            assert(!syscall(8, 7, parse_hex(optarg)));
            break;
        case 'v':
            assert(!syscall(8, 8, parse_hex(optarg)));
            break;
        case 'w':
            assert(!syscall(8, 9, parse_hex(optarg)));
            break;
        case 'c': {
            char *a = strsep(&optarg, "+");
            assert(a && optarg);
            assert(!syscall(8, 10, parse_hex(a), parse_hex(optarg)));
            break;
        }
        case 'C':
            dump_creep();
            break;
        case 'P':
            dump_protoss();
            break;
        case 'U':
            assert(!syscall(8, 12));
            break;
        case 128:
            printf("%p\n", (void *) syscall(8, 13, optarg));
            break;
        case 129:
            syscall(8, 4);
            break;
        case 130:
            assert(!syscall(8, 15));
            break;
        case 'd':
            assert(!syscall(8, 17, parse_hex(optarg)));
            break;
        case 'D':
            assert(!syscall(8, 18));
            break;
        case '?':
        default:
            goto usage;
        }
    }

    if(!did_something) goto usage;
    return 0;
usage:
    printf("Usage: %s ...\n"
           "    -r:           print some regs\n"
           "    -0:           dump memory map at ttbr0\n"
           "    -1:           dump memory map at ttbr1\n"
           "    -2:           dump memory map at ttbr1-0x4000\n"
           "    -s:           dump some info about IOSurfaces\n"
           "    -l addr:      do a read32\n"
           "    -p addr:      do a physical read32\n"
           "    -u:           unhook\n"
           "    -h addr:      hook for generic logging\n"
           "    -v addr:      hook vm_fault_enter for logging\n"
           "    -w addr:      hook weird for logging\n"
           "    -c addr+size: hook range for creep\n"
           "    -C:           dump creep results\n"
           "    -P:           dump protoss results\n"
           "    -U:           do something usb related\n"
           "    --ioreg path: look up IORegistryEntry\n"
           "    --crash-kernel: crash the kernel\n"
           "    -d addr:      disable kernel's use of debug registers\n"
           "                  (arg is the result of arm_debug_info())\n"
           "    -D:           re-enable kernel's use of debug registers\n"
           "    --test-protoss: test protoss (requires -d)\n"
           , argv[0]);
    return 1;
}
