// SPDX-License-Identifier: GPL-2.0
/*
 * icache_probe.c - measure the RLX4181 instruction cache geometry from KSEG0.
 *
 * Why this exists
 * ---------------
 * Placement work on this SoC is done blind. The I-cache is documented as 16 KB
 * with 16-byte lines, but its ASSOCIATIVITY has no source at all: the port's
 * lexra_cache_init() sets only linesz and never probes, CP0 Config reads zero
 * on this part (Config0.M = 0, so the Config1 chain is not advertised), and the
 * public datasheet is silent. Every conflict figure computed from symbol
 * addresses assumes direct mapping; at 2- or 4-way most of those overlaps are
 * not conflicts and the whole avenue is worth much less.
 *
 * There is no PMU and no CP0 Count, so timing is the only instrument.
 *
 * Why in the kernel rather than userspace
 * ---------------------------------------
 * A userspace probe runs in mapped space, which brings two confounds it cannot
 * remove: a knee could be TLB capacity rather than cache associativity, and
 * `base + i*stride` only selects cache sets if the cache is virtually indexed
 * or the physical page colours are known -- which is one of the things we are
 * trying to determine. Running from KSEG0 removes both at once: translation is
 * a fixed unmapped mapping with the low address bits preserved
 * (phys = virt & 0x1fffffff), so no TLB is consulted and the index bits are the
 * physical ones whatever the indexing mode.
 *
 * KSEG0 gives the mapping, NOT contiguity. The arena is therefore a single
 * high-order alloc_pages() allocation, and its physical contiguity is asserted
 * rather than assumed.
 *
 * Method
 * ------
 * Generate N blocks of exactly one cache line (4 MIPS-I instructions), chained
 * by absolute jumps, at a controlled stride, ending in a tail that decrements a
 * counter and jumps back. The chain therefore iterates INTERNALLY and returns
 * to C once, so per-call overhead is not divided by N.
 *
 * Blocks separated by a multiple of the way size all map to the same set. With
 * A ways the first A fit; the (A+1)th evicts one and every later traversal
 * misses. Sweeping N at a candidate way size should show a step at N = A + 1.
 *
 * Two things stop that from being read naively:
 *
 *  - The tail consumes cache lines of its own. It is placed at a deliberate
 *    +5-line offset from the block set, so it can never share a set with the
 *    blocks for any plausible geometry.
 *  - Replacement policy and any fetch-ahead smear the step. A single perfect
 *    knee is not the read-out; a STABLE PATTERN ACROSS SEVERAL BASE OFFSETS is.
 *
 * Every point is reported as the paired difference against a matched control:
 * the same block count and the same span, but each block deliberately shifted
 * one line further so they land in consecutive distinct sets. Raw ns/block is
 * never the result -- only conflict minus control.
 *
 * Safety
 * ------
 * Nothing runs at boot. The sweep executes only on an explicit read of
 * /proc/rtl8196e_icache_probe (mode 0400, root only). Each timed batch runs
 * under local_irq_save() for a few hundred microseconds: masking interrupts
 * does not stop the clocksource -- the TC1 counter keeps running -- but it does
 * defer clockevents, so batches are kept far below the ~60 s watchdog timeout
 * and the network's tolerance.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/sort.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/irqflags.h>
#include <asm/cacheflush.h>
#include <asm/page.h>

#define LINE_BYTES	16		/* documented I-cache line */
#define MAX_BLOCKS	16
#define TAIL_LINE_OFF	5		/* tail sits 5 lines off the block set */
#define REPS		9		/* odd, so the median is a real sample */
#define ITERS		2000		/* traversals inside one timed batch */

/* MIPS-I encodings, big-endian target. nop is sll $0,$0,0. */
#define INSN_NOP	0x00000000u
#define INSN_JR_RA	0x03e00008u
/* addiu a0, a0, -1  ->  001001 00100 00100 1111111111111111 */
#define INSN_ADDIU_A0	0x2484ffffu
/* beq a0, zero, +3 -> lands on the jr ra, skipping the j and its delay slot.
 * Offset verified against the assembler, not derived by hand: an off-by-one
 * here lands in the j's delay slot, which happens to work and would have
 * hidden the error. */
#define INSN_BEQ_A0_DONE	0x10800003u

static inline u32 insn_j(unsigned long target)
{
	return 0x08000000u | ((u32)((target >> 2) & 0x03ffffffu));
}

typedef void (*chain_fn)(long iters);

/*
 * Candidate way sizes for a 16 KB cache: direct-mapped -> 16 KB, 2-way -> 8 KB,
 * 4-way -> 4 KB, 8-way -> 2 KB. A stride of 32 KB is included as a consistency
 * check: it must alias exactly like 16 KB does if the cache really is 16 KB.
 */
static const unsigned int strides[] = { 2048, 4096, 8192, 16384, 32768 };
static const unsigned int blocks[]  = { 1, 2, 3, 4, 5, 6, 8, 10, 12 };

/* Several base offsets = several cache colours. A geometry that only shows up
 * at one of them is not a geometry. */
static const unsigned int base_offsets[] = { 0, 64, 1024, 4096 + 128 };

struct probe_arena {
	struct page	*pages;
	unsigned int	order;
	void		*va;		/* KSEG0 */
	unsigned long	size;
};

/*
 * Lay out n blocks at `stride` from `base`, chained, with the tail placed
 * clear of the block set. `skew` shifts block i by skew*i bytes: 0 builds the
 * conflict chain (all blocks in one set), LINE_BYTES builds the matched
 * control (consecutive distinct sets, same block count, same span + n lines).
 */
static chain_fn build_chain(struct probe_arena *a, unsigned long base,
			    unsigned int stride, int n, unsigned int skew,
			    unsigned long *tail_out)
{
	unsigned long tail;
	int i;

	/* Tail after the last block, offset by a non-multiple of any candidate
	 * way size so it cannot collide with the blocks whatever the geometry. */
	tail = base + (unsigned long)n * stride + (unsigned long)n * skew
	     + TAIL_LINE_OFF * LINE_BYTES;

	for (i = 0; i < n; i++) {
		u32 *b = (u32 *)(base + (unsigned long)i * stride
				      + (unsigned long)i * skew);
		unsigned long next = (i == n - 1)
			? tail
			: base + (unsigned long)(i + 1) * stride
			       + (unsigned long)(i + 1) * skew;

		b[0] = INSN_NOP;
		b[1] = INSN_NOP;
		b[2] = insn_j(next);
		b[3] = INSN_NOP;	/* branch delay slot, always executed */
	}

	{
		u32 *t = (u32 *)tail;

		t[0] = INSN_ADDIU_A0;	/* addiu a0, a0, -1     */
		t[1] = INSN_BEQ_A0_DONE;/* beq   a0, zero, done */
		t[2] = INSN_NOP;	/*   delay slot         */
		t[3] = insn_j(base);	/* j     block0         */
		t[4] = INSN_NOP;	/*   delay slot         */
		t[5] = INSN_JR_RA;	/* done: jr ra          */
		t[6] = INSN_NOP;	/*   delay slot         */
	}

	/* Written as data, about to be executed: the D-cache holds the only
	 * copy and the I-cache may hold a stale one. Both must be settled. */
	flush_icache_range(base, tail + 7 * sizeof(u32));

	if (tail_out)
		*tail_out = tail;
	return (chain_fn)base;
}

static int cmp_u64(const void *x, const void *y)
{
	u64 a = *(const u64 *)x, b = *(const u64 *)y;

	return (a > b) - (a < b);
}

/*
 * One timed batch: ITERS internal traversals, interrupts masked. Returns
 * picoseconds per block traversed, so integer arithmetic keeps resolution
 * (one traversal of one block is a few ns).
 */
static u64 time_chain(chain_fn fn, int n)
{
	u64 samples[REPS];
	unsigned long flags;
	u64 t0, t1;
	int r;

	for (r = 0; r < REPS; r++) {
		fn(4);				/* warm the lines back in */
		local_irq_save(flags);
		t0 = ktime_get_ns();
		fn(ITERS);
		t1 = ktime_get_ns();
		local_irq_restore(flags);
		/* No hardware 64-bit divide on this core: use div_u64(). The
		 * divisor is ITERS * n, at most 2000 * 16, so it fits u32. */
		samples[r] = div_u64((t1 - t0) * 1000ull,
				     (u32)ITERS * (u32)n);
	}
	sort(samples, REPS, sizeof(samples[0]), cmp_u64, NULL);
	return samples[REPS / 2];
}

static struct probe_arena arena;

static int arena_alloc(struct probe_arena *a)
{
	unsigned int order;

	/* Largest footprint: max stride * max blocks, plus the control's skew
	 * and the tail. Try progressively smaller orders rather than fail. */
	for (order = 7; order >= 5; order--) {
		a->pages = alloc_pages(GFP_KERNEL | __GFP_NOWARN, order);
		if (a->pages) {
			a->order = order;
			a->size = PAGE_SIZE << order;
			a->va = page_address(a->pages);
			return 0;
		}
	}
	return -ENOMEM;
}

static void arena_free(struct probe_arena *a)
{
	if (a->pages) {
		__free_pages(a->pages, a->order);
		a->pages = NULL;
	}
}

static int icache_probe_show(struct seq_file *m, void *v)
{
	unsigned long phys_lo, phys_hi;
	int si, bi, oi;

	if (arena_alloc(&arena)) {
		seq_puts(m, "arena: allocation failed, no contiguous region\n");
		return 0;
	}

	phys_lo = virt_to_phys(arena.va);
	phys_hi = phys_lo + arena.size - 1;

	seq_printf(m, "RLX4181 I-cache probe (KSEG0)\n");
	seq_printf(m, "arena  va 0x%08lx  phys 0x%08lx-0x%08lx  %lu KB (order %u)\n",
		   (unsigned long)arena.va, phys_lo, phys_hi,
		   arena.size >> 10, arena.order);
	seq_printf(m, "batch  %d internal traversals, median of %d, IRQs masked\n",
		   ITERS, REPS);
	seq_printf(m, "line   %d bytes assumed for block size\n\n", LINE_BYTES);

	/* Contiguity is a property of the allocation, but say so explicitly:
	 * a single alloc_pages() order-N block IS physically contiguous, and
	 * KSEG0 preserves the low address bits, so the stride we program is
	 * the stride the cache indexer sees. */
	seq_puts(m, "Values are ps/block: CONFLICT chain minus MATCHED CONTROL.\n"
		    "The control has the same block count and span, each block\n"
		    "shifted one line so they occupy consecutive distinct sets.\n"
		    "A step at N blocks suggests associativity N-1. Read a\n"
		    "pattern stable across base offsets, not one perfect knee.\n\n");

	for (oi = 0; oi < ARRAY_SIZE(base_offsets); oi++) {
		seq_printf(m, "=== base offset %u ===\n", base_offsets[oi]);
		seq_printf(m, "%8s", "stride");
		for (bi = 0; bi < ARRAY_SIZE(blocks); bi++)
			seq_printf(m, "%8u", blocks[bi]);
		seq_puts(m, "   <- blocks\n");

		for (si = 0; si < ARRAY_SIZE(strides); si++) {
			unsigned int stride = strides[si];

			seq_printf(m, "%8u", stride);
			for (bi = 0; bi < ARRAY_SIZE(blocks); bi++) {
				unsigned long base = (unsigned long)arena.va
						   + base_offsets[oi];
				int n = blocks[bi];
				unsigned long need_c, need_k;
				u64 tc, tk;

				need_c = (unsigned long)n * stride
				       + (TAIL_LINE_OFF + 8) * LINE_BYTES;
				need_k = need_c + (unsigned long)n * LINE_BYTES;
				if (base_offsets[oi] + need_k > arena.size) {
					seq_printf(m, "%8s", "-");
					continue;
				}

				tc = time_chain(build_chain(&arena, base, stride,
							    n, 0, NULL), n);
				tk = time_chain(build_chain(&arena, base, stride,
							    n, LINE_BYTES, NULL), n);
				seq_printf(m, "%8lld", (long long)tc - (long long)tk);
			}
			seq_puts(m, "\n");
		}
		seq_puts(m, "\n");
	}

	arena_free(&arena);
	return 0;
}

static int icache_probe_open(struct inode *inode, struct file *file)
{
	return single_open(file, icache_probe_show, NULL);
}

static const struct proc_ops icache_probe_ops = {
	.proc_open	= icache_probe_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init icache_probe_init(void)
{
	proc_create("rtl8196e_icache_probe", 0400, NULL, &icache_probe_ops);
	pr_info("rtl8196e: I-cache probe armed at /proc/rtl8196e_icache_probe (read to run)\n");
	return 0;
}
late_initcall(icache_probe_init);
