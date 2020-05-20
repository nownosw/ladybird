/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <AK/Assertions.h>
#include <AK/Memory.h>
#include <AK/StringView.h>
#include <Kernel/Arch/i386/CPU.h>
#include <Kernel/CMOS.h>
#include <Kernel/FileSystem/Inode.h>
#include <Kernel/Multiboot.h>
#include <Kernel/Process.h>
#include <Kernel/VM/AnonymousVMObject.h>
#include <Kernel/VM/ContiguousVMObject.h>
#include <Kernel/VM/MemoryManager.h>
#include <Kernel/VM/PageDirectory.h>
#include <Kernel/VM/PhysicalRegion.h>
#include <Kernel/VM/PurgeableVMObject.h>
#include <Kernel/VM/SharedInodeVMObject.h>
#include <Kernel/StdLib.h>

//#define MM_DEBUG
//#define PAGE_FAULT_DEBUG

extern FlatPtr start_of_kernel_text;
extern FlatPtr start_of_kernel_data;
extern FlatPtr end_of_kernel_bss;

namespace Kernel {

static MemoryManager* s_the;

MemoryManager& MM
{
    return *s_the;
}

MemoryManager::MemoryManager()
{
    m_kernel_page_directory = PageDirectory::create_kernel_page_directory();
    parse_memory_map();
    write_cr3(kernel_page_directory().cr3());
    setup_low_identity_mapping();
    setup_low_pseudo_identity_mapping();
    protect_kernel_image();

    m_shared_zero_page = allocate_user_physical_page();
}

MemoryManager::~MemoryManager()
{
}

void MemoryManager::setup_low_pseudo_identity_mapping()
{
    // This code switches the pseudo-identity mapping (8 first MB above 3G mark) from 2MB pages to 4KB pages.
    // The boot code sets it up as 2MB huge pages for convenience. But we need 4KB pages to be able to protect
    // the kernel soon!

    for (size_t i = 0; i < 4; ++i) {
        m_low_pseudo_identity_mapping_pages[i] = allocate_supervisor_physical_page();
        FlatPtr base = i * (2 * MB);
        auto* page_table = (PageTableEntry*)quickmap_page(*m_low_pseudo_identity_mapping_pages[i]);
        for (size_t j = 0; j < 512; ++j) {
            auto& pte = page_table[j];
            pte.set_physical_page_base(base + j * PAGE_SIZE);
            pte.set_writable(true);
            pte.set_present(true);
            pte.set_execute_disabled(false);
            pte.set_user_allowed(false);
        }
        unquickmap_page();
    }

    auto* pd = quickmap_pd(*m_kernel_page_directory, 3);
    for (size_t i = 0; i < 4; ++i) {
        pd[i].set_huge(false);
        pd[i].set_page_table_base(m_low_pseudo_identity_mapping_pages[i]->paddr().get());
    }

    flush_entire_tlb();
}

void MemoryManager::protect_kernel_image()
{
    // Disable writing to the kernel text and rodata segments.
    for (size_t i = (FlatPtr)&start_of_kernel_text; i < (FlatPtr)&start_of_kernel_data; i += PAGE_SIZE) {
        auto& pte = ensure_pte(kernel_page_directory(), VirtualAddress(i));
        pte.set_writable(false);
    }

    if (g_cpu_supports_nx) {
        // Disable execution of the kernel data and bss segments.
        for (size_t i = (FlatPtr)&start_of_kernel_data; i < (FlatPtr)&end_of_kernel_bss; i += PAGE_SIZE) {
            auto& pte = ensure_pte(kernel_page_directory(), VirtualAddress(i));
            pte.set_execute_disabled(true);
        }
    }
}

void MemoryManager::setup_low_identity_mapping()
{
    m_low_page_table = allocate_user_physical_page(ShouldZeroFill::Yes);

    auto* pd_zero = quickmap_pd(kernel_page_directory(), 0);
    pd_zero[1].set_present(false);
    pd_zero[2].set_present(false);
    pd_zero[3].set_present(false);

    auto& pde_zero = pd_zero[0];
    pde_zero.set_page_table_base(m_low_page_table->paddr().get());
    pde_zero.set_present(true);
    pde_zero.set_huge(false);
    pde_zero.set_writable(true);
    pde_zero.set_user_allowed(false);
    if (g_cpu_supports_nx)
        pde_zero.set_execute_disabled(true);

    for (FlatPtr offset = (1 * MB); offset < (2 * MB); offset += PAGE_SIZE) {
        auto& page_table_page = m_low_page_table;
        auto& pte = quickmap_pt(page_table_page->paddr())[offset / PAGE_SIZE];
        pte.set_physical_page_base(offset);
        pte.set_user_allowed(false);
        pte.set_present(offset != 0);
        pte.set_writable(offset < (1 * MB));
    }
}

void MemoryManager::parse_memory_map()
{
    RefPtr<PhysicalRegion> region;
    bool region_is_super = false;

    auto* mmap = (multiboot_memory_map_t*)(low_physical_to_virtual(multiboot_info_ptr->mmap_addr));
    for (; (unsigned long)mmap < (low_physical_to_virtual(multiboot_info_ptr->mmap_addr)) + (multiboot_info_ptr->mmap_length); mmap = (multiboot_memory_map_t*)((unsigned long)mmap + mmap->size + sizeof(mmap->size))) {
        klog() << "MM: Multiboot mmap: base_addr = " << String::format("0x%08x", mmap->addr) << ", length = " << String::format("0x%08x", mmap->len) << ", type = 0x" << String::format("%x", mmap->type);
        if (mmap->type != MULTIBOOT_MEMORY_AVAILABLE)
            continue;

        // FIXME: Maybe make use of stuff below the 1MB mark?
        if (mmap->addr < (1 * MB))
            continue;

        if ((mmap->addr + mmap->len) > 0xffffffff)
            continue;

        auto diff = (FlatPtr)mmap->addr % PAGE_SIZE;
        if (diff != 0) {
            klog() << "MM: got an unaligned region base from the bootloader; correcting " << String::format("%p", mmap->addr) << " by " << diff << " bytes";
            diff = PAGE_SIZE - diff;
            mmap->addr += diff;
            mmap->len -= diff;
        }
        if ((mmap->len % PAGE_SIZE) != 0) {
            klog() << "MM: got an unaligned region length from the bootloader; correcting " << mmap->len << " by " << (mmap->len % PAGE_SIZE) << " bytes";
            mmap->len -= mmap->len % PAGE_SIZE;
        }
        if (mmap->len < PAGE_SIZE) {
            klog() << "MM: memory region from bootloader is too small; we want >= " << PAGE_SIZE << " bytes, but got " << mmap->len << " bytes";
            continue;
        }

#ifdef MM_DEBUG
        klog() << "MM: considering memory at " << String::format("%p", (FlatPtr)mmap->addr) << " - " << String::format("%p", (FlatPtr)(mmap->addr + mmap->len));
#endif

        for (size_t page_base = mmap->addr; page_base < (mmap->addr + mmap->len); page_base += PAGE_SIZE) {
            auto addr = PhysicalAddress(page_base);

            if (page_base < 7 * MB) {
                // nothing
            } else if (page_base >= 7 * MB && page_base < 8 * MB) {
                if (region.is_null() || !region_is_super || region->upper().offset(PAGE_SIZE) != addr) {
                    m_super_physical_regions.append(PhysicalRegion::create(addr, addr));
                    region = m_super_physical_regions.last();
                    region_is_super = true;
                } else {
                    region->expand(region->lower(), addr);
                }
            } else {
                if (region.is_null() || region_is_super || region->upper().offset(PAGE_SIZE) != addr) {
                    m_user_physical_regions.append(PhysicalRegion::create(addr, addr));
                    region = m_user_physical_regions.last();
                    region_is_super = false;
                } else {
                    region->expand(region->lower(), addr);
                }
            }
        }
    }

    for (auto& region : m_super_physical_regions)
        m_super_physical_pages += region.finalize_capacity();

    for (auto& region : m_user_physical_regions)
        m_user_physical_pages += region.finalize_capacity();

    ASSERT(m_super_physical_pages > 0);
    ASSERT(m_user_physical_pages > 0);
}

const PageTableEntry* MemoryManager::pte(const PageDirectory& page_directory, VirtualAddress vaddr)
{
    ASSERT_INTERRUPTS_DISABLED();
    u32 page_directory_table_index = (vaddr.get() >> 30) & 0x3;
    u32 page_directory_index = (vaddr.get() >> 21) & 0x1ff;
    u32 page_table_index = (vaddr.get() >> 12) & 0x1ff;

    auto* pd = quickmap_pd(const_cast<PageDirectory&>(page_directory), page_directory_table_index);
    const PageDirectoryEntry& pde = pd[page_directory_index];
    if (!pde.is_present())
        return nullptr;

    return &quickmap_pt(PhysicalAddress((FlatPtr)pde.page_table_base()))[page_table_index];
}

PageTableEntry& MemoryManager::ensure_pte(PageDirectory& page_directory, VirtualAddress vaddr)
{
    ASSERT_INTERRUPTS_DISABLED();
    u32 page_directory_table_index = (vaddr.get() >> 30) & 0x3;
    u32 page_directory_index = (vaddr.get() >> 21) & 0x1ff;
    u32 page_table_index = (vaddr.get() >> 12) & 0x1ff;

    auto* pd = quickmap_pd(page_directory, page_directory_table_index);
    PageDirectoryEntry& pde = pd[page_directory_index];
    if (!pde.is_present()) {
#ifdef MM_DEBUG
        dbg() << "MM: PDE " << page_directory_index << " not present (requested for " << vaddr << "), allocating";
#endif
        auto page_table = allocate_user_physical_page(ShouldZeroFill::Yes);
#ifdef MM_DEBUG
        dbg() << "MM: PD K" << &page_directory << " (" << (&page_directory == m_kernel_page_directory ? "Kernel" : "User") << ") at " << PhysicalAddress(page_directory.cr3()) << " allocated page table #" << page_directory_index << " (for " << vaddr << ") at " << page_table->paddr();
#endif
        pde.set_page_table_base(page_table->paddr().get());
        pde.set_user_allowed(true);
        pde.set_present(true);
        pde.set_writable(true);
        pde.set_global(&page_directory == m_kernel_page_directory.ptr());
        page_directory.m_physical_pages.set(page_directory_index, move(page_table));
    }

    return quickmap_pt(PhysicalAddress((FlatPtr)pde.page_table_base()))[page_table_index];
}

void MemoryManager::initialize()
{
    s_the = new MemoryManager;
}

Region* MemoryManager::kernel_region_from_vaddr(VirtualAddress vaddr)
{
    if (vaddr.get() < 0xc0000000)
        return nullptr;
    for (auto& region : MM.m_kernel_regions) {
        if (region.contains(vaddr))
            return &region;
    }
    return nullptr;
}

Region* MemoryManager::user_region_from_vaddr(Process& process, VirtualAddress vaddr)
{
    // FIXME: Use a binary search tree (maybe red/black?) or some other more appropriate data structure!
    for (auto& region : process.m_regions) {
        if (region.contains(vaddr))
            return &region;
    }
#ifdef MM_DEBUG
    dbg() << process << " Couldn't find user region for " << vaddr;
#endif
    return nullptr;
}

Region* MemoryManager::region_from_vaddr(Process& process, VirtualAddress vaddr)
{
    if (auto* region = user_region_from_vaddr(process, vaddr))
        return region;
    return kernel_region_from_vaddr(vaddr);
}

const Region* MemoryManager::region_from_vaddr(const Process& process, VirtualAddress vaddr)
{
    if (auto* region = user_region_from_vaddr(const_cast<Process&>(process), vaddr))
        return region;
    return kernel_region_from_vaddr(vaddr);
}

Region* MemoryManager::region_from_vaddr(VirtualAddress vaddr)
{
    if (auto* region = kernel_region_from_vaddr(vaddr))
        return region;
    auto page_directory = PageDirectory::find_by_cr3(read_cr3());
    if (!page_directory)
        return nullptr;
    ASSERT(page_directory->process());
    return user_region_from_vaddr(*page_directory->process(), vaddr);
}

PageFaultResponse MemoryManager::handle_page_fault(const PageFault& fault)
{
    ASSERT_INTERRUPTS_DISABLED();
    ASSERT(Thread::current);
    if (g_in_irq) {
        dbg() << "BUG! Page fault while handling IRQ! code=" << fault.code() << ", vaddr=" << fault.vaddr();
        dump_kernel_regions();
    }
#ifdef PAGE_FAULT_DEBUG
    dbg() << "MM: handle_page_fault(" << String::format("%w", fault.code()) << ") at " << fault.vaddr();
#endif
    auto* region = region_from_vaddr(fault.vaddr());
    if (!region) {
        klog() << "NP(error) fault at invalid address " << fault.vaddr();
        return PageFaultResponse::ShouldCrash;
    }

    return region->handle_fault(fault);
}

OwnPtr<Region> MemoryManager::allocate_contiguous_kernel_region(size_t size, const StringView& name, u8 access, bool user_accessible, bool cacheable)
{
    ASSERT(!(size % PAGE_SIZE));
    auto range = kernel_page_directory().range_allocator().allocate_anywhere(size);
    if (!range.is_valid())
        return nullptr;
    auto vmobject = ContiguousVMObject::create_with_size(size);
    auto region = allocate_kernel_region_with_vmobject(range, vmobject, name, access, user_accessible, cacheable);
    if (!region)
        return nullptr;
    return region;
}

OwnPtr<Region> MemoryManager::allocate_kernel_region(size_t size, const StringView& name, u8 access, bool user_accessible, bool should_commit, bool cacheable)
{
    ASSERT(!(size % PAGE_SIZE));
    auto range = kernel_page_directory().range_allocator().allocate_anywhere(size);
    if (!range.is_valid())
        return nullptr;
    auto vmobject = AnonymousVMObject::create_with_size(size);
    auto region = allocate_kernel_region_with_vmobject(range, vmobject, name, access, user_accessible, cacheable);
    if (!region)
        return nullptr;
    if (should_commit && !region->commit())
        return nullptr;
    return region;
}

OwnPtr<Region> MemoryManager::allocate_kernel_region(PhysicalAddress paddr, size_t size, const StringView& name, u8 access, bool user_accessible, bool cacheable)
{
    ASSERT(!(size % PAGE_SIZE));
    auto range = kernel_page_directory().range_allocator().allocate_anywhere(size);
    if (!range.is_valid())
        return nullptr;
    auto vmobject = AnonymousVMObject::create_for_physical_range(paddr, size);
    if (!vmobject)
        return nullptr;
    return allocate_kernel_region_with_vmobject(range, *vmobject, name, access, user_accessible, cacheable);
}

OwnPtr<Region> MemoryManager::allocate_user_accessible_kernel_region(size_t size, const StringView& name, u8 access, bool cacheable)
{
    return allocate_kernel_region(size, name, access, true, true, cacheable);
}

OwnPtr<Region> MemoryManager::allocate_kernel_region_with_vmobject(const Range& range, VMObject& vmobject, const StringView& name, u8 access, bool user_accessible, bool cacheable)
{
    InterruptDisabler disabler;
    OwnPtr<Region> region;
    if (user_accessible)
        region = Region::create_user_accessible(range, vmobject, 0, name, access, cacheable);
    else
        region = Region::create_kernel_only(range, vmobject, 0, name, access, cacheable);
    if (region)
        region->map(kernel_page_directory());
    return region;
}

OwnPtr<Region> MemoryManager::allocate_kernel_region_with_vmobject(VMObject& vmobject, size_t size, const StringView& name, u8 access, bool user_accessible, bool cacheable)
{
    ASSERT(!(size % PAGE_SIZE));
    auto range = kernel_page_directory().range_allocator().allocate_anywhere(size);
    if (!range.is_valid())
        return nullptr;
    return allocate_kernel_region_with_vmobject(range, vmobject, name, access, user_accessible, cacheable);
}

void MemoryManager::deallocate_user_physical_page(PhysicalPage&& page)
{
    for (auto& region : m_user_physical_regions) {
        if (!region.contains(page)) {
            klog() << "MM: deallocate_user_physical_page: " << page.paddr() << " not in " << region.lower() << " -> " << region.upper();
            continue;
        }

        region.return_page(move(page));
        --m_user_physical_pages_used;

        return;
    }

    klog() << "MM: deallocate_user_physical_page couldn't figure out region for user page @ " << page.paddr();
    ASSERT_NOT_REACHED();
}

RefPtr<PhysicalPage> MemoryManager::find_free_user_physical_page()
{
    RefPtr<PhysicalPage> page;
    for (auto& region : m_user_physical_regions) {
        page = region.take_free_page(false);
        if (!page.is_null())
            break;
    }
    return page;
}

RefPtr<PhysicalPage> MemoryManager::allocate_user_physical_page(ShouldZeroFill should_zero_fill)
{
    InterruptDisabler disabler;
    auto page = find_free_user_physical_page();

    if (!page) {
        // We didn't have a single free physical page. Let's try to free something up!
        // First, we look for a purgeable VMObject in the volatile state.
        for_each_vmobject_of_type<PurgeableVMObject>([&](auto& vmobject) {
            int purged_page_count = vmobject.purge_with_interrupts_disabled({});
            if (purged_page_count) {
                klog() << "MM: Purge saved the day! Purged " << purged_page_count << " pages from PurgeableVMObject{" << &vmobject << "}";
                page = find_free_user_physical_page();
                ASSERT(page);
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });

        if (!page) {
            klog() << "MM: no user physical pages available";
            return {};
        }
    }

#ifdef MM_DEBUG
    dbg() << "MM: allocate_user_physical_page vending " << page->paddr();
#endif

    if (should_zero_fill == ShouldZeroFill::Yes) {
        auto* ptr = quickmap_page(*page);
        memset(ptr, 0, PAGE_SIZE);
        unquickmap_page();
    }

    ++m_user_physical_pages_used;
    return page;
}

void MemoryManager::deallocate_supervisor_physical_page(PhysicalPage&& page)
{
    for (auto& region : m_super_physical_regions) {
        if (!region.contains(page)) {
            klog() << "MM: deallocate_supervisor_physical_page: " << page.paddr() << " not in " << region.lower() << " -> " << region.upper();
            continue;
        }

        region.return_page(move(page));
        --m_super_physical_pages_used;
        return;
    }

    klog() << "MM: deallocate_supervisor_physical_page couldn't figure out region for super page @ " << page.paddr();
    ASSERT_NOT_REACHED();
}

NonnullRefPtrVector<PhysicalPage> MemoryManager::allocate_contiguous_supervisor_physical_pages(size_t size)
{
    ASSERT(!(size % PAGE_SIZE));
    InterruptDisabler disabler;
    size_t count = ceil_div(size, PAGE_SIZE);
    NonnullRefPtrVector<PhysicalPage> physical_pages;

    for (auto& region : m_super_physical_regions) {
        physical_pages = region.take_contiguous_free_pages((count), true);
        if (physical_pages.is_empty())
            continue;
    }

    if (physical_pages.is_empty()) {
        if (m_super_physical_regions.is_empty()) {
            klog() << "MM: no super physical regions available (?)";
        }

        klog() << "MM: no super physical pages available";
        ASSERT_NOT_REACHED();
        return {};
    }

    auto cleanup_region = MM.allocate_kernel_region(physical_pages[0].paddr(), PAGE_SIZE * count, "MemoryManager Allocation Sanitization", Region::Access::Read | Region::Access::Write);
    fast_u32_fill((u32*)cleanup_region->vaddr().as_ptr(), 0, (PAGE_SIZE * count) / sizeof(u32));
    m_super_physical_pages_used += count;
    return physical_pages;
}

RefPtr<PhysicalPage> MemoryManager::allocate_supervisor_physical_page()
{
    InterruptDisabler disabler;
    RefPtr<PhysicalPage> page;

    for (auto& region : m_super_physical_regions) {
        page = region.take_free_page(true);
        if (page.is_null())
            continue;
    }

    if (!page) {
        if (m_super_physical_regions.is_empty()) {
            klog() << "MM: no super physical regions available (?)";
        }

        klog() << "MM: no super physical pages available";
        ASSERT_NOT_REACHED();
        return {};
    }

#ifdef MM_DEBUG
    dbg() << "MM: allocate_supervisor_physical_page vending " << page->paddr();
#endif

    fast_u32_fill((u32*)page->paddr().offset(0xc0000000).as_ptr(), 0, PAGE_SIZE / sizeof(u32));
    ++m_super_physical_pages_used;
    return page;
}

void MemoryManager::enter_process_paging_scope(Process& process)
{
    ASSERT(Thread::current);
    InterruptDisabler disabler;

    Thread::current->tss().cr3 = process.page_directory().cr3();
    write_cr3(process.page_directory().cr3());
}

void MemoryManager::flush_entire_tlb()
{
    write_cr3(read_cr3());
}

void MemoryManager::flush_tlb(VirtualAddress vaddr)
{
#ifdef MM_DEBUG
    dbg() << "MM: Flush page " << vaddr;
#endif
    asm volatile("invlpg %0"
                 :
                 : "m"(*(char*)vaddr.get())
                 : "memory");
}

extern "C" PageTableEntry boot_pd3_pde1023_pt[1024];

PageDirectoryEntry* MemoryManager::quickmap_pd(PageDirectory& directory, size_t pdpt_index)
{
    auto& pte = boot_pd3_pde1023_pt[4];
    auto pd_paddr = directory.m_directory_pages[pdpt_index]->paddr();
    if (pte.physical_page_base() != pd_paddr.as_ptr()) {
#ifdef MM_DEBUG
        dbg() << "quickmap_pd: Mapping P" << (void*)directory.m_directory_pages[pdpt_index]->paddr().as_ptr() << " at 0xffe04000 in pte @ " << &pte;
#endif
        pte.set_physical_page_base(pd_paddr.get());
        pte.set_present(true);
        pte.set_writable(true);
        pte.set_user_allowed(false);
        flush_tlb(VirtualAddress(0xffe04000));
    }
    return (PageDirectoryEntry*)0xffe04000;
}

PageTableEntry* MemoryManager::quickmap_pt(PhysicalAddress pt_paddr)
{
    auto& pte = boot_pd3_pde1023_pt[8];
    if (pte.physical_page_base() != pt_paddr.as_ptr()) {
#ifdef MM_DEBUG
        dbg() << "quickmap_pt: Mapping P" << (void*)pt_paddr.as_ptr() << " at 0xffe08000 in pte @ " << &pte;
#endif
        pte.set_physical_page_base(pt_paddr.get());
        pte.set_present(true);
        pte.set_writable(true);
        pte.set_user_allowed(false);
        flush_tlb(VirtualAddress(0xffe08000));
    }
    return (PageTableEntry*)0xffe08000;
}

u8* MemoryManager::quickmap_page(PhysicalPage& physical_page)
{
    ASSERT_INTERRUPTS_DISABLED();
    ASSERT(!m_quickmap_in_use);
    m_quickmap_in_use = true;

    auto& pte = boot_pd3_pde1023_pt[0];
    if (pte.physical_page_base() != physical_page.paddr().as_ptr()) {
#ifdef MM_DEBUG
        dbg() << "quickmap_page: Mapping P" << (void*)physical_page.paddr().as_ptr() << " at 0xffe00000 in pte @ " << &pte;
#endif
        pte.set_physical_page_base(physical_page.paddr().get());
        pte.set_present(true);
        pte.set_writable(true);
        pte.set_user_allowed(false);
        flush_tlb(VirtualAddress(0xffe00000));
    }
    return (u8*)0xffe00000;
}

void MemoryManager::unquickmap_page()
{
    ASSERT_INTERRUPTS_DISABLED();
    ASSERT(m_quickmap_in_use);
    auto& pte = boot_pd3_pde1023_pt[0];
    pte.clear();
    flush_tlb(VirtualAddress(0xffe00000));
    m_quickmap_in_use = false;
}

template<MemoryManager::AccessSpace space, MemoryManager::AccessType access_type>
bool MemoryManager::validate_range(const Process& process, VirtualAddress base_vaddr, size_t size) const
{
    ASSERT(size);
    if (base_vaddr > base_vaddr.offset(size)) {
        dbg() << "Shenanigans! Asked to validate wrappy " << base_vaddr << " size=" << size;
        return false;
    }

    VirtualAddress vaddr = base_vaddr.page_base();
    VirtualAddress end_vaddr = base_vaddr.offset(size - 1).page_base();
    if (end_vaddr < vaddr) {
        dbg() << "Shenanigans! Asked to validate " << base_vaddr << " size=" << size;
        return false;
    }
    const Region* region = nullptr;
    while (vaddr <= end_vaddr) {
        if (!region || !region->contains(vaddr)) {
            if (space == AccessSpace::Kernel)
                region = kernel_region_from_vaddr(vaddr);
            if (!region || !region->contains(vaddr))
                region = user_region_from_vaddr(const_cast<Process&>(process), vaddr);
            if (!region
                || (space == AccessSpace::User && !region->is_user_accessible())
                || (access_type == AccessType::Read && !region->is_readable())
                || (access_type == AccessType::Write && !region->is_writable())) {
                return false;
            }
        }
        vaddr = region->range().end();
    }
    return true;
}

bool MemoryManager::validate_user_stack(const Process& process, VirtualAddress vaddr) const
{
    if (!is_user_address(vaddr))
        return false;
    auto* region = user_region_from_vaddr(const_cast<Process&>(process), vaddr);
    return region && region->is_user_accessible() && region->is_stack();
}

bool MemoryManager::validate_kernel_read(const Process& process, VirtualAddress vaddr, size_t size) const
{
    return validate_range<AccessSpace::Kernel, AccessType::Read>(process, vaddr, size);
}

bool MemoryManager::can_read_without_faulting(const Process& process, VirtualAddress vaddr, size_t size) const
{
    // FIXME: Use the size argument!
    UNUSED_PARAM(size);
    auto* pte = const_cast<MemoryManager*>(this)->pte(process.page_directory(), vaddr);
    if (!pte)
        return false;
    return pte->is_present();
}

bool MemoryManager::validate_user_read(const Process& process, VirtualAddress vaddr, size_t size) const
{
    if (!is_user_address(vaddr))
        return false;
    return validate_range<AccessSpace::User, AccessType::Read>(process, vaddr, size);
}

bool MemoryManager::validate_user_write(const Process& process, VirtualAddress vaddr, size_t size) const
{
    if (!is_user_address(vaddr))
        return false;
    return validate_range<AccessSpace::User, AccessType::Write>(process, vaddr, size);
}

void MemoryManager::register_vmobject(VMObject& vmobject)
{
    InterruptDisabler disabler;
    m_vmobjects.append(&vmobject);
}

void MemoryManager::unregister_vmobject(VMObject& vmobject)
{
    InterruptDisabler disabler;
    m_vmobjects.remove(&vmobject);
}

void MemoryManager::register_region(Region& region)
{
    InterruptDisabler disabler;
    if (region.vaddr().get() >= 0xc0000000)
        m_kernel_regions.append(&region);
    else
        m_user_regions.append(&region);
}

void MemoryManager::unregister_region(Region& region)
{
    InterruptDisabler disabler;
    if (region.vaddr().get() >= 0xc0000000)
        m_kernel_regions.remove(&region);
    else
        m_user_regions.remove(&region);
}

void MemoryManager::dump_kernel_regions()
{
    klog() << "Kernel regions:";
    klog() << "BEGIN       END         SIZE        ACCESS  NAME";
    for (auto& region : MM.m_kernel_regions) {
        klog() << String::format("%08x", region.vaddr().get()) << " -- " << String::format("%08x", region.vaddr().offset(region.size() - 1).get()) << "    " << String::format("%08x", region.size()) << "    " << (region.is_readable() ? 'R' : ' ') << (region.is_writable() ? 'W' : ' ') << (region.is_executable() ? 'X' : ' ') << (region.is_shared() ? 'S' : ' ') << (region.is_stack() ? 'T' : ' ') << (region.vmobject().is_purgeable() ? 'P' : ' ') << "    " << region.name().characters();
    }
}

}
