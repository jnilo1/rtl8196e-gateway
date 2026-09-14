/*
 * RLX-specific cache operations for Linux 5.4
 * Ported from linux-3.10.24-realtek arch/rlx/mm/c-rlx.c
 *
 * Original: Tony Wu (tonywu@realtek.com) - Realtek Semiconductor
 * Port to 5.4: 2025
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/mmu_context.h>
#include <asm/io.h>
#include <asm/cpu.h>
#include <asm/cpu-features.h>
#include <asm/cacheflush.h>
#include <asm/mach-realtek/imem.h>

/* Entry point called from arch/mips/mm/cache.c as a weak override */
void lexra_cache_init(void);

/*
 * RLX4181 (WULING) Configuration:
 * - Has DCACHE_OP (data cache operations via CACHE instruction)
 * - NO ICACHE_OP (must use CCTL for instruction cache)
 * - Write-back data cache
 * - 16KB instruction cache, 16-byte line size
 * - 8KB data cache, 16-byte line size
 */

/*
 * CCTL OP codes for RLX cache control
 * These are written to CP0 register $20 to control caches
 */
#define CCTL_ICACHE_FLUSH		0x2	/* Invalidate I-cache */
#define CCTL_DCACHE_WBACK		0x100	/* Write-back D-cache */
#define CCTL_DCACHE_FLUSH		0x200	/* Write-back and invalidate D-cache */

/*
 * CACHE instruction opcodes for RLX
 */
#define CACHE_DCACHE_FLUSH		0x15	/* D-cache flush */
#define CACHE_DCACHE_WBACK		0x19	/* D-cache write-back */

/*
 * Execute CCTL operation on RLX4181/WULING
 * This toggles the operation bits in CP0 $20
 * Includes hazard barriers to ensure CP0 writes complete
 */
#define CCTL_OP(op)					\
	__asm__ __volatile__(				\
		".set	push\n"				\
		".set	noreorder\n"			\
		"mfc0	$8, $20\n"		/* Read CCTL */ \
		"ori	$8, %0\n"		/* Set op bits */ \
		"xori	$9, $8, %0\n"		/* Clear op bits */ \
		"mtc0	$9, $20\n"		/* Write cleared */ \
		"mtc0	$8, $20\n"		/* Write with op */ \
		"nop\n"					/* CP0 hazard */ \
		"nop\n"					/* CP0 hazard */ \
		"nop\n"					/* CP0 hazard */ \
		".set	pop\n"				\
		: : "i" (op) : "$8", "$9", "memory"	\
	)

/*
 * Single CACHE instruction
 */
#define CACHE_OP(op, p)					\
	__asm__ __volatile__ (				\
		".set	push\n"				\
		".set	noreorder\n"			\
		"cache	%0, 0x000(%1)\n"		\
		".set	pop\n"				\
		: : "i" (op), "r" (p)			\
	)

/*
 * Unrolled CACHE instruction for 16-byte cache lines
 * Processes 8 cache lines (128 bytes) at once
 */
#define CACHE16_UNROLL8(op, p)				\
	__asm__ __volatile__ (				\
		".set	push\n"				\
		".set	noreorder\n"			\
		"cache	%0, 0x000(%1)\n"		\
		"cache	%0, 0x010(%1)\n"		\
		"cache	%0, 0x020(%1)\n"		\
		"cache	%0, 0x030(%1)\n"		\
		"cache	%0, 0x040(%1)\n"		\
		"cache	%0, 0x050(%1)\n"		\
		"cache	%0, 0x060(%1)\n"		\
		"cache	%0, 0x070(%1)\n"		\
		".set	pop\n"				\
		: : "i" (op), "r" (p)			\
	)

/*
 * Flush D-cache range using CACHE instructions
 * For RLX4181 which has CONFIG_CPU_HAS_DCACHE_OP
 */
static noinline __iram void rlx_flush_dcache_fast(unsigned long start, unsigned long end)
{
	/*
	 * Line-bounded: 128-byte unrolled op only for groups wholly inside the
	 * 16-byte-rounded range, 16-byte ops for the remainder. One
	 * CACHE16_UNROLL8 per iteration regardless of length would touch 128
	 * bytes for a 20/32-byte flush and spill onto adjacent buffers — on a
	 * non-coherent platform that needlessly widens every dma_cache_* op.
	 */
	unsigned long p = start & ~(16UL - 1);
	unsigned long e = (end + 16UL - 1) & ~(16UL - 1);

	for (; p + 0x80 <= e; p += 0x80)
		CACHE16_UNROLL8(CACHE_DCACHE_FLUSH, p);
	for (; p < e; p += 16)
		CACHE_OP(CACHE_DCACHE_FLUSH, p);
}

/*
 * Flush D-cache range
 * Use CCTL for large ranges, CACHE instructions for small ranges
 */
static noinline __iram void rlx_flush_dcache_range(unsigned long start, unsigned long end)
{
	if (unlikely(start >= end))	/* zero-length / underflow guard */
		return;

	/* For large ranges, use CCTL to flush entire cache */
	if (end - start > 8192) {  /* cpu_dcache_size = 8KB */
		CCTL_OP(CCTL_DCACHE_FLUSH);
		return;
	}

	/* For small ranges, use targeted CACHE instructions */
	rlx_flush_dcache_fast(start, end);
}

/*
 * Flush I-cache range
 * RLX4181 does NOT have ICACHE_OP, so we must use CCTL to flush entire I-cache
 */
static void rlx_flush_icache_range(unsigned long start, unsigned long end)
{
	/*
	 * RLX4181/WULING has no I-cache CACHE instruction support.
	 * We must flush D-cache first (in case of self-modifying code),
	 * then flush entire I-cache using CCTL.
	 * The "memory" clobber in CCTL_OP prevents reordering.
	 */
	if (unlikely(start >= end))	/* zero-length / underflow guard */
		return;
	rlx_flush_dcache_range(start, end);
	CCTL_OP(CCTL_ICACHE_FLUSH);
}

/*
 * Flush entire cache
 */
static inline void rlx___flush_cache_all(void)
{
	CCTL_OP(CCTL_DCACHE_FLUSH);
	CCTL_OP(CCTL_ICACHE_FLUSH);
}

/*
 * Flush a single data cache page
 */
static inline void local_rlx_flush_data_cache_page(void *addr)
{
	rlx_flush_dcache_fast((unsigned long)addr,
			      (unsigned long)addr + PAGE_SIZE);
}

static void rlx_flush_data_cache_page(unsigned long addr)
{
	preempt_disable();
	local_rlx_flush_data_cache_page((void *)addr);
	preempt_enable();
}

/*
 * Flush cache for a single page
 */
static void rlx_flush_cache_page(struct vm_area_struct *vma,
				 unsigned long addr, unsigned long pfn)
{
	unsigned long kaddr = (unsigned long)__va(pfn << PAGE_SHIFT);

	preempt_disable();
	rlx_flush_dcache_fast(kaddr, kaddr + PAGE_SIZE);
	preempt_enable();
}

/*
 * Kernel vmap range flush
 */
static inline void rlx_flush_kernel_vmap_range(unsigned long vaddr, int size)
{
	rlx_flush_dcache_range(vaddr, vaddr + size);
}

/*
 * DMA coherency support
 */
#ifdef CONFIG_DMA_NONCOHERENT

static inline void rlx_wback_dcache_fast(unsigned long start, unsigned long end)
{
	/* Line-bounded, like rlx_flush_dcache_fast() — no 128-byte over-run. */
	unsigned long p = start & ~(16UL - 1);
	unsigned long e = (end + 16UL - 1) & ~(16UL - 1);

	for (; p + 0x80 <= e; p += 0x80)
		CACHE16_UNROLL8(CACHE_DCACHE_WBACK, p);
	for (; p < e; p += 16)
		CACHE_OP(CACHE_DCACHE_WBACK, p);
}

static inline void rlx_wback_dcache_range(unsigned long start, unsigned long end)
{
	if (unlikely(start >= end))	/* zero-length / underflow guard */
		return;
	if (end - start > 8192) {
		CCTL_OP(CCTL_DCACHE_WBACK);
		return;
	}
	rlx_wback_dcache_fast(start, end);
}

static __iram void rlx_dma_cache_wback_inv(unsigned long start, unsigned long size)
{
	unsigned long end = start + size;
	rlx_flush_dcache_range(start, end);
}

static __iram void rlx_dma_cache_inv(unsigned long start, unsigned long size)
{
	unsigned long end = start + size;
	rlx_flush_dcache_range(start, end);
}

#endif /* CONFIG_DMA_NONCOHERENT */

/*
 * Setup cache coherency attributes
 * CRITICAL: Must set _page_cachable_default for proper page mappings
 */
static void coherency_setup(void)
{
	extern unsigned long _page_cachable_default;

	/*
	 * RLX4181 requires non-coherent cacheable pages.
	 * For RLX4181, _CACHE_CACHABLE_NONCOHERENT = 0 (no shift needed)
	 * This sets the default cache attribute for all user pages.
	 */
	_page_cachable_default = _CACHE_CACHABLE_NONCOHERENT;

	pr_info("RLX cache: Cache coherency attribute set to 0x%lx\n",
		_page_cachable_default);
}

/*
 * Probe and setup RLX/Lexra cache
 * This is called during CPU initialization
 */
void lexra_cache_init(void)
{
	extern void build_clear_page(void);
	extern void build_copy_page(void);
	struct cpuinfo_mips *c = &current_cpu_data;

	pr_info("RLX cache: Initializing RLX4181/WULING cache operations\n");

	/* Set cache line sizes for kernel */
	c->icache.linesz = 16;  /* RLX4181: 16-byte I-cache line */
	c->dcache.linesz = 16;  /* RLX4181: 16-byte D-cache line */

	/* Assign cache operation function pointers */
	flush_cache_all = rlx___flush_cache_all;
	__flush_cache_all = rlx___flush_cache_all;
	flush_cache_mm = (void *)rlx___flush_cache_all;
	flush_cache_range = (void *)rlx___flush_cache_all;
	flush_cache_page = rlx_flush_cache_page;

	/* CRITICAL: I-cache flush function */
	flush_icache_range = rlx_flush_icache_range;
	local_flush_icache_range = rlx_flush_icache_range;

	__flush_icache_user_range = rlx_flush_icache_range;
	__local_flush_icache_user_range = rlx_flush_icache_range;

	__flush_kernel_vmap_range = rlx_flush_kernel_vmap_range;

	/* flush_cache_sigtramp removed in 5.4 */
	/* local_flush_data_cache_page removed in 6.x — only flush_data_cache_page remains */
	flush_data_cache_page = rlx_flush_data_cache_page;

#ifdef CONFIG_DMA_NONCOHERENT
	_dma_cache_wback_inv = rlx_dma_cache_wback_inv;
	_dma_cache_inv = rlx_dma_cache_inv;
	/*
	 * Writeback-only resolves to writeback+invalidate.  The dedicated
	 * primitive exists above (rlx_wback_dcache_range) but is unwired, and
	 * nothing in this tree calls dma_cache_wback(), so the alias is inert
	 * today.  A caller added later would silently lose the lines it expects
	 * to keep cached — wire this to rlx_wback_dcache_range at that point.
	 * Keeping the buffer warm is not automatically the win it looks like:
	 * a 1500-byte frame is ~19 % of this 8 KB D-cache, and a measured A/B
	 * on the TX flush path came out 1.1 % slower that way.
	 */
	_dma_cache_wback = rlx_dma_cache_wback_inv;
#endif

	build_clear_page();
	build_copy_page();

	/* Setup cache coherency - CRITICAL for proper page mappings */
	coherency_setup();

	/* Flush all caches at initialization */
	rlx___flush_cache_all();

	pr_info("RLX cache: Initialization complete\n");
}
