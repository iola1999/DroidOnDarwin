#!/usr/bin/env python3
"""将固定 ARM64 eBPF 后端适配到 LKL 的宿主执行池。"""
import hashlib
from pathlib import Path

UPSTREAM_SHA256 = {
    'arch/arm64/net/bpf_jit_comp.c': '3bd90c73c35b6536cf238043e154e1aaf04ec6b9a40cdada07fb162f2e9ddb72',
    'arch/arm64/net/bpf_jit.h': 'e86eb15b1756cbd772ce88251690341f30df7edd3280029ddabd5fbe6fae7224',
    'arch/arm64/lib/insn.c': '0ba350b1da420f474fff08f4bf0c8b8d2d5b7bd59fd5649fe2712a3f0df4eaac',
    'arch/arm64/include/asm/insn.h': 'a93b6c28775a2a8a13d1ed758e529cefe77f88e6c75b5e556304ac689dafcff4',
    'arch/arm64/include/asm/insn-def.h': '1aedb653e1998954f26e930e459a5cc61b8fbfc6ff20ffc49a1819fb74f27328',
    'arch/arm64/include/asm/brk-imm.h': 'fc11ce4f381259b3c8ddc103471adcbe7ef3c9449d11f0b7bd5cdb45abc573a5',
    'arch/lkl/Kconfig': 'dd0d392a25dd3c08392d154de40381b2d94b38053b269ac5150fe937e34ffef0',
    'arch/lkl/Makefile': 'e0301a8aef5697f5c4f4e6799f49dccc9a6dd68a2d92e8846d00fc4d97c133c3',
    'arch/lkl/include/uapi/asm/host_ops.h': 'a5ee5cf1d2410c81245975570f57fb65d939f2522bacf371d3565ee47e4e326e',
    'kernel/bpf/core.c': '20c9eceae44121e354b1623d7bf37eda4a55b557333907839f9450d8acb51a8c',
    'kernel/bpf/verifier.c': 'dc7faf153f033e5ffcd3653a6f2ca74b086302c5f168d1ca713ff61978cea90c',
    'kernel/bpf/ringbuf.c': 'f21f85c324430e4941487d9228548203205e86db4099be0da32153fb4aa71c02',
    'kernel/bpf/syscall.c': 'cb98915b60c7cc9f94345da5104a130dd23905aaf578acc2f35b46403b6e4ed9',
    'include/linux/filter.h': '0c97422394f7e39084d2dd729c728673842ce967e6bdf2f3b0789f95f349d6b8',
    'include/uapi/linux/bpf.h': '7d67e542c00f833a99b4c4e465aa3bc6bc785bc490d4969e879e931907af4807',
    'net/core/sysctl_net_core.c': 'd355e505e9a01efeda5cf7d1515ebde60254545ed64c02c2bb62ae600cb217de',
    'kernel/bpf/Kconfig': '04169c0238a9f5f6e8a302139a2052be7eb661a7eaf740c749fd5e26aaa38777',
}
GENERATED_FILES = (
    'arch/lkl/net/Makefile', 'arch/lkl/net/bpf_memory.c',
    'arch/lkl/net/bpf_jit.h', 'arch/lkl/net/insn.c', 'arch/lkl/net/bpf_jit_comp.c',
    'arch/lkl/include/asm/insn.h', 'arch/lkl/include/asm/insn-def.h', 'arch/lkl/include/asm/brk-imm.h',
)


def check_sources(source: Path, names):
    for name in names:
        expected = UPSTREAM_SHA256[name]
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError('Linux BPF source mismatch: ' + name)


def apply(source: Path, experiment: Path):
    # NCE/translation has already changed the LKL inputs; the ARM64 inputs stay original.
    check_sources(source, (name for name in UPSTREAM_SHA256 if name.startswith('arch/arm64/')))

    def edit(name, before, after):
        path = source / name
        text = path.read_text()
        if text.count(after) == 1:
            return
        if text.count(before) != 1:
            raise RuntimeError('LKL BPF source mismatch: ' + name)
        path.write_text(text.replace(before, after))

    edit('arch/lkl/Kconfig', 'config LKL_FUZZING', '''config LKL_BPF_JIT
	bool "Native ARM64 BPF JIT with host execution memory"
	depends on 64BIT && MMU && BPF_SYSCALL
	select HAVE_EBPF_JIT
	select ARCH_WANT_DEFAULT_BPF_JIT
	default n

config LKL_FUZZING''')
    edit('arch/lkl/Makefile', 'core-y += arch/lkl/drivers/',
         'core-y += arch/lkl/drivers/\ncore-$(CONFIG_LKL_BPF_JIT) += arch/lkl/net/')
    edit('arch/lkl/include/uapi/asm/host_ops.h', '#define _ASM_UAPI_LKL_HOST_OPS_H',
         '#define _ASM_UAPI_LKL_HOST_OPS_H\n#define LKL_NCE_BPF_JIT 1')
    edit('arch/lkl/include/uapi/asm/host_ops.h', '\tstruct lkl_dev_pci_ops *pci_ops;', '''	struct lkl_dev_pci_ops *pci_ops;

	/* Prepared before startup. Linux synchronizes execution before reuse.
	 * The RW alias shares the RX allocation lifetime; publish flushes
	 * the data alias and instruction cache for the supplied range. */
	void *(*bpf_code_alloc)(unsigned long size);
	int (*bpf_code_free)(void *address);
	void *(*bpf_code_write_alias)(const void *address, unsigned long size);
	int (*bpf_code_publish)(const void *address, unsigned long size);
	unsigned long (*bpf_code_capacity)(void);''')

    edit('kernel/bpf/core.c', '''	void *ptr;

	ptr = bpf_arch_text_copy(ro_header, rw_header, rw_header->size);

	kvfree(rw_header);

	if (IS_ERR(ptr)) {
		bpf_prog_pack_free(ro_header, ro_header->size);
		return PTR_ERR(ptr);
	}''', '''	u32 size = rw_header->size;
	void *ptr;

	ptr = bpf_arch_text_copy(ro_header, rw_header, size);
	kvfree(rw_header);

	if (IS_ERR(ptr)) {
		bpf_prog_pack_free(ro_header, size);
		bpf_jit_uncharge_modmem(size);
		return PTR_ERR(ptr);
	}''')
    edit('kernel/bpf/core.c', ''' * bpf_jit_binary_pack_free requires proper ro_header->size. However,
 * bpf_jit_binary_pack_alloc does not set it. Therefore, ro_header->size
 * must be set with either bpf_jit_binary_pack_finalize (normal path) or
 * bpf_arch_text_copy (when jit fails).''', ''' * Before finalize, the RW header owns the allocation size. After finalize,
 * only the published RO header remains.''')
    edit('kernel/bpf/core.c', '''{
	u32 size = ro_header->size;

	bpf_prog_pack_free(ro_header, size);
	kvfree(rw_header);
	bpf_jit_uncharge_modmem(size);
}''', '''{
	u32 size = rw_header ? rw_header->size : ro_header->size;

	bpf_prog_pack_free(ro_header, size);
	kvfree(rw_header);
	bpf_jit_uncharge_modmem(size);
}''')

    net = source / 'arch/lkl/net'
    net.mkdir(exist_ok=True)
    (net / 'Makefile').write_text('obj-y += bpf_jit_comp.o insn.o bpf_memory.o\n')
    (net / 'bpf_memory.c').write_bytes((experiment / 'lkl-bpf-jit-memory.c').read_bytes())
    (net / 'bpf_jit.h').write_bytes((source / 'arch/arm64/net/bpf_jit.h').read_bytes())
    for name in ('insn.h', 'insn-def.h', 'brk-imm.h'):
        (source / 'arch/lkl/include/asm' / name).write_bytes((source / 'arch/arm64/include/asm' / name).read_bytes())

    insn = (source / 'arch/arm64/lib/insn.c').read_text()
    insn = insn.replace('#include <linux/bitops.h>', '#include <linux/bitops.h>\n#include <linux/align.h>')
    insn = insn.replace('#include <asm/debug-monitors.h>\n', '')
    insn = insn.replace('#include <asm/kprobes.h>\n', '').replace('__kprobes ', '')
    (net / 'insn.c').write_text(insn)

    jit = (source / 'arch/arm64/net/bpf_jit_comp.c').read_text()

    def replace(before, after):
        nonlocal jit
        if jit.count(before) != 1:
            raise RuntimeError('ARM64 BPF section mismatch: ' + before[:80])
        jit = jit.replace(before, after)

    def replace_between(first, last, after):
        nonlocal jit
        if jit.count(first) != 1 or jit.count(last) != 1:
            raise RuntimeError('ARM64 BPF section mismatch: ' + first[:80])
        start, end = jit.index(first), jit.index(last)
        jit = jit[:start] + after + jit[end:]

    for include in ('asm-extable.h', 'cacheflush.h', 'debug-monitors.h', 'patching.h', 'set_memory.h'):
        replace('#include <asm/' + include + '>\n', '')
    replace('#include <asm/insn.h>', '#include <asm/insn.h>\n#include <asm/host_ops.h>')
    replace_between('/*\n * Kernel addresses in the vmalloc space use at most 48 bits,',
                    'static bool should_emit_indirect_call', '''/* Host helper and pool addresses use all four 16-bit address components.
 * Keep the instruction count fixed while subprogram targets are resolved. */
static inline void emit_addr_mov_i64(const int reg, const u64 val,
				   struct jit_ctx *ctx)
{
	emit(A64_MOVZ(1, reg, val & 0xffff, 0), ctx);
	for (int shift = 16; shift < 64; shift += 16)
		emit(A64_MOVK(1, reg, (val >> shift) & 0xffff, shift), ctx);
}

''')
    replace('''	__le32 *ptr;
	/* We are guaranteed to have aligned memory. */
	for (ptr = area; size >= sizeof(u32); size -= sizeof(u32))
		*ptr++ = cpu_to_le32(AARCH64_BREAK_FAULT);''', '''	__le32 *ptr;
	void *alias = lkl_ops->bpf_code_write_alias(area, size);
	/* The pack allocator also supplies an ordinary RW compilation buffer. */
	for (ptr = alias ? alias : area; size >= sizeof(u32); size -= sizeof(u32))
		*ptr++ = cpu_to_le32(AARCH64_BREAK_FAULT);''')
    replace_between('int bpf_arch_text_invalidate(', 'static inline int epilogue_offset', '')
    replace_between('#define BPF_FIXUP_OFFSET_MASK', '/* JITs an eBPF instruction.', '''/* Host faults do not use ARM64 kernel exception tables. */
static int add_exception_handler(const struct bpf_insn *insn,
				struct jit_ctx *ctx, int dst_reg)
{
	if (BPF_MODE(insn->code) == BPF_PROBE_MEM ||
	    BPF_MODE(insn->code) == BPF_PROBE_MEMSX ||
	    BPF_MODE(insn->code) == BPF_PROBE_MEM32 ||
	    BPF_MODE(insn->code) == BPF_PROBE_ATOMIC)
		return -EOPNOTSUPP;
	return 0;
}

''')
    replace_between('''		} else if (insn_is_mov_percpu_addr(insn)) {''',
                    '''		switch (insn->off) {''', '''		} else if (insn_is_mov_percpu_addr(insn)) {
			return -EOPNOTSUPP;
		}
''')
    replace_between('''		u32 cpu_offset;
''', '''		ret = bpf_jit_get_func_addr''', '')
    replace('if (cpus_have_cap(ARM64_HAS_LSE_ATOMICS))', 'if (false)')
    replace_between('static inline void bpf_flush_icache(', 'struct arm64_jit_data {', '')
    replace('''		/*
		 * The instructions have now been copied to the ROX region from
		 * where they will execute. Now the data cache has to be cleaned to
		 * the PoU and the I-cache has to be invalidated for the VAs.
		 */
		bpf_flush_icache(ro_header, ctx.ro_image + ctx.idx);''',
            '\t\t/* bpf_arch_text_copy publishes the RX alias before it returns. */')
    replace('''	if (!prog->jit_requested)
		return orig_prog;''', '''	if (!prog->jit_requested || prog->aux->num_exentries ||
	    prog->aux->exception_boundary || prog->aux->exception_cb)
		return orig_prog;''')
    replace('''	build_prologue(&ctx, was_classic);

	/* Record exentry_idx''', '''	if (build_prologue(&ctx, was_classic)) {
		prog = orig_prog;
		goto out_free_hdr;
	}

	/* Record exentry_idx''')
    replace('''			prog->bpf_func = NULL;
			prog->jited = 0;
			prog->jited_len = 0;
			goto out_free_hdr;''', '\t\t\tgoto out_free_hdr;')
    replace('''			/* ro_header has been freed */
			ro_header = NULL;
			prog = orig_prog;
			goto out_off;''', '''			/* Both headers and their charge have been released. */
			prog = orig_prog;
			goto out_clear_jit;''')
    replace('''out_free_hdr:
	if (header) {
		bpf_arch_text_copy(&ro_header->size, &header->size,
				   sizeof(header->size));
		bpf_jit_binary_pack_free(ro_header, header);
	}
	goto out_off;''', '''out_free_hdr:
	if (header)
		bpf_jit_binary_pack_free(ro_header, header);
out_clear_jit:
	/* The first attempt keeps the interpreter selected by the BPF core. */
	if (extra_pass) {
		prog->bpf_func = NULL;
		prog->jited = 0;
		prog->jited_len = 0;
	}
	goto out_off;''')
    # Keep the generic false feature predicates for ARM64 kernel tracing,
    # kfuncs, arenas, percpu instruction rewriting and exception unwinding.
    replace_between('bool bpf_jit_supports_kfunc_call(void)', 'void bpf_jit_free(struct bpf_prog *prog)', '''bool bpf_jit_supports_subprog_tailcalls(void)
{
	return true;
}

''')
    replace_between('''		struct bpf_binary_header *hdr;
''', '''		WARN_ON_ONCE(!bpf_prog_kallsyms_verify_off(prog));''', '''		struct bpf_binary_header *hdr;
		struct bpf_binary_header *rw_header = NULL;

		/* A later subprogram can fail before this one is finalized. */
		if (jit_data) {
			hdr = jit_data->ro_header;
			rw_header = jit_data->header;
			kvfree(jit_data->ctx.offset);
			kfree(jit_data);
			prog->aux->jit_data = NULL;
		} else {
			hdr = bpf_jit_binary_pack_hdr(prog);
		}
		bpf_jit_binary_pack_free(hdr, rw_header);
''')
    if any(name in jit for name in ('cpus_have_cap', 'A64_MRS_SP_EL0', '#include <asm/patching.h>')):
        raise RuntimeError('ARM64 kernel-only dependency remains in LKL BPF JIT')
    (net / 'bpf_jit_comp.c').write_text(jit)
