#include <unistd.h>
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <mvas/vas.h>

#include "types.h"
#include "cr_options.h"
#include "servicefd.h"
#include "mem.h"
#include "parasite-syscall.h"
#include "parasite.h"
#include "page-pipe.h"
#include "page-xfer.h"
#include "log.h"
#include "kerndat.h"
#include "stats.h"
#include "vma.h"
#include "shmem.h"
#include "pstree.h"
#include "restorer.h"
#include "rst-malloc.h"
#include "bitmap.h"
#include "sk-packet.h"
#include "files-reg.h"
#include "pagemap-cache.h"
#include "fault-injection.h"

#include "protobuf.h"
#include "images/pagemap.pb-c.h"

static int task_reset_dirty_track(int pid)
{
	int ret;

	if (!opts.track_mem)
		return 0;

	BUG_ON(!kdat.has_dirty_track);

	ret = do_task_reset_dirty_track(pid);
	BUG_ON(ret == 1);
	return ret;
}

int do_task_reset_dirty_track(int pid)
{
	int fd, ret;
	char cmd[] = "4";

	pr_info("Reset %d's dirty tracking\n", pid);

	fd = __open_proc(pid, EACCES, O_RDWR, "clear_refs");
	if (fd < 0)
		return errno == EACCES ? 1 : -1;

	ret = write(fd, cmd, sizeof(cmd));
	if (ret < 0) {
               if (errno == EINVAL) /* No clear-soft-dirty in kernel */
                       ret = 1;
               else {
                       pr_perror("Can't reset %d's dirty memory tracker (%d)", pid, errno);
                       ret = -1;
               }
       } else {
               pr_info(" ... done\n");
               ret = 0;
	}

       close(fd);
       return ret;
}

unsigned int dump_pages_args_size(struct vm_area_list *vmas)
{
	/* In the worst case I need one iovec for each page */
	return sizeof(struct parasite_dump_pages_args) +
		vmas->nr * sizeof(struct parasite_vma_entry) +
		(vmas->priv_size + 1) * sizeof(struct iovec);
}

bool should_dump_page(VmaEntry *vmae, u64 pme)
{
#ifdef CONFIG_VDSO
	/*
	 * vDSO area must be always dumped because on restore
	 * we might need to generate a proxy.
	 */
	if (vma_entry_is(vmae, VMA_AREA_VDSO))
		return true;
	/*
	 * In turn VVAR area is special and referenced from
	 * vDSO area by IP addressing (at least on x86) thus
	 * never ever dump its content but always use one provided
	 * by the kernel on restore, ie runtime VVAR area must
	 * be remapped into proper place..
	 */
	if (vma_entry_is(vmae, VMA_AREA_VVAR))
		return false;
#endif
	/*
	 * Optimisation for private mapping pages, that haven't
	 * yet being COW-ed
	 */
	if (vma_entry_is(vmae, VMA_FILE_PRIVATE) && (pme & PME_FILE))
		return false;
	if (vma_entry_is(vmae, VMA_AREA_AIORING))
		return true;
	if (pme & PME_SWAP)
		return true;
	if ((pme & PME_PRESENT) && ((pme & PME_PFRAME_MASK) != kdat.zero_page_pfn))
		return true;

	return false;
}

bool page_in_parent(bool dirty)
{
	/*
	 * If we do memory tracking, but w/o parent images,
	 * then we have to dump all memory
	 */

	return opts.track_mem && opts.img_parent && !dirty;
}

/*
 * This routine finds out what memory regions to grab from the
 * dumpee. The iovs generated are then fed into vmsplice to
 * put the memory into the page-pipe's pipe.
 *
 * "Holes" in page-pipe are regions, that should be dumped, but
 * the memory contents is present in the pagent image set.
 */

static int generate_iovs(struct vma_area *vma, struct page_pipe *pp, u64 *map, u64 *off, bool has_parent)
{
	u64 *at = &map[PAGE_PFN(*off)];
	unsigned long pfn, nr_to_scan;
	unsigned long pages[2] = {};

	nr_to_scan = (vma_area_len(vma) - *off) / PAGE_SIZE;

	for (pfn = 0; pfn < nr_to_scan; pfn++) {
		unsigned long vaddr;
		int ret;

		if (!should_dump_page(vma->e, at[pfn]))
			continue;

		vaddr = vma->e->start + *off + pfn * PAGE_SIZE;

		/*
		 * If we're doing incremental dump (parent images
		 * specified) and page is not soft-dirty -- we dump
		 * hole and expect the parent images to contain this
		 * page. The latter would be checked in page-xfer.
		 */

		if (has_parent && page_in_parent(at[pfn] & PME_SOFT_DIRTY)) {
			ret = page_pipe_add_hole(pp, vaddr);
			pages[0]++;
		} else {
			ret = page_pipe_add_page(pp, vaddr);
			pages[1]++;
		}

		if (ret) {
			*off += pfn * PAGE_SIZE;
			return ret;
		}
	}

	*off += pfn * PAGE_SIZE;

	cnt_add(CNT_PAGES_SCANNED, nr_to_scan);
	cnt_add(CNT_PAGES_SKIPPED_PARENT, pages[0]);
	cnt_add(CNT_PAGES_WRITTEN, pages[1]);

	pr_info("Pagemap generated: %lu pages %lu holes\n", pages[1], pages[0]);
	return 0;
}

static struct parasite_dump_pages_args *prep_dump_pages_args(struct parasite_ctl *ctl,
		struct vm_area_list *vma_area_list, bool skip_non_trackable)
{
	struct parasite_dump_pages_args *args;
	struct parasite_vma_entry *p_vma;
	struct vma_area *vma;

	args = parasite_args_s(ctl, dump_pages_args_size(vma_area_list));

	p_vma = pargs_vmas(args);
	args->nr_vmas = 0;

	list_for_each_entry(vma, &vma_area_list->h, list) {
		if (!vma_area_is_private(vma, kdat.task_size))
			continue;
		/*
		 * Kernel write to aio ring is not soft-dirty tracked,
		 * so we ignore them at pre-dump.
		 */
		if (vma_entry_is(vma->e, VMA_AREA_AIORING) && skip_non_trackable)
			continue;
		if (vma->e->prot & PROT_READ)
			continue;

		p_vma->start = vma->e->start;
		p_vma->len = vma_area_len(vma);
		p_vma->prot = vma->e->prot;

		args->nr_vmas++;
		p_vma++;
	}

	return args;
}

static int drain_pages(struct page_pipe *pp, struct parasite_ctl *ctl,
		      struct parasite_dump_pages_args *args)
{
	struct page_pipe_buf *ppb;
	int ret = 0;

	debug_show_page_pipe(pp);

	/* Step 2 -- grab pages into page-pipe */
	list_for_each_entry(ppb, &pp->bufs, l) {
		args->nr_segs = ppb->nr_segs;
		args->nr_pages = ppb->pages_in;
		pr_debug("PPB: %d pages %d segs %u pipe %d off\n",
				args->nr_pages, args->nr_segs, ppb->pipe_size, args->off);

		ret = __parasite_execute_daemon(PARASITE_CMD_DUMPPAGES, ctl);
		if (ret < 0)
			return -1;
		ret = parasite_send_fd(ctl, ppb->p[1]);
		if (ret)
			return -1;

		ret = __parasite_wait_daemon_ack(PARASITE_CMD_DUMPPAGES, ctl);
		if (ret < 0)
			return -1;

		args->off += args->nr_segs;
	}

	return 0;
}

static int xfer_pages(struct page_pipe *pp, struct page_xfer *xfer)
{
	int ret;

	/*
	 * Step 3 -- write pages into image (or delay writing for
	 *           pre-dump action (see pre_dump_one_task)
	 */
	timing_start(TIME_MEMWRITE);
	ret = page_xfer_dump_pages(xfer, pp, 0);
	timing_stop(TIME_MEMWRITE);

	return ret;
}

static int __parasite_dump_pages_seized(struct pstree_item *item,
		struct parasite_dump_pages_args *args,
		struct vm_area_list *vma_area_list,
		struct mem_dump_ctl *mdc,
		struct parasite_ctl *ctl)
{
	pmc_t pmc = PMC_INIT;
	struct page_pipe *pp;
	struct vma_area *vma_area;
	struct page_xfer xfer = { .parent = NULL };
	int ret = -1;
	unsigned cpp_flags = 0;
	unsigned long pmc_size;

	pr_info("\n");
	pr_info("Dumping pages (type: %d pid: %d)\n", CR_FD_PAGES, item->pid->real);
	pr_info("----------------------------------------\n");

	timing_start(TIME_MEMDUMP);

	pr_debug("   Private vmas %lu/%lu pages\n",
			vma_area_list->priv_longest, vma_area_list->priv_size);

	/*
	 * Step 0 -- prepare
	 */

	pmc_size = max(vma_area_list->priv_longest,
		vma_area_list->shared_longest);
	if (pmc_init(&pmc, item->pid->real, &vma_area_list->h,
			 pmc_size * PAGE_SIZE))
		return -1;

	ret = -1;
	if (!mdc->pre_dump)
		/*
		 * Chunk mode pushes pages portion by portion. This mode
		 * only works when we don't need to keep pp for later
		 * use, i.e. on non-lazy non-predump.
		 */
		cpp_flags |= PP_CHUNK_MODE;
	pp = create_page_pipe(vma_area_list->priv_size,
					    pargs_iovs(args), cpp_flags);
	if (!pp)
		goto out;

	if (!mdc->pre_dump) {
		/*
		 * Regular dump -- create xfer object and send pages to it
		 * right here. For pre-dumps the pp will be taken by the
		 * caller and handled later.
		 */
		ret = open_page_xfer(&xfer, CR_FD_PAGEMAP, vpid(item));
		if (ret < 0)
			goto out_pp;
	} else {
		ret = check_parent_page_xfer(CR_FD_PAGEMAP, vpid(item));
		if (ret < 0)
			goto out_pp;

		if (ret)
			xfer.parent = NULL + 1;
	}

	/*
	 * Step 1 -- generate the pagemap
	 */
	args->off = 0;
	list_for_each_entry(vma_area, &vma_area_list->h, list) {
		bool has_parent = !!xfer.parent;
		u64 off = 0;
		u64 *map;

		if (!vma_area_is_private(vma_area, kdat.task_size) &&
				!vma_area_is(vma_area, VMA_ANON_SHARED))
			continue;
		if (vma_entry_is(vma_area->e, VMA_AREA_AIORING)) {
			if (mdc->pre_dump)
				continue;
			has_parent = false;
		}

		map = pmc_get_map(&pmc, vma_area);
		if (!map)
			goto out_xfer;
		if (vma_area_is(vma_area, VMA_ANON_SHARED))
			ret = add_shmem_area(item->pid->real, vma_area->e, map);
		else {
again:
			ret = generate_iovs(vma_area, pp, map, &off,
				has_parent);
			if (ret == -EAGAIN) {
				BUG_ON(!(pp->flags & PP_CHUNK_MODE));

				ret = drain_pages(pp, ctl, args);
				if (!ret)
					ret = xfer_pages(pp, &xfer);
				if (!ret) {
					page_pipe_reinit(pp);
					goto again;
				}
			}
		}
		if (ret < 0)
			goto out_xfer;
	}

	ret = drain_pages(pp, ctl, args);
	if (!ret && !mdc->pre_dump)
		ret = xfer_pages(pp, &xfer);
	if (ret)
		goto out_xfer;

	timing_stop(TIME_MEMDUMP);

	/*
	 * Step 4 -- clean up
	 */

	ret = task_reset_dirty_track(item->pid->real);
out_xfer:
	if (!mdc->pre_dump)
		xfer.close(&xfer);
out_pp:
	if (ret || !mdc->pre_dump)
		destroy_page_pipe(pp);
	else
		dmpi(item)->mem_pp = pp;
out:
	pmc_fini(&pmc);
	pr_info("----------------------------------------\n");
	return ret;
}

long vas_cow_pages(struct pstree_item *item)
{
   long ret;
   timing_start(TIME_VASFORK);
   ret = vas_fork(item->pid->real);
   timing_stop(TIME_VASFORK);

   if (ret == -1) {
       pr_err("Can't vas_fork memory\n");
   }
   pr_debug("vid after vas_fork: %ld\n", ret);

   return ret;
}

int parasite_dump_pages_seized(struct pstree_item *item,
		struct vm_area_list *vma_area_list,
		struct mem_dump_ctl *mdc,
		struct parasite_ctl *ctl)
{
	int ret;
	struct parasite_dump_pages_args *pargs;

	pargs = prep_dump_pages_args(ctl, vma_area_list, mdc->pre_dump);

	/*
	 * Add PROT_READ protection for all VMAs we're about to
	 * dump if they don't have one. Otherwise we'll not be
	 * able to read the memory contents.
	 *
	 * Afterwards -- reprotect memory back.
	 */

	pargs->add_prot = PROT_READ;
	ret = parasite_execute_daemon(PARASITE_CMD_MPROTECT_VMAS, ctl);
	if (ret) {
		pr_err("Can't dump unprotect vmas with parasite\n");
		return ret;
	}

	if (fault_injected(FI_DUMP_PAGES)) {
		pr_err("fault: Dump VMA pages failure!\n");
		return -1;
	}

	ret = __parasite_dump_pages_seized(item, pargs, vma_area_list, mdc, ctl);
	if (ret) {
		pr_err("Can't dump page with parasite\n");
		/* Parasite will unprotect VMAs after fail in fini() */
		return ret;
	}

	pargs->add_prot = 0;
	if (parasite_execute_daemon(PARASITE_CMD_MPROTECT_VMAS, ctl)) {
		pr_err("Can't rollback unprotected vmas with parasite\n");
		ret = -1;
	}

	return ret;
}

int prepare_mm_pid(struct pstree_item *i)
{
	pid_t pid = vpid(i);
	int ret = -1, vn = 0;
	struct cr_img *img;
	struct rst_info *ri = rsti(i);

	img = open_image(CR_FD_MM, O_RSTR, pid);
	if (!img)
		return -1;

	ret = pb_read_one_eof(img, &ri->mm, PB_MM);
	close_image(img);
	if (ret <= 0)
		return ret;

	if (collect_special_file(ri->mm->exe_file_id) == NULL)
		return -1;

	pr_debug("Found %zd VMAs in image\n", ri->mm->n_vmas);
	img = NULL;
	if (ri->mm->n_vmas == 0) {
		/*
		 * Old image. Read VMAs from vma-.img
		 */
		img = open_image(CR_FD_VMAS, O_RSTR, pid);
		if (!img)
			return -1;
	}


	while (vn < ri->mm->n_vmas || img != NULL) {
		struct vma_area *vma;

		ret = -1;
		vma = alloc_vma_area();
		if (!vma)
			break;

		ret = 0;
		ri->vmas.nr++;
		if (!img)
			vma->e = ri->mm->vmas[vn++];
		else {
			ret = pb_read_one_eof(img, &vma->e, PB_VMA);
			if (ret <= 0) {
				xfree(vma);
				close_image(img);
				break;
			}
		}
		list_add_tail(&vma->list, &ri->vmas.h);

		if (vma_area_is_private(vma, kdat.task_size)) {
			ri->vmas.priv_size += vma_area_len(vma);
			if (vma->e->flags & MAP_GROWSDOWN)
				ri->vmas.priv_size += PAGE_SIZE;
		}

		pr_info("vma 0x%"PRIx64" 0x%"PRIx64"\n", vma->e->start, vma->e->end);

		if (vma_area_is(vma, VMA_ANON_SHARED))
			ret = collect_shmem(pid, vma);
		else if (vma_area_is(vma, VMA_FILE_PRIVATE) ||
				vma_area_is(vma, VMA_FILE_SHARED))
			ret = collect_filemap(vma);
		else if (vma_area_is(vma, VMA_AREA_SOCKET))
			ret = collect_socket_map(vma);
		else
			ret = 0;
		if (ret)
			break;
	}

	return ret;
}

int prepare_mappings(struct pstree_item *t)
{
	int ret = 0;
	void *addr;
	struct vm_area_list *vmas;

	void *old_premmapped_addr = NULL;
	unsigned long old_premmapped_len;

	vmas = &rsti(t)->vmas;
	if (vmas->nr == 0) /* Zombie */
		goto out;

	/* Reserve a place for mapping private vma-s one by one */
	addr = mmap(NULL, vmas->priv_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (addr == MAP_FAILED) {
		ret = -1;
		pr_perror("Unable to reserve memory (%lu bytes)", vmas->priv_size);
		goto out;
	}

	old_premmapped_addr = rsti(t)->premmapped_addr;
	old_premmapped_len = rsti(t)->premmapped_len;
	rsti(t)->premmapped_addr = addr;
	rsti(t)->premmapped_len = vmas->priv_size;
/*
	ret = premap_priv_vmas(t, vmas, addr);
	if (ret < 0)
		goto out;

	ret = restore_priv_vma_content(t);
	if (ret < 0)
		goto out;
*/
	if (old_premmapped_addr) {
		ret = munmap(old_premmapped_addr, old_premmapped_len);
		if (ret < 0)
			pr_perror("Unable to unmap %p(%lx)",
					old_premmapped_addr, old_premmapped_len);
	}

out:
	return ret;
}

/*
 * A gard page must be unmapped after restoring content and
 * forking children to restore COW memory.
 */
int unmap_guard_pages(struct pstree_item *t)
{
	struct vma_area *vma;
	struct list_head *vmas = &rsti(t)->vmas.h;

	list_for_each_entry(vma, vmas, list) {
		if (!vma_area_is_private(vma, kdat.task_size))
			continue;

		if (vma->e->flags & MAP_GROWSDOWN) {
			void *addr = decode_pointer(vma->premmaped_addr);

			if (munmap(addr - PAGE_SIZE, PAGE_SIZE)) {
				pr_perror("Can't unmap guard page");
				return -1;
			}
		}
	}

	return 0;
}

int open_vmas(struct pstree_item *t)
{
	int pid = vpid(t);
	struct vma_area *vma;
	struct vm_area_list *vmas = &rsti(t)->vmas;

	list_for_each_entry(vma, &vmas->h, list) {
		if (!vma_area_is(vma, VMA_AREA_REGULAR) || !vma->vm_open)
			continue;

		pr_info("Opening %#016"PRIx64"-%#016"PRIx64" %#016"PRIx64" (%x) vma\n",
				vma->e->start, vma->e->end,
				vma->e->pgoff, vma->e->status);

		if (vma->vm_open(pid, vma)) {
			pr_err("`- Can't open vma\n");
			return -1;
		}
	}

	return 0;
}

int prepare_vmas(struct pstree_item *t, struct task_restore_args *ta)
{
	struct vma_area *vma;
	struct vm_area_list *vmas = &rsti(t)->vmas;

	ta->vmas = (VmaEntry *)rst_mem_align_cpos(RM_PRIVATE);
	ta->vmas_n = vmas->nr;

	list_for_each_entry(vma, &vmas->h, list) {
		VmaEntry *vme;

		vme = rst_mem_alloc(sizeof(*vme), RM_PRIVATE);
		if (!vme)
			return -1;

		/*
		 * Copy VMAs to private rst memory so that it's able to
		 * walk them and m(un|re)map.
		 */
		*vme = *vma->e;

		if (vma_area_is_private(vma, kdat.task_size))
			vma_premmaped_start(vme) = vma->premmaped_addr;
	}

	return 0;
}

