/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * (C) Copyright 1995 1996 Linus Torvalds
 * (C) Copyright 2001, 2002 Ralf Baechle
 */
#include <linux/module.h>
#include <asm/addrspace.h>
#include <asm/byteorder.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/idr.h>
#include <linux/hardirq.h>
#include <asm/cacheflush.h>
#include <asm/io.h>
#include <asm/tlbflush.h>

#define ATOMIC_AREA_SIZE	512

static DEFINE_IDA(atomic_mapping);
struct vm_struct *atomic_area;

static inline void remap_area_pte(pte_t * pte, unsigned long address,
	phys_t size, phys_t phys_addr, unsigned long flags)
{
	phys_t end;
	unsigned long pfn;
	pgprot_t pgprot = __pgprot(_PAGE_GLOBAL | _PAGE_PRESENT | __READABLE
	                           | __WRITEABLE | flags);

	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	BUG_ON(address >= end);
	pfn = phys_addr >> PAGE_SHIFT;
	do {
		if (!pte_none(*pte)) {
			printk("remap_area_pte: page already exists\n");
			BUG();
		}
		set_pte(pte, pfn_pte(pfn, pgprot));
		address += PAGE_SIZE;
		pfn++;
		pte++;
	} while (address && (address < end));
}

static inline int remap_area_pmd(pmd_t * pmd, unsigned long address,
	phys_t size, phys_t phys_addr, unsigned long flags)
{
	phys_t end;

	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	phys_addr -= address;
	BUG_ON(address >= end);
	do {
		pte_t * pte = pte_alloc_kernel(pmd, address);
		if (!pte)
			return -ENOMEM;
		remap_area_pte(pte, address, end - address, address + phys_addr, flags);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address && (address < end));
	return 0;
}

static int remap_area_pages(unsigned long address, phys_t phys_addr,
	phys_t size, unsigned long flags)
{
	int error;
	pgd_t * dir;
	unsigned long end = address + size;

	phys_addr -= address;
	dir = pgd_offset(&init_mm, address);
	flush_cache_all();
	BUG_ON(address >= end);
	do {
		pud_t *pud;
		pmd_t *pmd;

		error = -ENOMEM;
		pud = pud_alloc(&init_mm, dir, address);
		if (!pud)
			break;
		pmd = pmd_alloc(&init_mm, pud, address);
		if (!pmd)
			break;
		if (remap_area_pmd(pmd, address, end - address,
					 phys_addr + address, flags))
			break;
		error = 0;
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	} while (address && (address < end));
	flush_tlb_all();
	return error;
}

/*
 * Generic mapping function (not visible outside):
 */

/*
 * Remap an arbitrary physical address space into the kernel virtual
 * address space. Needed when the kernel wants to access high addresses
 * directly.
 *
 * NOTE! We need to allow non-page-aligned mappings too: we will obviously
 * have to convert them into an offset in a page-aligned mapping, but the
 * caller shouldn't need to know that small detail.
 */

#define IS_LOW512(addr) (!((phys_t)(addr) & (phys_t) ~0x1fffffffULL))

void __iomem * __ioremap(phys_t phys_addr, phys_t size, unsigned long flags)
{
	struct vm_struct * area;
	unsigned long offset;
	phys_t last_addr;
	void * addr;

	phys_addr = fixup_bigphys_addr(phys_addr, size);

	/* Don't allow wraparound or zero size */
	last_addr = phys_addr + size - 1;
	if (!size || last_addr < phys_addr)
		return NULL;

#ifndef CONFIG_MAPPED_KERNEL
	/*
	 * Map uncached objects in the low 512mb of address space using KSEG1,
	 * otherwise map using page tables.
	 */
	if (IS_LOW512(phys_addr) && IS_LOW512(last_addr) &&
	    flags == _CACHE_UNCACHED)
		return (void __iomem *) CKSEG1ADDR(phys_addr);

	/*
	 * Don't allow anybody to remap normal RAM that we're using..
	 */
	if (phys_addr < virt_to_phys(high_memory)) {
		char *t_addr, *t_end;
		struct page *page;

		t_addr = __va(phys_addr);
		t_end = t_addr + (size - 1);

		for(page = virt_to_page(t_addr); page <= virt_to_page(t_end); page++)
			if(!PageReserved(page))
				return NULL;
	}
#endif

	/*
	 * Mappings have to be page-aligned
	 */
	offset = phys_addr & ~PAGE_MASK;
	phys_addr &= PAGE_MASK;
	size = PAGE_ALIGN(last_addr + 1) - phys_addr;

	if (in_interrupt()) {
		int pg = -1;

		if (size <= PAGE_SIZE)
			pg = ida_simple_get(&atomic_mapping, 0, ATOMIC_AREA_SIZE,
					    GFP_ATOMIC);
		if (pg < 0)
			return (void __iomem *) (offset + CAC_ADDR(phys_addr));

		addr = atomic_area->addr + (pg << PAGE_SHIFT);
	} else {
	/*
	 * Ok, go for it..
	 */
	area = get_vm_area(size, VM_IOREMAP);
	if (!area)
		return NULL;
	addr = area->addr;
	}
	if (remap_area_pages((unsigned long) addr, phys_addr, size, flags)) {
		vunmap(addr);
		return NULL;
	}

	return (void __iomem *) (offset + (char *)addr);
}

#define IS_KSEG1(addr) (((unsigned long)(addr) & ~0x1fffffffUL) == CKSEG1)

void __iounmap(const volatile void __iomem *addr)
{
	struct vm_struct *p;

	if (IS_KSEG1(addr))
		return;

	if (atomic_area && addr >= atomic_area->addr &&
	    addr < atomic_area->addr + atomic_area->size) {
		extern void vunmap_page_range(unsigned long addr, unsigned long end);

		unsigned long vaddr = (unsigned long) addr & PAGE_MASK;
		unsigned long off = addr - atomic_area->addr;

		ida_simple_remove(&atomic_mapping, off >> PAGE_SHIFT);
		vunmap_page_range(vaddr, vaddr + PAGE_SIZE);
		return;
	}

	p = remove_vm_area((void *) (PAGE_MASK & (unsigned long __force) addr));
	if (!p)
		printk(KERN_ERR "iounmap: bad address %p\n", addr);

        kfree(p);
}

EXPORT_SYMBOL(__ioremap);
EXPORT_SYMBOL(__iounmap);

static int __init init_ioremap(void)
{
	atomic_area = get_vm_area(ATOMIC_AREA_SIZE * PAGE_SIZE, VM_IOREMAP);

	return 0;
}
fs_initcall(init_ioremap);
