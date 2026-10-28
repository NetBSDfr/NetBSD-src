/*	$NetBSD: virtgpu_gem.c,v 1.3 2021/12/18 23:45:45 riastradh Exp $	*/

/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: virtgpu_gem.c,v 1.3 2021/12/18 23:45:45 riastradh Exp $");

#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>

#include "virtgpu_drv.h"

#ifdef __NetBSD__
/*
 * Map GEM objects into userland: NetBSD's UVM pager for a GEM object's
 * gemo_uvmobj installs per-page mappings of the object's backing pages.
 */
#include <uvm/uvm_extern.h>

#include <drm/drm_gem_shmem_helper.h>

static int
virtio_gpu_gem_uvm_fault(struct uvm_faultinfo *ufi, vaddr_t vaddr,
    struct vm_page **pps, int npages, int centeridx, vm_prot_t access_type,
    int flags)
{
	struct vm_map_entry *entry = ufi->entry;
	struct uvm_object *const uobj = entry->object.uvm_obj;
	struct drm_gem_object *const gem_obj = container_of(uobj,
	    struct drm_gem_object, gemo_uvmobj);
	struct drm_gem_shmem_object *const shmem =
	    to_drm_gem_shmem_obj(gem_obj);
	off_t curr_offset;
	vaddr_t curr_va;
	int lcv, retval;

	if (UVM_ET_ISCOPYONWRITE(entry))
		return EIO;

	/*
	 * The shmem pages are pinned for the lifetime of the resource
	 * backing (virtio_gpu_object_attach() through
	 * virtio_gpu_object_detach()), so no pinning is needed here.
	 */
	if (shmem->pages == NULL)
		return EIO;

	retval = 0;
	curr_offset = entry->offset + (vaddr - entry->start);
	curr_va = vaddr;
	for (lcv = 0; lcv < npages; lcv++, curr_offset += PAGE_SIZE,
	    curr_va += PAGE_SIZE) {
		const voff_t poff = atop(curr_offset);
		const struct vm_page *pg;
		vm_prot_t mapprot;

		if ((flags & PGO_ALLPAGES) == 0 && lcv != centeridx)
			continue;
		if (pps[lcv] == PGO_DONTCARE)
			continue;
		if (poff >= (gem_obj->size >> PAGE_SHIFT))
			return EIO;

		pg = (const struct vm_page *)shmem->pages[poff];
		if (pg == NULL)
			return EIO;

		mapprot = ufi->entry->protection;
		if (pmap_enter(ufi->orig_map->pmap, curr_va,
		    uvm_vm_page_to_phys(pg), mapprot,
		    PMAP_CANFAIL | mapprot) != 0) {
			pmap_update(ufi->orig_map->pmap);
			uvmfault_unlockall(ufi, ufi->entry->aref.ar_amap,
			    uobj);
			return ENOMEM;
		}
	}

	pmap_update(ufi->orig_map->pmap);
	uvmfault_unlockall(ufi, ufi->entry->aref.ar_amap, uobj);
	return retval;
}

const struct uvm_pagerops virtio_gpu_gem_uvm_ops = {
	.pgo_reference = drm_gem_pager_reference,
	.pgo_detach = drm_gem_pager_detach,
	.pgo_fault = virtio_gpu_gem_uvm_fault,
};
#endif /* __NetBSD__ */

int virtio_gpu_gem_create(struct drm_file *file,
			  struct drm_device *dev,
			  struct virtio_gpu_object_params *params,
			  struct drm_gem_object **obj_p,
			  uint32_t *handle_p)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_object *obj;
	int ret;
	u32 handle;

	ret = virtio_gpu_object_create(vgdev, params, &obj, NULL);
	if (ret < 0)
		return ret;

	ret = drm_gem_handle_create(file, &obj->base.base, &handle);
	if (ret) {
		drm_gem_object_release(&obj->base.base);
		return ret;
	}

	*obj_p = &obj->base.base;

	/* drop reference from allocate - handle holds it now */
	drm_gem_object_put_unlocked(&obj->base.base);

	*handle_p = handle;
	return 0;
}

int virtio_gpu_mode_dumb_create(struct drm_file *file_priv,
				struct drm_device *dev,
				struct drm_mode_create_dumb *args)
{
	struct drm_gem_object *gobj;
	struct virtio_gpu_object_params params = { 0 };
	int ret;
	uint32_t pitch;

	if (args->bpp != 32)
		return -EINVAL;

	pitch = args->width * 4;
	args->size = pitch * args->height;
#ifdef __NetBSD__	/* NetBSD sys/param.h ALIGN() takes one argument */
	args->size = PAGE_ALIGN(args->size);
#else
	args->size = ALIGN(args->size, PAGE_SIZE);
#endif

	params.format = virtio_gpu_translate_format(DRM_FORMAT_HOST_XRGB8888);
	params.width = args->width;
	params.height = args->height;
	params.size = args->size;
	params.dumb = true;
	ret = virtio_gpu_gem_create(file_priv, dev, &params, &gobj,
				    &args->handle);
	if (ret)
		goto fail;

	args->pitch = pitch;
	return ret;

fail:
	return ret;
}

int virtio_gpu_mode_dumb_mmap(struct drm_file *file_priv,
			      struct drm_device *dev,
			      uint32_t handle, uint64_t *offset_p)
{
	struct drm_gem_object *gobj;

	BUG_ON(!offset_p);
	gobj = drm_gem_object_lookup(file_priv, handle);
	if (gobj == NULL)
		return -ENOENT;
	*offset_p = drm_vma_node_offset_addr(&gobj->vma_node);
	drm_gem_object_put_unlocked(gobj);
	return 0;
}

int virtio_gpu_gem_object_open(struct drm_gem_object *obj,
			       struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;
	struct virtio_gpu_object_array *objs;

	if (!vgdev->has_virgl_3d)
		return 0;

	objs = virtio_gpu_array_alloc(1);
	if (!objs)
		return -ENOMEM;
	virtio_gpu_array_add_obj(objs, obj);

	virtio_gpu_cmd_context_attach_resource(vgdev, vfpriv->ctx_id,
					       objs);
	return 0;
}

void virtio_gpu_gem_object_close(struct drm_gem_object *obj,
				 struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;
	struct virtio_gpu_object_array *objs;

	if (!vgdev->has_virgl_3d)
		return;

	objs = virtio_gpu_array_alloc(1);
	if (!objs)
		return;
	virtio_gpu_array_add_obj(objs, obj);

	virtio_gpu_cmd_context_detach_resource(vgdev, vfpriv->ctx_id,
					       objs);
}

struct virtio_gpu_object_array *virtio_gpu_array_alloc(u32 nents)
{
	struct virtio_gpu_object_array *objs;
	size_t size = sizeof(*objs) + sizeof(objs->objs[0]) * nents;

	objs = kmalloc(size, GFP_KERNEL);
	if (!objs)
		return NULL;

	objs->nents = 0;
	objs->total = nents;
	return objs;
}

static void virtio_gpu_array_free(struct virtio_gpu_object_array *objs)
{
	kfree(objs);
}

struct virtio_gpu_object_array*
virtio_gpu_array_from_handles(struct drm_file *drm_file, u32 *handles, u32 nents)
{
	struct virtio_gpu_object_array *objs;
	u32 i;

	objs = virtio_gpu_array_alloc(nents);
	if (!objs)
		return NULL;

	for (i = 0; i < nents; i++) {
		objs->objs[i] = drm_gem_object_lookup(drm_file, handles[i]);
		if (!objs->objs[i]) {
			objs->nents = i;
			virtio_gpu_array_put_free(objs);
			return NULL;
		}
	}
	objs->nents = i;
	return objs;
}

void virtio_gpu_array_add_obj(struct virtio_gpu_object_array *objs,
			      struct drm_gem_object *obj)
{
	if (WARN_ON_ONCE(objs->nents == objs->total))
		return;

	drm_gem_object_get(obj);
	objs->objs[objs->nents] = obj;
	objs->nents++;
}

int virtio_gpu_array_lock_resv(struct virtio_gpu_object_array *objs)
{
	int ret;

	if (objs->nents == 1) {
		ret = dma_resv_lock_interruptible(objs->objs[0]->resv, NULL);
	} else {
		ret = drm_gem_lock_reservations(objs->objs, objs->nents,
						&objs->ticket);
	}
	return ret;
}

void virtio_gpu_array_unlock_resv(struct virtio_gpu_object_array *objs)
{
	if (objs->nents == 1) {
		dma_resv_unlock(objs->objs[0]->resv);
	} else {
		drm_gem_unlock_reservations(objs->objs, objs->nents,
					    &objs->ticket);
	}
}

void virtio_gpu_array_add_fence(struct virtio_gpu_object_array *objs,
				struct dma_fence *fence)
{
	int i;

	for (i = 0; i < objs->nents; i++)
		dma_resv_add_excl_fence(objs->objs[i]->resv, fence);
}

void virtio_gpu_array_put_free(struct virtio_gpu_object_array *objs)
{
	u32 i;

	for (i = 0; i < objs->nents; i++)
		drm_gem_object_put_unlocked(objs->objs[i]);
	virtio_gpu_array_free(objs);
}

void virtio_gpu_array_put_free_delayed(struct virtio_gpu_device *vgdev,
				       struct virtio_gpu_object_array *objs)
{
	spin_lock(&vgdev->obj_free_lock);
	list_add_tail(&objs->next, &vgdev->obj_free_list);
	spin_unlock(&vgdev->obj_free_lock);
	schedule_work(&vgdev->obj_free_work);
}

void virtio_gpu_array_put_free_work(struct work_struct *work)
{
	struct virtio_gpu_device *vgdev =
		container_of(work, struct virtio_gpu_device, obj_free_work);
	struct virtio_gpu_object_array *objs;

	spin_lock(&vgdev->obj_free_lock);
	while (!list_empty(&vgdev->obj_free_list)) {
		objs = list_first_entry(&vgdev->obj_free_list,
					struct virtio_gpu_object_array, next);
		list_del(&objs->next);
		spin_unlock(&vgdev->obj_free_lock);
		virtio_gpu_array_put_free(objs);
		spin_lock(&vgdev->obj_free_lock);
	}
	spin_unlock(&vgdev->obj_free_lock);
}
