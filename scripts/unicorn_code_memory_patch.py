"""为 ARM64 Unicorn 变体分离代码执行地址与写入地址。"""


def apply(patched, pristine):
    def change(name, before, after, count=1):
        source = patched.get(name)
        if source is None:
            source = pristine(name)
        if source.count(before) != count:
            raise RuntimeError('Unicorn dual mapping source mismatch: ' + name + ': ' + before)
        patched[name] = source.replace(before, after)

    change('qemu/include/qemu/osdep.h', '#include "config-host.h"', '''#include "config-host.h"
#ifdef NCE_TCG_DUAL_MAP
/* RX and RW aliases do not use per-thread MAP_JIT permissions. */
#undef HAVE_PTHREAD_JIT_PROTECT
#endif''')
    change('qemu/include/tcg/tcg.h', '#define TCG_H', '''#define TCG_H

#ifdef NCE_TCG_DUAL_MAP
void *nce_uc_code_alloc(size_t size);
void nce_uc_code_free(void *address, size_t size);
void *nce_uc_code_write(const void *address);
void *nce_uc_metadata_alloc(const void *address, size_t size);
void nce_uc_metadata_reset(const void *address);
#define NCE_CODE_WRITE(p) ((__typeof__(p))nce_uc_code_write(p))
#else
#define NCE_CODE_WRITE(p) (p)
#endif''')
    name = 'qemu/tcg/tcg.c'
    change(name, '*s->code_ptr++ = v;', '*NCE_CODE_WRITE(s->code_ptr++) = v;', 4)
    change(name, '*p = v;', '*NCE_CODE_WRITE(p) = v;', 4)
    change(name, 'memcpy(p, &v, sizeof(v));', 'memcpy(NCE_CODE_WRITE(p), &v, sizeof(v));', 6)
    change(name, '    return tb;\n}\n\nvoid tcg_prologue_init', '''#ifdef NCE_TCG_DUAL_MAP
    return nce_uc_metadata_alloc(tb, sizeof(*tb));
#else
    return tb;
#endif
}

void tcg_prologue_init''')
    change(name, '    tcg_region_tree_reset_all(tcg_ctx);', '''    tcg_region_tree_reset_all(tcg_ctx);
#ifdef NCE_TCG_DUAL_MAP
    nce_uc_metadata_reset(tcg_ctx->code_gen_buffer);
#endif''')
    change('qemu/include/exec/exec-all.h', '    struct tb_tc tc;', '''    struct tb_tc tc;
#ifdef NCE_TCG_DUAL_MAP
    uint8_t *nce_search;
#endif''')
    name = 'qemu/tcg/aarch64/tcg-target.inc.c'
    for field, width in ((0, 26), (5, 19)):
        change(name, f'*code_ptr = deposit32(*code_ptr, {field}, {width}, offset);',
               f'*NCE_CODE_WRITE(code_ptr) = deposit32(*code_ptr, {field}, {width}, offset);')
    change(name, 'atomic_set((uint64_t *)jmp_addr, pair);',
           'atomic_set(NCE_CODE_WRITE((uint64_t *)jmp_addr), pair);')
    change(name, '        p[i] = NOP;', '        NCE_CODE_WRITE(p)[i] = NOP;')
    change('qemu/tcg/tcg-pool.inc.c', 'memcpy(a, p->data, size);',
           'memcpy(NCE_CODE_WRITE(a), p->data, size);')
    name = 'qemu/accel/tcg/translate-all.c'
    change(name, '    uint8_t *p = (uint8_t *)tb->tc.ptr + tb->tc.size;',
           '''#ifdef NCE_TCG_DUAL_MAP
    uint8_t *p = tb->nce_search;
#else
    uint8_t *p = (uint8_t *)tb->tc.ptr + tb->tc.size;
#endif''')
    change(name, '    uint8_t *highwater = tcg_ctx->code_gen_highwater;\n    uint8_t *p = block;',
           '''    uint8_t *highwater = NCE_CODE_WRITE((uint8_t *)tcg_ctx->code_gen_highwater);
    block = NCE_CODE_WRITE(block);
    uint8_t *p = block;''')
    change(name, '    return p - block;\n}\n\n/* The cpu state', '''#ifdef NCE_TCG_DUAL_MAP
    tb->nce_search = nce_uc_metadata_alloc(tb->tc.ptr, p - block);
    memcpy(tb->nce_search, block, p - block);
#endif
    return p - block;
}

/* The cpu state''')
    change(name, '''        if (munmap(tcg_ctx->initial_buffer, tcg_ctx->initial_buffer_size)) {
            perror("fail code_gen_buffer");
        }''', '''#ifdef NCE_TCG_DUAL_MAP
        nce_uc_code_free(tcg_ctx->initial_buffer, tcg_ctx->initial_buffer_size);
#else
        if (munmap(tcg_ctx->initial_buffer, tcg_ctx->initial_buffer_size)) {
            perror("fail code_gen_buffer");
        }
#endif''')
    change(name, '''    buf = mmap(NULL, size, prot, flags, -1, 0);
    if (buf == MAP_FAILED) {
        return NULL;
    }''', '''#ifdef NCE_TCG_DUAL_MAP
    buf = nce_uc_code_alloc(size);
    if (!buf) {
        return NULL;
    }
#else
    buf = mmap(NULL, size, prot, flags, -1, 0);
    if (buf == MAP_FAILED) {
        return NULL;
    }
#endif''')
