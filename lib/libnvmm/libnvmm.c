/*
 * Copyright (c) 2018-2021 Maxime Villard, m00nbsd.net
 * All rights reserved.
 *
 * This code is part of the NVMM hypervisor.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/queue.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "nvmm.h"

struct nvmm_capability_v2 {
	uint32_t version;
	uint32_t state_size;
	uint32_t max_machines;
	uint32_t max_vcpus;
	uint64_t max_ram;
	struct nvmm_cap_md arch;
};

struct nvmm_ioc_capability_v2 {
	struct nvmm_capability_v2 cap;
};

struct nvmm_ioc_vcpu_create_v2 {
	nvmm_machid_t machid;
	nvmm_cpuid_t cpuid;
};

static struct nvmm_capability __capability;

#define NVMM_COMM_PAGE_SIZE	\
	(roundup(sizeof(struct nvmm_comm_page), PAGE_SIZE))

#define NVMM_IOC_CAPABILITY_V2 _IOR ('N',  0, struct nvmm_ioc_capability_v2)
#define NVMM_IOC_VCPU_CREATE_V2 _IOW ('N',  4, struct nvmm_ioc_vcpu_create_v2)

#define NVMM_COMM_OFF(machid, cpuid)		\
	((((uint64_t)machid & 0xFFULL) << 20) |	\
	 (((uint64_t)cpuid & 0xFFULL) << 12))

#ifdef __x86_64__
#include "libnvmm_x86.c"
#endif

#ifdef __DragonFly__
#define LIST_FOREACH_SAFE	LIST_FOREACH_MUTABLE
#endif

typedef struct __area {
	LIST_ENTRY(__area) list;
	gpaddr_t gpa;
	uintptr_t hva;
	size_t size;
	nvmm_prot_t prot;
} area_t;

typedef LIST_HEAD(, __area) area_list_t;

static int nvmm_fd = -1;

/* -------------------------------------------------------------------------- */

static bool
__area_isvalid(struct nvmm_machine *mach, gpaddr_t gpa, size_t size)
{
	area_list_t *areas = mach->areas;
	area_t *ent;

	LIST_FOREACH(ent, areas, list) {
		/* Collision on GPA */
		if (gpa >= ent->gpa && gpa < ent->gpa + ent->size) {
			return false;
		}
		if (gpa + size > ent->gpa &&
		    gpa + size <= ent->gpa + ent->size) {
			return false;
		}
		if (gpa <= ent->gpa && gpa + size >= ent->gpa + ent->size) {
			return false;
		}
	}

	return true;
}

static int
__area_add(struct nvmm_machine *mach, uintptr_t hva, gpaddr_t gpa, size_t size,
    int prot)
{
	area_list_t *areas = mach->areas;
	nvmm_prot_t nprot;
	area_t *area;

	nprot = 0;
	if (prot & PROT_READ)
		nprot |= NVMM_PROT_READ;
	if (prot & PROT_WRITE)
		nprot |= NVMM_PROT_WRITE;
	if (prot & PROT_EXEC)
		nprot |= NVMM_PROT_EXEC;

	if (!__area_isvalid(mach, gpa, size)) {
		errno = EINVAL;
		return -1;
	}

	area = malloc(sizeof(*area));
	if (area == NULL)
		return -1;
	area->gpa = gpa;
	area->hva = hva;
	area->size = size;
	area->prot = nprot;

	LIST_INSERT_HEAD(areas, area, list);

	return 0;
}

static int
__area_delete(struct nvmm_machine *mach, uintptr_t hva, gpaddr_t gpa,
    size_t size)
{
	area_list_t *areas = mach->areas;
	area_t *ent, *nxt;

	LIST_FOREACH_SAFE(ent, areas, list, nxt) {
		if (hva == ent->hva && gpa == ent->gpa && size == ent->size) {
			LIST_REMOVE(ent, list);
			free(ent);
			return 0;
		}
	}

	return -1;
}

static void
__area_remove_all(struct nvmm_machine *mach)
{
	area_list_t *areas = mach->areas;
	area_t *ent;

	while ((ent = LIST_FIRST(areas)) != NULL) {
		LIST_REMOVE(ent, list);
		free(ent);
	}

	free(areas);
}

/* -------------------------------------------------------------------------- */

int
nvmm_init(void)
{
	if (nvmm_fd != -1)
		return 0;
	nvmm_fd = open("/dev/nvmm", O_RDONLY | O_CLOEXEC);
	if (nvmm_fd == -1)
		return -1;
	if (nvmm_capability(&__capability) == -1) {
		close(nvmm_fd);
		nvmm_fd = -1;
		return -1;
	}
	/* Allow backward compatibility */
	if (NVMM_USER_VERSION < __capability.version) {
		close(nvmm_fd);
		nvmm_fd = -1;
		errno = EPROGMISMATCH;
		return -1;
	}

	return 0;
}

int
nvmm_root_init(void)
{
	if (nvmm_fd != -1)
		return 0;
	nvmm_fd = open("/dev/nvmm", O_WRONLY | O_CLOEXEC);
	if (nvmm_fd == -1)
		return -1;
	if (nvmm_capability(&__capability) == -1) {
		close(nvmm_fd);
		nvmm_fd = -1;
		return -1;
	}
	if (NVMM_USER_VERSION < __capability.version) {
		close(nvmm_fd);
		nvmm_fd = -1;
		errno = EPROGMISMATCH;
		return -1;
	}

	return 0;
}

int
nvmm_capability(struct nvmm_capability *cap)
{
	struct nvmm_ioc_capability args;
	struct nvmm_ioc_capability_v2 args_v2;

	if (ioctl(nvmm_fd, NVMM_IOC_CAPABILITY, &args) == -1) {
		/* Try v2 */
		if (ioctl(nvmm_fd, NVMM_IOC_CAPABILITY_V2, &args_v2) == -1)
			return -1;
		args.cap.version = args_v2.cap.version;
		args.cap.state_size = args_v2.cap.state_size;
		args.cap.max_machines = args_v2.cap.max_machines;
		args.cap.max_vcpus = args_v2.cap.max_vcpus;
		args.cap.max_ram = args_v2.cap.max_ram;
		/* Not available in v2 */
		args.cap.comm_size = NVMM_COMM_PAGE_SIZE;
	}

	memcpy(cap, &args.cap, sizeof(args.cap));

	return 0;
}

int
nvmm_machine_create(struct nvmm_machine *mach)
{
	struct nvmm_ioc_machine_create args;
	struct nvmm_comm_page **pages;
	area_list_t *areas;
	int ret;

	areas = calloc(1, sizeof(*areas));
	if (areas == NULL)
		return -1;

	pages = calloc(__capability.max_vcpus, sizeof(*pages));
	if (pages == NULL) {
		free(areas);
		return -1;
	}

	ret = ioctl(nvmm_fd, NVMM_IOC_MACHINE_CREATE, &args);
	if (ret == -1) {
		free(areas);
		free(pages);
		return -1;
	}

	LIST_INIT(areas);

	memset(mach, 0, sizeof(*mach));
	mach->machid = args.machid;
	mach->pages = pages;
	mach->areas = areas;

	return 0;
}

int
nvmm_machine_destroy(struct nvmm_machine *mach)
{
	struct nvmm_ioc_machine_destroy args;
	int ret;

	args.machid = mach->machid;

	ret = ioctl(nvmm_fd, NVMM_IOC_MACHINE_DESTROY, &args);
	if (ret == -1)
		return -1;

	__area_remove_all(mach);
	free(mach->pages);

	return 0;
}

int
nvmm_machine_configure(struct nvmm_machine *mach, uint64_t op, void *conf)
{
	struct nvmm_ioc_machine_configure args;
	int ret;

	args.machid = mach->machid;
	args.op = op;
	args.conf = conf;

	ret = ioctl(nvmm_fd, NVMM_IOC_MACHINE_CONFIGURE, &args);
	if (ret == -1)
		return -1;

	return 0;
}

int
nvmm_vcpu_create(struct nvmm_machine *mach, nvmm_cpuid_t cpuid,
    struct nvmm_vcpu *vcpu)
{
	struct nvmm_ioc_vcpu_create args;
	struct nvmm_comm_page *comm;
	int ret;

	args.machid = mach->machid;
	args.cpuid = cpuid;
	args.comm = NULL;

	if (__capability.version < 3)
		ret = ioctl(nvmm_fd, NVMM_IOC_VCPU_CREATE_V2, &args);
	else
		ret = ioctl(nvmm_fd, NVMM_IOC_VCPU_CREATE, &args);

	if (ret == -1)
		return -1;

	if (__capability.version < 3) {
		comm = mmap(NULL, NVMM_COMM_PAGE_SIZE, PROT_READ|PROT_WRITE,
			MAP_SHARED|MAP_FILE, nvmm_fd,
			NVMM_COMM_OFF(mach->machid, cpuid));
		if (comm == MAP_FAILED)
			return -1;
		args.comm = comm;

	}
	mach->pages[cpuid] = args.comm;

	vcpu->cpuid = cpuid;

	vcpu->state = &args.comm->state;
	vcpu->event = &args.comm->event;

	vcpu->stop = &args.comm->stop;

	vcpu->exit = malloc(sizeof(*vcpu->exit));

	return 0;
}

int
nvmm_vcpu_destroy(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu)
{
	struct nvmm_ioc_vcpu_destroy args;
	struct nvmm_comm_page *comm;
	int ret;

	args.machid = mach->machid;
	args.cpuid = vcpu->cpuid;

	ret = ioctl(nvmm_fd, NVMM_IOC_VCPU_DESTROY, &args);
	if (ret == -1)
		return -1;

	/*
	 * Need to unmap the comm page on the user side, because the
	 * kernel has no guarantee to get the correct address space to
	 * do the unmapping at the point of closing fd.
	 */
	comm = mach->pages[vcpu->cpuid];
	munmap(comm, __capability.comm_size);

	free(vcpu->exit);

	return 0;
}

int
nvmm_vcpu_configure(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu,
    uint64_t op, void *conf)
{
	struct nvmm_ioc_vcpu_configure args;
	int ret;

	switch (op) {
	case NVMM_VCPU_CONF_CALLBACKS:
		memcpy(&vcpu->cbs, conf, sizeof(vcpu->cbs));
		return 0;
	}

	args.machid = mach->machid;
	args.cpuid = vcpu->cpuid;
	args.op = op;
	args.conf = conf;

	ret = ioctl(nvmm_fd, NVMM_IOC_VCPU_CONFIGURE, &args);
	if (ret == -1)
		return -1;

	return 0;
}

int
nvmm_vcpu_setstate(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu,
    uint64_t flags)
{
	struct nvmm_comm_page *comm;

	comm = mach->pages[vcpu->cpuid];
	comm->state_commit |= flags;
	comm->state_cached |= flags;

	return 0;
}

int
nvmm_vcpu_getstate(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu,
    uint64_t flags)
{
	struct nvmm_ioc_vcpu_getstate args;
	struct nvmm_comm_page *comm;
	int ret;

	comm = mach->pages[vcpu->cpuid];

	if (__predict_true((flags & ~comm->state_cached) == 0)) {
		return 0;
	}
	comm->state_wanted = flags & ~comm->state_cached;

	args.machid = mach->machid;
	args.cpuid = vcpu->cpuid;

	ret = ioctl(nvmm_fd, NVMM_IOC_VCPU_GETSTATE, &args);
	if (ret == -1)
		return -1;

	return 0;
}

int
nvmm_vcpu_inject(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu)
{
	struct nvmm_comm_page *comm;

	comm = mach->pages[vcpu->cpuid];
	comm->event_commit = true;

	return 0;
}

int
nvmm_vcpu_run(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu)
{
	struct nvmm_ioc_vcpu_run args;
	int ret;

	args.machid = mach->machid;
	args.cpuid = vcpu->cpuid;
	memset(&args.exit, 0, sizeof(args.exit));

	ret = ioctl(nvmm_fd, NVMM_IOC_VCPU_RUN, &args);
	if (ret == -1)
		return -1;

	/* No comm support yet, just copy. */
	memcpy(vcpu->exit, &args.exit, sizeof(args.exit));

	return 0;
}

int
nvmm_vcpu_stop(struct nvmm_vcpu *vcpu)
{
	struct nvmm_ioc_vcpu_stop args;
	int err = 0;

	*vcpu->stop |= NVMM_VCPU_STOP;

	/* Only for nvmm v3 */
	if (__capability.version > 2 && (*vcpu->stop & NVMM_VCPU_RUNNING))
		err = ioctl(nvmm_fd, NVMM_IOC_VCPU_STOP, &args);

	return err;
}

int
nvmm_gpa_map(struct nvmm_machine *mach, uintptr_t hva, gpaddr_t gpa,
    size_t size, int prot)
{
	struct nvmm_ioc_gpa_map args;
	int ret;
#if NVMM_KERN_VERSION > 2
	const char *gpa_wired = getenv("NVMM_GPA_WIRED");
#endif

	ret = __area_add(mach, hva, gpa, size, prot);
	if (ret == -1)
		return -1;

	args.machid = mach->machid;
	args.hva = hva;
	args.gpa = gpa;
	args.size = size;
	args.prot = prot;
#if NVMM_KERN_VERSION > 2
	if (gpa_wired)
		args.wired = true;
	else
		args.wired = false;
#endif

	if (ioctl(nvmm_fd, NVMM_IOC_GPA_MAP, &args) == -1) {
		/* Can't recover. */
		abort();
	}

	return 0;
}

int
nvmm_gpa_unmap(struct nvmm_machine *mach, uintptr_t hva, gpaddr_t gpa,
    size_t size)
{
	struct nvmm_ioc_gpa_unmap args;
	int ret;

	ret = __area_delete(mach, hva, gpa, size);
	if (ret == -1)
		return -1;

	args.machid = mach->machid;
	args.gpa = gpa;
	args.size = size;

	if (ioctl(nvmm_fd, NVMM_IOC_GPA_UNMAP, &args) == -1) {
		/* Can't recover. */
		abort();
	}

	return 0;
}

int
nvmm_hva_map(struct nvmm_machine *mach, uintptr_t hva, size_t size)
{
	struct nvmm_ioc_hva_map args;

	args.machid = mach->machid;
	args.hva = hva;
	args.size = size;

	if (ioctl(nvmm_fd, NVMM_IOC_HVA_MAP, &args) == -1)
		return -1;

	return 0;
}

int
nvmm_hva_unmap(struct nvmm_machine *mach, uintptr_t hva, size_t size)
{
	struct nvmm_ioc_hva_unmap args;

	args.machid = mach->machid;
	args.hva = hva;
	args.size = size;

	if (ioctl(nvmm_fd, NVMM_IOC_HVA_UNMAP, &args) == -1)
		return -1;

	return 0;
}

/*
 * nvmm_gva_to_gpa(): architecture-specific.
 */

int
nvmm_gpa_to_hva(struct nvmm_machine *mach, gpaddr_t gpa, uintptr_t *hva,
    nvmm_prot_t *prot)
{
	area_list_t *areas = mach->areas;
	area_t *ent;

	LIST_FOREACH(ent, areas, list) {
		if (gpa >= ent->gpa && gpa < ent->gpa + ent->size) {
			*hva = ent->hva + (gpa - ent->gpa);
			*prot = ent->prot;
			return 0;
		}
	}

	errno = ENOENT;
	return -1;
}

/*
 * nvmm_assist_io(): architecture-specific.
 */

/*
 * nvmm_assist_mem(): architecture-specific.
 */

int
nvmm_ctl(int op, void *data, size_t size)
{
	struct nvmm_ioc_ctl args;

	args.op = op;
	args.data = data;
	args.size = size;

	if (ioctl(nvmm_fd, NVMM_IOC_CTL, &args) == -1)
		return -1;

	return 0;
}
