#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <mach/mach.h>
#include <data/mach-o/binary.h>
#include <data/find.h>
#include <data/cc.h>
#include <data/running_kernel.h>
#include <data/mach-o/headers/loader.h>
#include <data/mach-o/link.h>
#include <data/mach-o/inject.h>
#include <ctype.h>
#include <data/ios-classify.h>

static struct binary kern;

static addr_t find_hack_func(const struct binary *binary) {
    return b_sym(binary, "_IOFindBSDRoot", MUST_FIND | TO_EXECUTE);
}

static addr_t lookup_sym(void *binary, const char *sym);

static void insert_loader_stuff(struct binary *binary, const struct binary *kern) {
    unsigned int class = kern->reserved[0];
    addr_t patch_loc = b_read32(kern, b_sym(kern, "_kernel_pmap", MUST_FIND))
                            + spec(_50, 0x428,
                                   _43, 0x424,
                                   _armv6, 0x420);
    addr_t sysent = lookup_sym((void *) kern, "_sysent");
    
    CMD_ITERATE(binary->mach->hdr, cmd) {
        if(cmd->cmd == LC_ID_DYLIB) {
            struct dylib_command *d = (void *) cmd;
            d->dylib.timestamp = 0xdeadbeef;
            d->dylib.current_version = patch_loc;
            d->dylib.compatibility_version = sysent;
            return;
        }
    }

    die("dylib does not have a LC_ID_DYLIB to stick stuff into");
}
            
// apply the patch, and return the value of sysent
static addr_t apply_loader_stuff(const struct binary *binary) {
    CMD_ITERATE(binary->mach->hdr, cmd) {
        if(cmd->cmd == LC_ID_DYLIB) {
            const struct dylib_command *d = (void *) cmd;
            if(d->dylib.timestamp != 0xdeadbeef) {
                die("dylib does not have loader stuff stuck into it");
            }

            vm_address_t patch_loc = d->dylib.current_version;
            printf("patching %x\n", (int) patch_loc);
            uint32_t zero = 0;
            assert(!vm_write(get_kernel_task(), patch_loc, (vm_offset_t) &zero, sizeof(zero)));

            return d->dylib.compatibility_version;
        }
    }

    die("dylib does not have loader stuff stuck into it");
}

static addr_t find_data_munged(range_t range, const char *to_find, int align, int options) {
    autofree char *buf = strdup(to_find);
    for(char *p = buf; *p; p++) {
        s:
        switch(*p) {
            case '_': *p = ' '; break;
            case 'X': *p = '.'; break;
            case 'A': *p = '-'; break;
            case 'C': *p = '-'; align = 1; break;
            case 'T': *p = '+'; break;
            case 'M': align = 1; strcpy(p, p + 2); goto s;
        }
    }
    return find_data(range, buf, align, options);
}

// gigantic hack
static addr_t lookup_sym(void *_binary, const char *sym) {
    // special cases - really should be done in some kind of generic way
    struct binary *binary = _binary;
    if(!strcmp(sym, "_sysent")) {
        unsigned int class = binary->reserved[0];
        if(class >= _50) {
            return resolve_ldr(binary, find_data(b_macho_segrange(binary, "__TEXT"), "75 30 ff e6 - .. .. .. .. .. .. .. .. 00 00 90 e5", 0, MUST_FIND));
        } else {
            return find_int32(b_macho_segrange(binary, "__DATA"), 0x861000, MUST_FIND) + 4;
        }
    }

    if(!strcmp(sym, "_vfs_op_descs")) {
        return find_int32(b_macho_segrange(binary, "__DATA"), b_sym(binary, "_vnop_default_desc", MUST_FIND), MUST_FIND);
    }

    if(!strncmp(sym, "$strref_", 8)) {
        // '_'.join(re.findall('(..)', 'foobar'.encode('hex')))
        range_t range = b_macho_segrange(binary, "__TEXT");
        addr_t result = find_data_munged(range, sym + 8, 1, MUST_FIND);
        result = find_int32(range, result, MUST_FIND);
        result = find_bof(range, result, 2);
        return result;
    }

    // $_A_XX_XX_f0 -> find "- .. .. f0" in TEXT 
    if(!strncmp(sym, "$_", 2)) {
        return find_data_munged(b_macho_segrange(binary, "__TEXT"), sym + 2, 0, MUST_FIND);
    }

    // $in___TEXT__whatever
    if(!strncmp(sym, "$in___", 4)) {
        const char *sep = strstr(sym + 6, "__");
        if(!sep) {
            fprintf(stderr, "missing ending __\n");
            return 0;
        }
        autofree char *seg = strdup(sym + 4);
        seg[sep - (sym + 4)] = 0;
        return find_data_munged(b_macho_segrange(binary, seg), sep + 2, 0, MUST_FIND);
    }

    if(!strncmp(sym, "$ldr_", 5)) {
        return resolve_ldr(binary, lookup_sym(binary, sym + 5));
    }

    if(!strncmp(sym, "$bl", 3) && sym[4] == '_') {
        uint32_t func = lookup_sym(binary, sym + 5);
        if(!func) return 0;
        range_t range = (range_t) {binary, func, 0x1000};
        int number = sym[3] - '0';
        uint32_t bl = 0;
        while(number--) bl = find_bl(&range);
        return bl;
    }

    // $vt_<name> -> find offset to me from the corresponding vtable 
    // ex: __ZN11OSMetaClass20getMetaClassWithNameEPK8OSSymbol
    if(!strncmp(sym, "$vt_", 4)) {
        sym += 4;
        uint32_t the_func = lookup_sym(binary, sym);
        if(!the_func) return 0;

        // find the class, and construct its vtable name
        while(*sym && !isnumber(*sym)) sym++;
        char *class;
        unsigned int len = (unsigned int) strtol(sym, &class, 10) + (class - sym);
        assert(len > 0 && len <= strlen(sym));
        char *vt_name = malloc(len + 6);
        memcpy(vt_name, "__ZTV", 5);
        memcpy(vt_name + 5, sym, len);
        vt_name[len + 5] = 0;
        
        uint32_t vtable = b_sym(binary, vt_name, TO_EXECUTE);
        if(!vtable) return 0;
        uint32_t loc_in_vtable = find_int32((range_t) {binary, vtable, 0x1000}, the_func, true);

        uint32_t diff = loc_in_vtable - (vtable + 8);

        //fprintf(stderr, "b_lookup_sym: vtable index %d for %s = %x - %x\n", diff/4, sym, loc_in_vtable, vtable + 8);
        return diff;
    }

    return b_sym(binary, sym, TO_EXECUTE);
}

int main(int argc, char **argv) {
    b_init(&kern);
    (void) argc;
    argv++;
    bool ok = false;
    while(*argv) {
        char *arg = *argv++;
        if(arg[0] != '-' || arg[1] == '\0' || arg[2] != '\0') goto usage;
        switch(arg[1]) {
#ifndef MINIMAL
        case 'k': {
            char *kern_fn;
            if(!(kern_fn = *argv++)) goto usage;
            b_load_macho(&kern, kern_fn);
            kern.reserved[0] = classify(&kern);
            break;
        }
#ifdef IMG3_SUPPORT
        case 'i': {
            const char *kern_fn = *argv++;
            const char *key = *argv++;
            const char *iv = *argv++;
            prange_t decompressed = unpack(load_file(kern_fn, false, NULL), key, iv);
            b_prange_load_macho(&kern, decompressed, 0, kern_fn);
            break;
        }
#endif
        case 'p': {
        case 'P':
            ok = true;
            if(!kern.valid) goto usage;
            if(!*argv) goto usage;
            char *to_load_fn, *output_fn;
            if(!(to_load_fn = *argv++)) goto usage;
            if(!(output_fn = *argv++)) goto usage;

            struct binary to_load;
            b_init(&to_load);
            b_load_macho(&to_load, to_load_fn);
            if(to_load.mach->hdr->flags & MH_PREBOUND) die("prebound");
            if(arg[1] == 'P') {
                b_relocate(&to_load, &kern, RELOC_DEFAULT, lookup_sym, &kern, 0xf0000000);
            } else {
                b_relocate(&to_load, &kern, RELOC_EXTERN_ONLY, lookup_sym, &kern, 0);
            }
            insert_loader_stuff(&to_load, &kern);
            b_macho_store(&to_load, output_fn);

            break;
        }
        case 'q': {
            ok = true;
            if(!kern.valid) goto usage;
            char *out_kern = *argv++;
            if(!out_kern) goto usage;

            if(!*argv) goto usage;
            char *to_load_fn;
            while(to_load_fn = *argv++) {
                struct binary to_load;
                b_init(&to_load);
                b_load_macho(&to_load, to_load_fn);
                if(to_load.mach->hdr->flags & MH_PREBOUND) die("prebound");
                b_relocate(&to_load, &kern, RELOC_DEFAULT, lookup_sym, &kern, b_allocate_vmaddr(&kern));
                b_inject_macho_binary(&to_load, &kern, find_hack_func, false);
            }
            
            b_macho_store(&kern, out_kern);

            return 0;
        }
#endif // MINIMAL
#ifdef __APPLE__
        case 'l': {
            ok = true;
            char *to_load_fn;
            if(!(to_load_fn = *argv++)) goto usage;
            struct binary to_load;
            b_init(&to_load);
            b_load_macho(&to_load, to_load_fn);
            if(to_load.mach->hdr->flags & MH_PREBOUND) die("old-style prelink no longer supported");
            if(kern.valid) insert_loader_stuff(&to_load, &kern);
            addr_t sysent = apply_loader_stuff(&to_load);
            uint32_t slide = b_allocate_from_running_kernel(&to_load);
            struct binary *b = kern.valid ? &kern : NULL;
            b_relocate(&to_load, b, RELOC_DEFAULT, lookup_sym, b, slide);
            b_inject_into_running_kernel(&to_load, sysent);
            break;
        }
        case 'u': {
            ok = true;
            char *baseaddr_hex;
            if(!(baseaddr_hex = *argv++)) goto usage;
            unload_from_running_kernel(parse_hex_uint32(baseaddr_hex));
            break;
        }
#endif
        }
    }

    if(ok) return 0;

    usage:
    printf("Usage: loader "
#ifdef MINIMAL
                           "-l kcode.dylib                        load\n"
#else
#ifdef IMG3_SUPPORT
        "[-k kern | -i img3 key iv]\n"
           "                      "
#else
        "-k kern "
#endif
#ifdef __APPLE__
                                 "-l kcode.dylib                load\n"
           "                      "
#endif
                                 "-p kcode.dylib out.dylib      prelink\n"
           "                      -P kcode.dylib out.dylib      prelink & relocate (testing)\n"
           "                      -q out_kern kcode.dylib       insert into kc\n"
#endif // MINIMAL
#ifdef __APPLE__
           "              -u f0000000                           unload\n"
#endif
           );
    return 1;
}

