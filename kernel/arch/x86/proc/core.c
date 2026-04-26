/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * core.c — Linux-style ELF core dump writer for crashing user processes.
 */

#include "core.h"

#ifdef __aarch64__

int core_dump_process(process_t *proc, int signum)
{
	(void)proc;
	(void)signum;
	return -1;
}

#else

#include "arch.h"
#include "elf.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "kstring.h"
#include "mem_forensics.h"
#include "paging.h"
#include "vfs.h"
#include <stdint.h>

#define CORE_NOTE_NAME_ALIGN 4u
#define CORE_SEG_ALIGN 0x1000u
#define ELF_NGREG 17u

/* ELF note types for Linux-format core files. */
#define NT_PRSTATUS 1 /* prstatus_t — register state + signal info */
#define NT_PRPSINFO 3 /* prpsinfo_t — process name and argv        */
#define NT_DRUNIX_VMSTAT 0x4458564du
#define NT_DRUNIX_FAULT 0x44584654u
#define NT_DRUNIX_MAPS 0x44584d50u

static const char drunix_note_name[] = "DRUNIX";

typedef uint32_t elf_greg_t;
typedef elf_greg_t elf_gregset_t[ELF_NGREG];

typedef struct __attribute__((packed)) {
	int32_t si_signo;
	int32_t si_code;
	int32_t si_errno;
} core_siginfo_t;

typedef struct __attribute__((packed)) {
	int32_t tv_sec;
	int32_t tv_usec;
} core_timeval_t;

typedef struct __attribute__((packed)) {
	core_siginfo_t pr_info;
	uint16_t pr_cursig;
	uint16_t pr_pad0;
	uint32_t pr_sigpend;
	uint32_t pr_sighold;
	int32_t pr_pid;
	int32_t pr_ppid;
	int32_t pr_pgrp;
	int32_t pr_sid;
	core_timeval_t pr_utime;
	core_timeval_t pr_stime;
	core_timeval_t pr_cutime;
	core_timeval_t pr_cstime;
	elf_gregset_t pr_reg;
	int32_t pr_fpvalid;
} core_prstatus_t;

typedef char
    core_prstatus_size_check[(sizeof(core_prstatus_t) == 144) ? 1 : -1];

/*
 * NT_PRPSINFO (type 3) — process info note, carries the executable name.
 * Layout matches Linux's elf_prpsinfo for 32-bit targets.
 */
typedef struct __attribute__((packed)) {
	char pr_state;    /* numeric process state */
	char pr_sname;    /* char for pr_state */
	char pr_zomb;     /* zombie flag */
	char pr_nice;     /* nice value */
	uint32_t pr_flag; /* process flags */
	uint16_t pr_uid;
	uint16_t pr_gid;
	int32_t pr_pid;
	int32_t pr_ppid;
	int32_t pr_pgrp;
	int32_t pr_sid;
	char pr_fname[16];  /* basename of executable */
	char pr_psargs[80]; /* first 80 chars of argv (best-effort) */
} core_prpsinfo_t;

typedef char
    core_prpsinfo_size_check[(sizeof(core_prpsinfo_t) == 124) ? 1 : -1];

static uint32_t align_up(uint32_t val, uint32_t align)
{
	return (val + align - 1u) & ~(align - 1u);
}

static int
write_exact(uint32_t inode_num, uint32_t offset, const void *buf, uint32_t size)
{
	return fs_write(inode_num, offset, (const uint8_t *)buf, size) == (int)size
	           ? 0
	           : -1;
}

static void build_core_path(const process_t *proc, char *out, int outsz)
{
	if (proc->cwd[0] != '\0')
		k_snprintf(out, (uint32_t)outsz, "%s/core.%u", proc->cwd, proc->pid);
	else
		k_snprintf(out, (uint32_t)outsz, "core.%u", proc->pid);
}

static int user_page_info(uint32_t pd_phys,
                          uint32_t vaddr,
                          uint32_t *phys_out,
                          uint32_t *pte_flags_out)
{
	uint32_t *pd;
	uint32_t *pt;
	uint32_t pde;
	uint32_t pte;
	uint32_t pdi;
	uint32_t pti;

	if (vaddr >= USER_STACK_TOP)
		return 0;

	pd = (uint32_t *)pd_phys;
	pdi = vaddr >> 22;
	pde = pd[pdi];
	if (!(pde & PG_PRESENT) || !(pde & PG_USER))
		return 0;

	pt = (uint32_t *)(pde & ~0xFFFu);
	pti = (vaddr >> 12) & 0x3FFu;
	pte = pt[pti];
	if (!(pte & PG_PRESENT) || !(pte & PG_USER))
		return 0;

	if (phys_out)
		*phys_out = pte & ~0xFFFu;
	if (pte_flags_out)
		*pte_flags_out = pte & 0xFFFu;
	return 1;
}

static uint32_t core_segment_flags(uint32_t pte_flags)
{
	uint32_t flags = PF_R;
	if (pte_flags & PG_WRITABLE)
		flags |= PF_W;
	return flags;
}

static int next_user_segment(process_t *proc,
                             uint32_t search_start,
                             uint32_t *seg_vaddr,
                             uint32_t *seg_size,
                             uint32_t *seg_flags,
                             uint32_t *next_search)
{
	uint32_t vaddr = search_start & ~0xFFFu;
	uint32_t phys;
	uint32_t pte_flags;
	uint32_t flags;

	while (vaddr < USER_STACK_TOP) {
		if (!user_page_info(proc->pd_phys, vaddr, &phys, &pte_flags)) {
			vaddr += 0x1000u;
			continue;
		}

		*seg_vaddr = vaddr;
		*seg_size = 0;
		flags = core_segment_flags(pte_flags);
		*seg_flags = flags;

		do {
			*seg_size += 0x1000u;
			vaddr += 0x1000u;
		} while (vaddr < USER_STACK_TOP &&
		         user_page_info(proc->pd_phys, vaddr, &phys, &pte_flags) &&
		         core_segment_flags(pte_flags) == flags);

		*next_search = vaddr;
		return 1;
	}

	return 0;
}

static uint32_t count_user_segments(process_t *proc)
{
	uint32_t count = 0;
	uint32_t search = 0;
	uint32_t seg_vaddr, seg_size, seg_flags;

	while (next_user_segment(
	    proc, search, &seg_vaddr, &seg_size, &seg_flags, &search)) {
		count++;
	}

	return count;
}

static void fill_prstatus(core_prstatus_t *st, process_t *proc, int signum)
{
	k_memset(st, 0, sizeof(*st));

	st->pr_info.si_signo = signum;
	st->pr_cursig = (uint16_t)signum;
	st->pr_sigpend = proc->sig_pending;
	st->pr_sighold = proc->sig_blocked;
	st->pr_pid = (int32_t)proc->pid;
	st->pr_ppid = (int32_t)proc->parent_pid;
	st->pr_pgrp = (int32_t)proc->pgid;
	st->pr_sid = (int32_t)proc->sid;

	arch_core_fill_prstatus_regs(st->pr_reg, &proc->crash.frame);
}

static uint32_t prstatus_note_size(void)
{
	static const char note_name[] = "CORE";
	return sizeof(Elf32_Nhdr) +
	       align_up((uint32_t)sizeof(note_name), CORE_NOTE_NAME_ALIGN) +
	       align_up((uint32_t)sizeof(core_prstatus_t), CORE_NOTE_NAME_ALIGN);
}

static int write_prstatus_note(uint32_t inode_num,
                               uint32_t offset,
                               process_t *proc,
                               int signum)
{
	static const char note_name[] = "CORE";
	static const uint8_t zero_pad[4] = {0, 0, 0, 0};
	Elf32_Nhdr nhdr;
	core_prstatus_t prstatus;
	uint32_t name_pad;
	uint32_t desc_pad;

	nhdr.n_namesz = (uint32_t)sizeof(note_name);
	nhdr.n_descsz = (uint32_t)sizeof(prstatus);
	nhdr.n_type = NT_PRSTATUS;

	fill_prstatus(&prstatus, proc, signum);
	name_pad = align_up(nhdr.n_namesz, CORE_NOTE_NAME_ALIGN) - nhdr.n_namesz;
	desc_pad = align_up(nhdr.n_descsz, CORE_NOTE_NAME_ALIGN) - nhdr.n_descsz;

	if (write_exact(inode_num, offset, &nhdr, sizeof(nhdr)) != 0)
		return -1;
	offset += (uint32_t)sizeof(nhdr);

	if (write_exact(inode_num, offset, note_name, nhdr.n_namesz) != 0)
		return -1;
	offset += nhdr.n_namesz;

	if (name_pad && write_exact(inode_num, offset, zero_pad, name_pad) != 0)
		return -1;
	offset += name_pad;

	if (write_exact(inode_num, offset, &prstatus, sizeof(prstatus)) != 0)
		return -1;
	offset += (uint32_t)sizeof(prstatus);

	if (desc_pad && write_exact(inode_num, offset, zero_pad, desc_pad) != 0)
		return -1;

	return 0;
}

static uint32_t prpsinfo_note_size(void)
{
	static const char note_name[] = "CORE";
	return sizeof(Elf32_Nhdr) +
	       align_up((uint32_t)sizeof(note_name), CORE_NOTE_NAME_ALIGN) +
	       align_up((uint32_t)sizeof(core_prpsinfo_t), CORE_NOTE_NAME_ALIGN);
}

static char core_proc_state_name(uint32_t state)
{
	switch (state) {
	case PROC_READY:
	case PROC_RUNNING:
		return 'R';
	case PROC_BLOCKED:
		return 'S';
	case PROC_STOPPED:
		return 'T';
	case PROC_ZOMBIE:
		return 'Z';
	default:
		return 'R';
	}
}

static uint8_t core_proc_state_value(uint32_t state)
{
	switch (core_proc_state_name(state)) {
	case 'R':
		return 0;
	case 'S':
		return 1;
	case 'Z':
		return 3;
	case 'T':
		return 4;
	default:
		return 0;
	}
}

static int
write_prpsinfo_note(uint32_t inode_num, uint32_t offset, process_t *proc)
{
	static const char note_name[] = "CORE";
	static const uint8_t zero_pad[4] = {0, 0, 0, 0};
	Elf32_Nhdr nhdr;
	core_prpsinfo_t info;
	uint32_t name_pad;
	uint32_t desc_pad;

	k_memset(&info, 0, sizeof(info));
	info.pr_state = (char)core_proc_state_value(proc->state);
	info.pr_sname = core_proc_state_name(proc->state);
	info.pr_zomb = (proc->state == PROC_ZOMBIE) ? 1 : 0;
	info.pr_nice = 0;
	info.pr_flag = 0;
	info.pr_uid = 0;
	info.pr_gid = 0;
	info.pr_pid = (int32_t)proc->pid;
	info.pr_ppid = (int32_t)proc->parent_pid;
	info.pr_pgrp = (int32_t)proc->pgid;
	info.pr_sid = (int32_t)proc->sid;
	k_memcpy(info.pr_fname, proc->name, sizeof(info.pr_fname));
	k_memcpy(info.pr_psargs, proc->psargs, sizeof(info.pr_psargs));

	nhdr.n_namesz = (uint32_t)sizeof(note_name);
	nhdr.n_descsz = (uint32_t)sizeof(info);
	nhdr.n_type = NT_PRPSINFO;
	name_pad = align_up(nhdr.n_namesz, CORE_NOTE_NAME_ALIGN) - nhdr.n_namesz;
	desc_pad = align_up(nhdr.n_descsz, CORE_NOTE_NAME_ALIGN) - nhdr.n_descsz;

	if (write_exact(inode_num, offset, &nhdr, sizeof(nhdr)) != 0)
		return -1;
	offset += (uint32_t)sizeof(nhdr);

	if (write_exact(inode_num, offset, note_name, nhdr.n_namesz) != 0)
		return -1;
	offset += nhdr.n_namesz;

	if (name_pad && write_exact(inode_num, offset, zero_pad, name_pad) != 0)
		return -1;
	offset += name_pad;

	if (write_exact(inode_num, offset, &info, sizeof(info)) != 0)
		return -1;
	offset += (uint32_t)sizeof(info);

	if (desc_pad && write_exact(inode_num, offset, zero_pad, desc_pad) != 0)
		return -1;

	return 0;
}

static uint32_t text_note_size(const char *name, uint32_t text_len)
{
	uint32_t name_len = (uint32_t)k_strlen(name) + 1u;

	return (uint32_t)sizeof(Elf32_Nhdr) +
	       align_up(name_len, CORE_NOTE_NAME_ALIGN) +
	       align_up(text_len, CORE_NOTE_NAME_ALIGN);
}

static int write_text_note(uint32_t inode_num,
                           uint32_t offset,
                           uint32_t type,
                           const char *name,
                           const char *text,
                           uint32_t text_len)
{
	Elf32_Nhdr nhdr;
	static const uint8_t zero_pad[4] = {0, 0, 0, 0};
	uint32_t name_pad;
	uint32_t desc_pad;

	nhdr.n_namesz = (uint32_t)k_strlen(name) + 1u;
	nhdr.n_descsz = text_len;
	nhdr.n_type = type;
	name_pad = align_up(nhdr.n_namesz, CORE_NOTE_NAME_ALIGN) - nhdr.n_namesz;
	desc_pad = align_up(nhdr.n_descsz, CORE_NOTE_NAME_ALIGN) - nhdr.n_descsz;

	if (write_exact(inode_num, offset, &nhdr, sizeof(nhdr)) != 0)
		return -1;
	offset += (uint32_t)sizeof(nhdr);

	if (write_exact(inode_num, offset, name, nhdr.n_namesz) != 0)
		return -1;
	offset += nhdr.n_namesz;

	if (name_pad && write_exact(inode_num, offset, zero_pad, name_pad) != 0)
		return -1;
	offset += name_pad;

	if (write_exact(inode_num, offset, text, text_len) != 0)
		return -1;
	offset += text_len;

	if (desc_pad && write_exact(inode_num, offset, zero_pad, desc_pad) != 0)
		return -1;

	return 0;
}

static int write_segment_bytes(uint32_t inode_num,
                               uint32_t file_offset,
                               process_t *proc,
                               uint32_t seg_vaddr,
                               uint32_t seg_size)
{
	uint32_t phys;

	for (uint32_t off = 0; off < seg_size; off += 0x1000u) {
		if (!user_page_info(proc->pd_phys, seg_vaddr + off, &phys, 0))
			return -1;
		if (write_exact(
		        inode_num, file_offset + off, (const void *)phys, 0x1000u) != 0)
			return -1;
	}

	return 0;
}

int core_dump_process(process_t *proc, int signum)
{
	char *path;
	int ino;
	int rc = -1;
	uint32_t seg_count;
	uint32_t phnum;
	uint32_t note_off;
	uint32_t note_size;
	uint32_t load_off;
	uint32_t seg_vaddr, seg_size, seg_flags;
	uint32_t search;
	uint32_t phoff;
	uint32_t notes_off;
	uint32_t vmstat_cap;
	uint32_t fault_cap;
	uint32_t maps_cap;
	char *vmstat_note = 0;
	char *fault_note = 0;
	char *maps_note = 0;
	uint32_t vmstat_size = 0;
	uint32_t fault_size = 0;
	uint32_t maps_size = 0;
	Elf32_Ehdr ehdr;
	Elf32_Phdr phdr;

	if (!proc || !proc->crash.valid)
		return -1;

	path = (char *)kmalloc(4096);
	if (!path)
		return -1;

	build_core_path(proc, path, 4096);
	ino = vfs_create(path);
	kfree(path);
	if (ino < 0)
		return -1;

	vmstat_cap = mem_forensics_vmstat_note_size();
	fault_cap = mem_forensics_fault_note_size();
	if (vmstat_cap == 0u || fault_cap == 0u)
		return -1;

	if (mem_forensics_render_maps(proc, 0, 0, &maps_size) != 0)
		return -1;
	maps_cap = maps_size + 1u;
	if (maps_cap == 0u)
		return -1;

	vmstat_note = (char *)kmalloc(vmstat_cap);
	fault_note = (char *)kmalloc(fault_cap);
	maps_note = (char *)kmalloc(maps_cap);
	if (!vmstat_note || !fault_note || !maps_note)
		goto cleanup;

	seg_count = count_user_segments(proc);
	phnum = seg_count + 1u; /* one PT_NOTE plus N PT_LOADs */
	if (mem_forensics_render_vmstat(
	        proc, vmstat_note, vmstat_cap, &vmstat_size) != 0)
		goto cleanup;
	if (mem_forensics_render_fault(proc, fault_note, fault_cap, &fault_size) !=
	    0)
		goto cleanup;
	if (mem_forensics_render_maps(proc, maps_note, maps_cap, &maps_size) != 0)
		goto cleanup;
	note_size = prstatus_note_size() + prpsinfo_note_size() +
	            text_note_size(drunix_note_name, vmstat_size) +
	            text_note_size(drunix_note_name, fault_size) +
	            text_note_size(drunix_note_name, maps_size);
	note_off =
	    (uint32_t)sizeof(Elf32_Ehdr) + phnum * (uint32_t)sizeof(Elf32_Phdr);
	load_off = align_up(note_off + note_size, CORE_SEG_ALIGN);

	k_memset(&ehdr, 0, sizeof(ehdr));
	ehdr.e_ident[0] = 0x7F;
	ehdr.e_ident[1] = 'E';
	ehdr.e_ident[2] = 'L';
	ehdr.e_ident[3] = 'F';
	ehdr.e_ident[4] = 1; /* ELFCLASS32 */
	ehdr.e_ident[5] = 1; /* ELFDATA2LSB */
	ehdr.e_ident[6] = 1; /* EV_CURRENT */
	ehdr.e_type = ET_CORE;
	ehdr.e_machine = EM_386;
	ehdr.e_version = 1;
	ehdr.e_phoff = (uint32_t)sizeof(Elf32_Ehdr);
	ehdr.e_ehsize = (uint16_t)sizeof(Elf32_Ehdr);
	ehdr.e_phentsize = (uint16_t)sizeof(Elf32_Phdr);
	ehdr.e_phnum = (uint16_t)phnum;

	if (write_exact((uint32_t)ino, 0, &ehdr, sizeof(ehdr)) != 0)
		goto cleanup;

	phoff = ehdr.e_phoff;
	k_memset(&phdr, 0, sizeof(phdr));
	phdr.p_type = PT_NOTE;
	phdr.p_offset = note_off;
	phdr.p_filesz = note_size;
	phdr.p_align = 4;
	if (write_exact((uint32_t)ino, phoff, &phdr, sizeof(phdr)) != 0)
		goto cleanup;
	phoff += (uint32_t)sizeof(phdr);

	search = 0;
	uint32_t cur_load_off = load_off;
	while (next_user_segment(
	    proc, search, &seg_vaddr, &seg_size, &seg_flags, &search)) {
		k_memset(&phdr, 0, sizeof(phdr));
		phdr.p_type = PT_LOAD;
		phdr.p_offset = cur_load_off;
		phdr.p_vaddr = seg_vaddr;
		phdr.p_filesz = seg_size;
		phdr.p_memsz = seg_size;
		phdr.p_flags = seg_flags;
		phdr.p_align = CORE_SEG_ALIGN;
		if (write_exact((uint32_t)ino, phoff, &phdr, sizeof(phdr)) != 0)
			goto cleanup;
		phoff += (uint32_t)sizeof(phdr);
		cur_load_off += seg_size;
	}

	if (write_prstatus_note((uint32_t)ino, note_off, proc, signum) != 0)
		goto cleanup;

	if (write_prpsinfo_note(
	        (uint32_t)ino, note_off + prstatus_note_size(), proc) != 0)
		goto cleanup;

	notes_off = note_off + prstatus_note_size() + prpsinfo_note_size();
	if (write_text_note((uint32_t)ino,
	                    notes_off,
	                    NT_DRUNIX_VMSTAT,
	                    drunix_note_name,
	                    vmstat_note,
	                    vmstat_size) != 0)
		goto cleanup;
	notes_off += text_note_size(drunix_note_name, vmstat_size);

	if (write_text_note((uint32_t)ino,
	                    notes_off,
	                    NT_DRUNIX_FAULT,
	                    drunix_note_name,
	                    fault_note,
	                    fault_size) != 0)
		goto cleanup;
	notes_off += text_note_size(drunix_note_name, fault_size);

	if (write_text_note((uint32_t)ino,
	                    notes_off,
	                    NT_DRUNIX_MAPS,
	                    drunix_note_name,
	                    maps_note,
	                    maps_size) != 0)
		goto cleanup;

	search = 0;
	cur_load_off = load_off;
	while (next_user_segment(
	    proc, search, &seg_vaddr, &seg_size, &seg_flags, &search)) {
		(void)seg_flags;
		if (write_segment_bytes(
		        (uint32_t)ino, cur_load_off, proc, seg_vaddr, seg_size) != 0)
			goto cleanup;
		cur_load_off += seg_size;
	}

	if (fs_flush_inode((uint32_t)ino) != 0)
		goto cleanup;

	rc = 0;

cleanup:
	if (maps_note)
		kfree(maps_note);
	if (fault_note)
		kfree(fault_note);
	if (vmstat_note)
		kfree(vmstat_note);
	return rc;
}

#endif
