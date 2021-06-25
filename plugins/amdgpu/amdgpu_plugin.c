#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/limits.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>

#include <xf86drm.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>

#include "criu-plugin.h"
#include "criu-amdgpu.pb-c.h"

#include "kfd_ioctl.h"
#include "xmalloc.h"
#include "criu-log.h"

#include "common/list.h"
#include "amdgpu_plugin_topology.h"

#define DRM_FIRST_RENDER_NODE 128
#define DRM_LAST_RENDER_NODE 255

#define PROCPIDMEM      "/proc/%d/mem"
#define HSAKMT_SHM_PATH "/dev/shm/hsakmt_shared_mem"
#define HSAKMT_SHM      "/hsakmt_shared_mem"
#define HSAKMT_SEM_PATH "/dev/shm/sem.hsakmt_semaphore"
#define HSAKMT_SEM      "hsakmt_semaphore"


#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef LOG_PREFIX
#undef LOG_PREFIX
#endif
#define LOG_PREFIX "amdgpu_plugin: "

#ifdef DEBUG
#define plugin_log_msg(fmt, ...) pr_debug(fmt, ##__VA_ARGS__)
#else
#define plugin_log_msg(fmt, ...) {}
#endif

#define SDMA_PACKET(op, sub_op, e)	((((e) & 0xFFFF) << 16) |       \
					 (((sub_op) & 0xFF) << 8) |     \
					 (((op) & 0xFF) << 0))

#define SDMA_OPCODE_COPY		1
#define SDMA_COPY_SUB_OPCODE_LINEAR	0
#define SDMA_OPCODE_WRITE		2
#define SDMA_WRITE_SUB_OPCODE_LINEAR	0
#define SDMA_NOP			0
#define SDMA_LINEAR_COPY_MAX_SIZE	(1ULL << 21)
#define TIMEOUT_NS			(1ULL << 10)    /* CS timeout instead of AMDGPU_TIMEOUT_INFINITE */

enum sdma_op_type {
	SDMA_OP_VRAM_READ,
	SDMA_OP_VRAM_WRITE,
};

struct vma_metadata {
	struct list_head list;
	uint64_t old_pgoff;
	uint64_t new_pgoff;
	uint64_t vma_entry;
	uint32_t new_minor;
};

/************************************ Global Variables ********************************************/
struct tp_system src_topology;
struct tp_system dest_topology;

struct device_maps checkpoint_maps;
struct device_maps restore_maps;

static LIST_HEAD(update_vma_info_list);

extern bool kfd_fw_version_check;
extern bool kfd_sdma_fw_version_check;
extern bool kfd_caches_count_check;
extern bool kfd_num_gws_check;
extern bool kfd_vram_size_check;
extern bool kfd_numa_check;

/**************************************************************************************************/

int open_drm_render_device(int minor)
{
	char path[128];
	int fd;

	if (minor < DRM_FIRST_RENDER_NODE || minor > DRM_LAST_RENDER_NODE) {
		pr_perror("DRM render minor %d out of range [%d, %d]\n", minor,
			  DRM_FIRST_RENDER_NODE, DRM_LAST_RENDER_NODE);
		return -EINVAL;
	}

	sprintf(path, "/dev/dri/renderD%d", minor);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		if (errno != ENOENT && errno != EPERM) {
			pr_err("Failed to open %s: %s\n", path, strerror(errno));
			if (errno == EACCES)
				pr_err("Check user is in \"video\" group\n");
		}
		return -EBADFD;
	}

	return fd;
}

int write_file(const char *file_path, const void *buf, const size_t buf_len)
{
	int fd;
	FILE *fp;
	size_t len_wrote;

	fd = openat(criu_get_image_dir(), file_path, O_WRONLY | O_CREAT, 0600);
	if (fd < 0) {
		pr_perror("Cannot open %s", file_path);
		return -errno;
	}

	fp = fdopen(fd, "w");
	if (!fp) {
		pr_perror("Cannot fdopen %s", file_path);
		return -errno;
	}

	len_wrote = fwrite(buf, 1, buf_len, fp);
	if (len_wrote != buf_len) {
		pr_perror("Unable to write %s (wrote:%ld buf_len:%ld)\n", file_path, len_wrote, buf_len);
		fclose(fp);
		return -EIO;
	}

	pr_info("Wrote file:%s (%ld bytes)\n", file_path, buf_len);
	/* this will also close fd */
	fclose(fp);
	return 0;
}

int read_file(const char *file_path, void *buf, const size_t buf_len)
{
	int fd;
	FILE *fp;
	size_t len_read;

	fd = openat(criu_get_image_dir(), file_path, O_RDONLY);
	if (fd < 0) {
		pr_perror("Cannot open %s", file_path);
		return -errno;
	}

	fp = fdopen(fd, "r");
	if (!fp) {
		pr_perror("Cannot fdopen %s", file_path);
		return -errno;
	}

	len_read = fread(buf, 1, buf_len, fp);
	if (len_read != buf_len) {
		pr_perror("Unable to read %s\n", file_path);
		fclose(fp);
		return -EIO;
	}

	pr_info("Read file:%s (%ld bytes)\n", file_path, buf_len);

	/* this will also close fd */
	fclose(fp);
	return 0;
}

/* Call ioctl, restarting if it is interrupted */
int kmtIoctl(int fd, unsigned long request, void *arg)
{
        int ret;

        do {
                ret = ioctl(fd, request, arg);
        } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

        if (ret == -1 && errno == EBADF)
		/* In case pthread_atfork didn't catch it, this will
                 * make any subsequent hsaKmt calls fail in CHECK_KFD_OPEN.
                 */
		pr_perror("KFD file descriptor not valid in this process\n");
	return ret;
}

static void free_e(CriuKfd *e)
{
	for (int i = 0; i < e->n_bo_info_test; i++) {
		if (e->bo_info_test[i]->bo_rawdata.data)
			xfree(e->bo_info_test[i]->bo_rawdata.data);
		if (e->bo_info_test[i])
			xfree(e->bo_info_test[i]);
	}
	for (int i = 0; i < e->n_devinfo_entries; i++) {
		if (e->devinfo_entries[i]) {
			for (int j = 0; j < e->devinfo_entries[i]->n_iolinks; j++)
				xfree(e->devinfo_entries[i]->iolinks[j]);

			xfree(e->devinfo_entries[i]);
		}
	}
	for (int i = 0; i < e->n_q_entries; i++) {
		if (e->q_entries[i])
			xfree(e->q_entries[i]);
	}
	for (int i = 0; i < e->n_ev_entries; i++) {
		if (e->ev_entries[i])
			xfree(e->ev_entries[i]);
	}
	xfree(e);
}

static int allocate_devinfo_entries(CriuKfd *e, int num_of_devices)
{
	e->devinfo_entries = xmalloc(sizeof(DevinfoEntry*) * num_of_devices);
	if (!e->devinfo_entries) {
		pr_err("Failed to allocate devinfo_entries\n");
		return -1;
	}

	for (int i = 0; i < num_of_devices; i++)
	{
		DevinfoEntry *entry = xmalloc(sizeof(DevinfoEntry));
		if (!entry) {
			pr_err("Failed to allocate entry\n");
			return -ENOMEM;
		}

		devinfo_entry__init(entry);

		e->devinfo_entries[i] = entry;
		e->n_devinfo_entries++;

	}
	return 0;
}

static int allocate_bo_info_test(CriuKfd *e, int num_bos, struct kfd_criu_bo_buckets *bo_bucket_ptr)
{
	e->bo_info_test = xmalloc(sizeof(BoEntriesTest*) * num_bos);
	if (!e->bo_info_test) {
		pr_err("Failed to allocate bo_info\n");
		return -ENOMEM;
	}

	pr_info("Inside allocate_bo_info_test\n");
	for (int i = 0; i < num_bos; i++)
	{
		BoEntriesTest *botest;
		botest = xmalloc(sizeof(*botest));
		if (!botest) {
			pr_err("Failed to allocate botest\n");
			return -ENOMEM;
		}

		bo_entries_test__init(botest);

		if ((bo_bucket_ptr)[i].bo_alloc_flags &
		    KFD_IOC_ALLOC_MEM_FLAGS_VRAM ||
		    (bo_bucket_ptr)[i].bo_alloc_flags &
		    KFD_IOC_ALLOC_MEM_FLAGS_GTT) {
			botest->bo_rawdata.data = xmalloc((bo_bucket_ptr)[i].bo_size);
			botest->bo_rawdata.len = (bo_bucket_ptr)[i].bo_size;
		}

		e->bo_info_test[i] = botest;
		e->n_bo_info_test++;

	}

	return 0;
}

static int allocate_q_entries(CriuKfd *e, int num_queues)
{
	e->q_entries = xmalloc(sizeof(QEntry*) * num_queues);
	if (!e->q_entries) {
		pr_err("Failed to allocate q_entries\n");
		return -1;
	}

	for (int i = 0; i < num_queues; i++) {
		QEntry *q_entry = xmalloc(sizeof(QEntry));
		if (!q_entry) {
			pr_err("Failed to allocate q_entry\n");
			return -ENOMEM;
		}
		q_entry__init(q_entry);

		e->q_entries[i] = q_entry;
		e->n_q_entries++;

	}
	return 0;
}

static int allocate_ev_entries(CriuKfd *e, int num_events)
{
	e->ev_entries = xmalloc(sizeof(EvEntry*) * num_events);
	if (!e->ev_entries) {
		pr_err("Failed to allocate ev_entries\n");
		return -1;
	}

	for (int i = 0; i < num_events; i++) {
		EvEntry *ev_entry = xmalloc(sizeof(EvEntry));
		if (!ev_entry) {
			pr_err("Failed to allocate ev_entry\n");
			return -ENOMEM;
		}
		ev_entry__init(ev_entry);
		e->ev_entries[i] = ev_entry;
		e->n_ev_entries++;

	}
	e->num_of_events = num_events;
	return 0;
}

int topology_to_devinfo(struct tp_system *sys, struct kfd_criu_devinfo_bucket *devinfo_buckets,
			struct device_maps *maps, DevinfoEntry **devinfos)
{
	struct tp_node *node;
	uint32_t devinfo_index = 0;

	list_for_each_entry(node, &sys->nodes, listm_system) {
		DevinfoEntry *devinfo = devinfos[devinfo_index++];

		devinfo->node_id = node->id;

		if (NODE_IS_GPU(node)) {
			devinfo->gpu_id = maps_get_dest_gpu(&checkpoint_maps, node->gpu_id);
			if (!devinfo->gpu_id)
				return -EINVAL;

			devinfo->simd_count = node->simd_count;
			devinfo->mem_banks_count = node->mem_banks_count;
			devinfo->caches_count = node->caches_count;
			devinfo->io_links_count = node->io_links_count;
			devinfo->max_waves_per_simd = node->max_waves_per_simd;
			devinfo->lds_size_in_kb = node->lds_size_in_kb;
			devinfo->num_gws = node->num_gws;
			devinfo->wave_front_size = node->wave_front_size;
			devinfo->array_count = node->array_count;
			devinfo->simd_arrays_per_engine = node->simd_arrays_per_engine;
			devinfo->cu_per_simd_array = node->cu_per_simd_array;
			devinfo->simd_per_cu = node->simd_per_cu;
			devinfo->max_slots_scratch_cu = node->max_slots_scratch_cu;
			devinfo->vendor_id = node->vendor_id;
			devinfo->device_id = node->device_id;
			devinfo->domain = node->domain;
			devinfo->drm_render_minor = node->drm_render_minor;
			devinfo->hive_id = node->hive_id;
			devinfo->num_sdma_engines = node->num_sdma_engines;
			devinfo->num_sdma_xgmi_engines = node->num_sdma_xgmi_engines;
			devinfo->num_sdma_queues_per_engine = node->num_sdma_queues_per_engine;
			devinfo->num_cp_queues = node->num_cp_queues;
			devinfo->fw_version = node->fw_version;
			devinfo->capability = node->capability;
			devinfo->sdma_fw_version = node->sdma_fw_version;
			devinfo->vram_public = node->vram_public;
			devinfo->vram_size = node->vram_size;
		} else {
			devinfo->cpu_cores_count = node->cpu_cores_count;
		}

		if (node->num_valid_iolinks) {
			struct tp_iolink *iolink;
			uint32_t iolink_index = 0;
			devinfo->iolinks = xmalloc(sizeof(DevIolink*) * node->num_valid_iolinks);
			if (!devinfo->iolinks)
				return -ENOMEM;

			list_for_each_entry(iolink, &node->iolinks, listm) {
				if (!iolink->valid)
					continue;

				devinfo->iolinks[iolink_index] = xmalloc(sizeof(DevIolink));
				if (!devinfo->iolinks[iolink_index])
					return -ENOMEM;

				dev_iolink__init(devinfo->iolinks[iolink_index]);

				devinfo->iolinks[iolink_index]->type = iolink->type;
				devinfo->iolinks[iolink_index]->node_to_id = iolink->node_to_id;
				iolink_index++;
			}
			devinfo->n_iolinks = iolink_index;
		}
	}
	return 0;
}

int devinfo_to_topology(DevinfoEntry *devinfos[], uint32_t num_devices, struct tp_system *sys)
{
	for (int i = 0; i < num_devices; i++) {
		struct tp_node *node;
		DevinfoEntry *devinfo = devinfos[i];

		node = sys_add_node(sys, devinfo->node_id, devinfo->gpu_id);
		if (!node)
			return -ENOMEM;

		if (devinfo->cpu_cores_count) {
			node->cpu_cores_count = devinfo->cpu_cores_count;
		} else {
			node->simd_count = devinfo->simd_count;
			node->mem_banks_count = devinfo->mem_banks_count;
			node->caches_count = devinfo->caches_count;
			node->io_links_count = devinfo->io_links_count;
			node->max_waves_per_simd = devinfo->max_waves_per_simd;
			node->lds_size_in_kb = devinfo->lds_size_in_kb;
			node->num_gws = devinfo->num_gws;
			node->wave_front_size = devinfo->wave_front_size;
			node->array_count = devinfo->array_count;
			node->simd_arrays_per_engine = devinfo->simd_arrays_per_engine;
			node->cu_per_simd_array = devinfo->cu_per_simd_array;
			node->simd_per_cu = devinfo->simd_per_cu;
			node->max_slots_scratch_cu = devinfo->max_slots_scratch_cu;
			node->vendor_id = devinfo->vendor_id;
			node->device_id = devinfo->device_id;
			node->domain = devinfo->domain;
			node->drm_render_minor = devinfo->drm_render_minor;
			node->hive_id = devinfo->hive_id;
			node->num_sdma_engines = devinfo->num_sdma_engines;
			node->num_sdma_xgmi_engines = devinfo->num_sdma_xgmi_engines;
			node->num_sdma_queues_per_engine = devinfo->num_sdma_queues_per_engine;
			node->num_cp_queues = devinfo->num_cp_queues;
			node->fw_version = devinfo->fw_version;
			node->capability = devinfo->capability;
			node->sdma_fw_version = devinfo->sdma_fw_version;
			node->vram_public = devinfo->vram_public;
			node->vram_size = devinfo->vram_size;
		}

		for (int j = 0; j < devinfo->n_iolinks; j++) {
			struct tp_iolink *iolink;
			DevIolink *devlink = (devinfo->iolinks[j]);

			iolink = node_add_iolink(node, devlink->type, devlink->node_to_id);
			if (!iolink)
				return -ENOMEM;

		}
	}
	return 0;
}

void getenv_bool(const char *var, bool *value)
{
	char *value_str = getenv(var);
	if (value_str) {
		if (!strcmp(value_str, "0") || !strcasecmp(value_str, "NO"))
			*value = false;
		else if (!strcmp(value_str, "1") || !strcasecmp(value_str, "YES"))
			*value = true;
		else
			pr_err("Ignoring invalid value for %s=%s, expecting (YES/NO)\n",
				var, value_str);
	}
	pr_info("param: %s:%s", var, *value ? "Y" : "N");
	return;
}

int amdgpu_plugin_init(int stage)
{
	pr_info("amdgpu_plugin: initialized:  %s (AMDGPU/KFD)\n",
						CR_PLUGIN_DESC.name);

	topology_init(&src_topology);
	topology_init(&dest_topology);
	maps_init(&checkpoint_maps);
	maps_init(&restore_maps);

	if (stage == CR_PLUGIN_STAGE__RESTORE) {
		/* Default Values */
		kfd_fw_version_check = true;
		kfd_sdma_fw_version_check = true;
		kfd_caches_count_check = true;
		kfd_num_gws_check = true;
		kfd_vram_size_check = true;
		kfd_numa_check = true;

		getenv_bool("KFD_FW_VER_CHECK", &kfd_fw_version_check);
		getenv_bool("KFD_SDMA_FW_VER_CHECK", &kfd_sdma_fw_version_check);
		getenv_bool("KFD_CACHES_COUNT_CHECK", &kfd_caches_count_check);
		getenv_bool("KFD_NUM_GWS_CHECK", &kfd_num_gws_check);
		getenv_bool("KFD_VRAM_SIZE_CHECK", &kfd_vram_size_check);
		getenv_bool("KFD_NUMA_CHECK", &kfd_numa_check);
	}
	return 0;
}

void amdgpu_plugin_fini(int stage, int ret)
{
	pr_info("amdgpu_plugin: finished  %s (AMDGPU/KFD)\n", CR_PLUGIN_DESC.name);

	maps_free(&checkpoint_maps);
	maps_free(&restore_maps);

	topology_free(&src_topology);
	topology_free(&dest_topology);
}

CR_PLUGIN_REGISTER("amdgpu_plugin", amdgpu_plugin_init, amdgpu_plugin_fini)

struct thread_data {
	pthread_t thread;
	uint64_t num_of_bos;
	uint32_t gpu_id;
	pid_t pid;
	struct kfd_criu_bo_buckets *bo_buckets;
	BoEntriesTest **bo_info_test;
	__u64 *restored_bo_offsets;
	int drm_fd;
	int ret;
	amdgpu_device_handle h_dev;
};

int alloc_and_map(amdgpu_device_handle h_dev, uint64_t size, uint32_t domain,
		  amdgpu_bo_handle *ph_bo, amdgpu_va_handle *ph_va,
		  uint64_t *p_gpu_addr, void **p_cpu_addr)
{
	struct amdgpu_bo_alloc_request alloc_req;
	amdgpu_bo_handle h_bo;
	amdgpu_va_handle h_va;
	uint64_t gpu_addr;
	void *cpu_addr;
	int err;

	memset(&alloc_req, 0, sizeof(alloc_req));
	alloc_req.alloc_size = size;
	alloc_req.phys_alignment = 0x1000;
	alloc_req.preferred_heap = domain;
	alloc_req.flags = 0;
	err = amdgpu_bo_alloc(h_dev, &alloc_req, &h_bo);
	if (err) {
		pr_perror("failed to alloc BO");
		return err;
	}
	err = amdgpu_va_range_alloc(h_dev, amdgpu_gpu_va_range_general,
				    size, 0x1000, 0, &gpu_addr, &h_va, 0);
	if (err) {
		pr_perror("failed to alloc VA");
		goto err_va;
	}
	err = amdgpu_bo_va_op(h_bo, 0, size, gpu_addr, 0, AMDGPU_VA_OP_MAP);
	if (err) {
		pr_perror("failed to GPU map BO");
		goto err_gpu_map;
	}
	if (p_cpu_addr) {
		err = amdgpu_bo_cpu_map(h_bo, &cpu_addr);
		if (err) {
			pr_perror("failed to CPU map BO");
			goto err_cpu_map;
		}
		*p_cpu_addr = cpu_addr;
	}

	*ph_bo = h_bo;
	*ph_va = h_va;
	*p_gpu_addr = gpu_addr;

	return 0;

err_cpu_map:
	amdgpu_bo_va_op(h_bo, 0, size, gpu_addr, 0, AMDGPU_VA_OP_UNMAP);
err_gpu_map:
	amdgpu_va_range_free(h_va);
err_va:
	amdgpu_bo_free(h_bo);
	return err;
}

void free_and_unmap(uint64_t size, amdgpu_bo_handle h_bo,
		    amdgpu_va_handle h_va, uint64_t gpu_addr, void *cpu_addr)
{
	if (cpu_addr)
		amdgpu_bo_cpu_unmap(h_bo);
	amdgpu_bo_va_op(h_bo, 0, size, gpu_addr, 0, AMDGPU_VA_OP_UNMAP);
	amdgpu_va_range_free(h_va);
	amdgpu_bo_free(h_bo);
}

int attempt_sdma_read_write(struct kfd_criu_bo_buckets *bo_buckets,
		      BoEntriesTest **bo_info_test, int i,
		      amdgpu_device_handle h_dev, enum sdma_op_type type)
{
	uint64_t size, gpu_addr_src, gpu_addr_dest, gpu_addr_ib;
	uint64_t gpu_addr_src_orig, gpu_addr_dest_orig;
	amdgpu_va_handle h_va_src, h_va_dest, h_va_ib;
	amdgpu_bo_handle h_bo_src, h_bo_dest, h_bo_ib;
	struct amdgpu_bo_import_result res = {0};
	uint64_t copy_size, bytes_remain, j =0;
	struct amdgpu_gpu_info gpu_info = {0};
	struct amdgpu_cs_ib_info ib_info;
	amdgpu_bo_list_handle h_bo_list;
	struct amdgpu_cs_request cs_req;
	amdgpu_bo_handle resources[3];
	struct amdgpu_cs_fence fence;
	uint32_t family_id, expired;
	amdgpu_context_handle h_ctx;
	void *userptr = NULL;
	uint32_t *ib = NULL;
	int err, shared_fd;

	err = amdgpu_query_gpu_info(h_dev, &gpu_info);
	if (err) {
		pr_perror("failed to query gpuinfo via libdrm");
		return -EINVAL;
	}

	family_id = gpu_info.family_id;
	pr_info("libdrm initialized successfully, family: = %d\n", family_id);

	shared_fd = bo_buckets[i].dmabuf_fd;
	size = bo_buckets[i].bo_size;

	/* prepare src buffer */
	if (type == SDMA_OP_VRAM_WRITE)
	{
		/* create the userptr BO and prepare the src buffer */

		posix_memalign(&userptr, sysconf(_SC_PAGE_SIZE), size);
		if (!userptr) {
			pr_perror("failed to alloc memory for userptr");
			return -ENOMEM;
		}

		memcpy(userptr, bo_info_test[i]->bo_rawdata.data, size);
		pr_info("data copied to userptr from protobuf buffer\n");

		err = amdgpu_create_bo_from_user_mem(h_dev, userptr, size,
						     &h_bo_src);
		if (err) {
			pr_perror("failed to create userptr for sdma");
			free(userptr);
			return -EFAULT;
		}

	} else if (type == SDMA_OP_VRAM_READ) {
		err = amdgpu_bo_import(h_dev, amdgpu_bo_handle_type_dma_buf_fd,
				       shared_fd, &res);
		if (err) {
			pr_perror("failed to import dmabuf handle from libdrm");
			return -EFAULT;
		}

		h_bo_src = res.buf_handle;
		pr_info("closing src fd %d\n", shared_fd);
		close(shared_fd);

	} else {
		pr_perror("Invalid sdma operation");
		return -EINVAL;
	}

	err = amdgpu_va_range_alloc(h_dev, amdgpu_gpu_va_range_general,
				    size, 0x1000, 0, &gpu_addr_src,
				    &h_va_src, 0);
	if (err) {
		pr_perror("failed to alloc VA for src bo");
		goto err_src_va;
	}
	err = amdgpu_bo_va_op(h_bo_src, 0, size, gpu_addr_src, 0,
			      AMDGPU_VA_OP_MAP);
	if (err) {
		pr_perror("failed to GPU map the src BO");
		goto err_src_bo_map;
	}
	pr_info("Source BO: GPU VA: %lx, size: %lx\n", gpu_addr_src, size);

	/* prepare dest buffer */
	if (type == SDMA_OP_VRAM_WRITE) {
		err = amdgpu_bo_import(h_dev, amdgpu_bo_handle_type_dma_buf_fd,
				       shared_fd, &res);
		if (err) {
			pr_perror("failed to import dmabuf handle from libdrm");
			goto err_dest_bo_prep;
		}

		h_bo_dest = res.buf_handle;
		pr_info("closing dest fd %d\n", shared_fd);
		close(shared_fd);
	} else if (type == SDMA_OP_VRAM_READ) {
		posix_memalign(&userptr, sysconf(_SC_PAGE_SIZE), size);
		if (!userptr) {
			pr_perror("failed to alloc memory for userptr");
			goto err_dest_bo_prep;
		}
		memset(userptr, 0, size);
		err = amdgpu_create_bo_from_user_mem(h_dev, userptr, size,
						     &h_bo_dest);
		if (err) {
			pr_perror("failed to create userptr for sdma");
			free(userptr);
			goto err_dest_bo_prep;
		}
	} else {
		pr_perror("Invalid sdma operation");
		goto err_dest_bo_prep;
	}
	err = amdgpu_va_range_alloc(h_dev, amdgpu_gpu_va_range_general,
				    size, 0x1000, 0, &gpu_addr_dest,
				    &h_va_dest, 0);
	if (err) {
		pr_perror("failed to alloc VA for dest bo");
		goto err_dest_va;
	}
	err = amdgpu_bo_va_op(h_bo_dest, 0, size, gpu_addr_dest, 0,
			      AMDGPU_VA_OP_MAP);
	if (err) {
		pr_perror("failed to GPU map the dest BO");
		goto err_dest_bo_map;
	}
	pr_info("Dest BO: GPU VA: %lx, size: %lx\n", gpu_addr_dest, size);

	/* prepare ring buffer/indirect buffer for command submission */
	err = alloc_and_map(h_dev, 16384, AMDGPU_GEM_DOMAIN_GTT, &h_bo_ib,
			    &h_va_ib, &gpu_addr_ib, (void **)&ib);
	if (err) {
		pr_perror("failed to allocate and map ib/rb");
		goto err_ib_gpu_alloc;
	}
	pr_info("Indirect ring BO: GPU VA: %lx, size: 16384\n", gpu_addr_ib);

	resources[0] = h_bo_src;
	resources[1] = h_bo_dest;
	resources[2] = h_bo_ib;
	err = amdgpu_bo_list_create(h_dev, 3, resources, NULL, &h_bo_list);
	if (err) {
		pr_perror("failed to create BO resources list");
		goto err_bo_list;
	}

	memset(&cs_req, 0, sizeof(cs_req));
	memset(&fence, 0, sizeof(fence));
	memset(ib, 0, 16384);

	pr_info("setting up sdma packets for command submission\n");
	bytes_remain = size;
	gpu_addr_src_orig = gpu_addr_src;
	gpu_addr_dest_orig = gpu_addr_dest;
	while (bytes_remain > 0) {

		 if (bytes_remain > SDMA_LINEAR_COPY_MAX_SIZE)
			 copy_size = SDMA_LINEAR_COPY_MAX_SIZE;
		 else
			 copy_size = bytes_remain;

		 ib[j++] = SDMA_PACKET(SDMA_OPCODE_COPY,
				       SDMA_COPY_SUB_OPCODE_LINEAR, 0);
		 if (family_id >= AMDGPU_FAMILY_AI)
			 ib[j++] = copy_size - 1;
		 else
			 ib[j++] = copy_size;

		 ib[j++] = 0;
		 ib[j++] = 0xffffffff & gpu_addr_src;
		 ib[j++] = (0xffffffff00000000 & gpu_addr_src) >> 32;
		 ib[j++] = 0xffffffff & gpu_addr_dest;
		 ib[j++] = (0xffffffff00000000 & gpu_addr_dest) >> 32;

		 gpu_addr_src += copy_size;
		 gpu_addr_dest += copy_size;
		 bytes_remain -= copy_size;
	 }
	gpu_addr_src = gpu_addr_src_orig;
	gpu_addr_dest = gpu_addr_dest_orig;
	pr_info("pad the IB to aling on 8 dw boundary\n");
	 /* pad the IB to the required number of dw with SDMA_NOP */
	while (j & 7)
		 ib[j++] =  SDMA_NOP;

	 ib_info.ib_mc_address = gpu_addr_ib;
	 ib_info.size = j;

	 cs_req.ip_type = AMDGPU_HW_IP_DMA;
	 /* possible future optimization: may use other rings, info available in
	  * amdgpu_query_hw_ip_info()
	  */
	 cs_req.ring = 0;
	 cs_req.number_of_ibs = 1;
	 cs_req.ibs = &ib_info;
	 cs_req.resources = h_bo_list;
	 cs_req.fence_info.handle = NULL;

	 pr_info("create the context\n");
	 err = amdgpu_cs_ctx_create(h_dev, &h_ctx);
	 if (err) {
		 pr_perror("failed to create context for SDMA command submission");
		 goto err_ctx;
	 }

	 pr_info("initiate sdma command submission\n");
	 err = amdgpu_cs_submit(h_ctx, 0, &cs_req, 1);
	 if (err) {
		 pr_perror("failed to submit command for SDMA IB");
		 goto err_cs_submit_ib;
	 }

	 fence.context = h_ctx;
	 fence.ip_type = AMDGPU_HW_IP_DMA;
	 fence.ip_instance = 0;
	 fence.ring = 0;
	 fence.fence = cs_req.seq_no;
	 err = amdgpu_cs_query_fence_status(&fence, AMDGPU_TIMEOUT_INFINITE, 0,
					    &expired);
	 if (err) {
		 pr_perror("failed to query fence status");
		 goto err_cs_submit_ib;
	 }

	 if (!expired) {
		 pr_err("IB execution did not complete");
		 err = -EBUSY;
		 goto err_cs_submit_ib;
	 }

	 pr_info("done querying fence status\n");

	 if (type == SDMA_OP_VRAM_READ) {
		 memcpy(bo_info_test[i]->bo_rawdata.data, userptr, size);
		 pr_info("data copied to protobuf buffer\n");
	 }

err_cs_submit_ib:
	 amdgpu_cs_ctx_free(h_ctx);
err_ctx:
	amdgpu_bo_list_destroy(h_bo_list);
err_bo_list:
	free_and_unmap(16384, h_bo_ib, h_va_ib, gpu_addr_ib, ib);
err_ib_gpu_alloc:
	err = amdgpu_bo_va_op(h_bo_dest, 0, size, gpu_addr_dest, 0,
			AMDGPU_VA_OP_UNMAP);
	if (err) {
		pr_perror("failed to GPU unmap the dest BO %lx, size = %lx",
			  gpu_addr_dest, size);
	}
err_dest_bo_map:
	err = amdgpu_va_range_free(h_va_dest);
	if (err) {
		pr_perror("dest range free failed");
	}
err_dest_va:
	err = amdgpu_bo_free(h_bo_dest);
	if (err) {
		pr_perror("dest bo free failed");
	}

	if (userptr && (type == SDMA_OP_VRAM_READ)) {
		free(userptr);
		userptr = NULL;
	}

err_dest_bo_prep:
	err = amdgpu_bo_va_op(h_bo_src, 0, size, gpu_addr_src, 0,
			      AMDGPU_VA_OP_UNMAP);
	if (err) {
		pr_perror("failed to GPU unmap the src BO %lx, size = %lx",
			  gpu_addr_src, size);
	}
err_src_bo_map:
	err = amdgpu_va_range_free(h_va_src);
	if (err) {
		pr_perror("src range free failed");
	}
err_src_va:
	err = amdgpu_bo_free(h_bo_src);
	if (err) {
		pr_perror("src bo free failed");
	}

	if (userptr && (type == SDMA_OP_VRAM_WRITE)) {
		free(userptr);
		userptr = NULL;
	}

	pr_info("Leaving attempt_sdma_read_write, err = %d\n", err);
	return err;
}

void *dump_bo_contents(void *_thread_data)
{
	int i, ret = 0;
	int num_bos = 0;
	struct thread_data* thread_data = (struct thread_data*) _thread_data;
	struct kfd_criu_bo_buckets *bo_buckets = thread_data->bo_buckets;
	BoEntriesTest **bo_info_test = thread_data->bo_info_test;
	char *fname;
	int mem_fd = -1;

	pr_info("amdgpu_plugin: Thread[0x%x] started\n", thread_data->gpu_id);

	if (asprintf (&fname, PROCPIDMEM, thread_data->pid) < 0) {
		pr_perror("failed in asprintf, %s\n", fname);
		ret = -1;
		goto exit;
	}
	mem_fd = open (fname, O_RDONLY);
	if (mem_fd < 0) {
		pr_perror("Can't open %s for pid %d\n", fname, thread_data->pid);
		free (fname);
		ret = -errno;
		goto exit;
	}
	plugin_log_msg("Opened %s file for pid = %d\n", fname, thread_data->pid);
	free (fname);

	for (i = 0; i < thread_data->num_of_bos; i++) {
		if (bo_buckets[i].gpu_id != thread_data->gpu_id)
			continue;

		num_bos++;
		if (!(bo_buckets[i].bo_alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_VRAM) &&
		    !(bo_buckets[i].bo_alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_GTT))
			continue;

		if (bo_buckets[i].bo_alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_VRAM) {
			/* Attempt sDMA based vram copy, return 0 on success */
			if (!attempt_sdma_read_write(bo_buckets, bo_info_test,
					i, thread_data->h_dev, SDMA_OP_VRAM_READ)) {
				pr_info("\n** Successfully drained the BO using sDMA: bo_buckets[%d] **\n", i);
				continue;
			}

			pr_info("Failed to read the BO using sDMA, retry with HDP: bo_buckets[%d] \n", i);
		}


		if (bo_info_test[i]->bo_alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC) {
			void *addr;

			plugin_log_msg("amdgpu_plugin: large bar read possible\n");

			addr = mmap(NULL,
					bo_buckets[i].bo_size,
					PROT_READ,
					MAP_SHARED,
					thread_data->drm_fd,
					bo_buckets[i].bo_offset);
			if (addr == MAP_FAILED) {
				pr_perror("amdgpu_plugin: mmap failed\n");
				ret = -errno;
				goto exit;
			}

			/* direct memcpy is possible on large bars */
			memcpy(bo_info_test[i]->bo_rawdata.data, addr, bo_buckets[i].bo_size);
			munmap(addr, bo_buckets[i].bo_size);
		} else {
			plugin_log_msg("Now try reading BO contents with /proc/pid/mem");
			if (lseek (mem_fd, (off_t) bo_buckets[i].bo_addr, SEEK_SET) == -1) {
				pr_perror("Can't lseek for bo_offset for pid = %d\n", thread_data->pid);
				ret = -errno;
				goto exit;
			}
			plugin_log_msg("Try to read file now\n");

			if (read(mem_fd, bo_info_test[i]->bo_rawdata.data,
				bo_info_test[i]->bo_size) != bo_info_test[i]->bo_size) {
				pr_perror("Can't read buffer\n");
				ret = -errno;
				goto exit;
			}
		} /* PROCPIDMEM read done */
	}

exit:
	pr_info("amdgpu_plugin: Thread[0x%x] done num_bos:%d ret:%d\n",
			thread_data->gpu_id, num_bos, ret);

	if (mem_fd >= 0)
		close(mem_fd);
	thread_data->ret = ret;
	return NULL;
};

void *restore_bo_contents(void *_thread_data)
{
	int i, ret = 0;
	int num_bos = 0;
	struct thread_data* thread_data = (struct thread_data*) _thread_data;
	struct kfd_criu_bo_buckets *bo_buckets = thread_data->bo_buckets;
	BoEntriesTest **bo_info_test = thread_data->bo_info_test;
	__u64 *restored_bo_offsets_array = thread_data->restored_bo_offsets;
	char *fname;
	int mem_fd = -1;

	pr_info("amdgpu_plugin: Thread[0x%x] started\n", thread_data->gpu_id);

	if (asprintf (&fname, PROCPIDMEM, thread_data->pid) < 0) {
		pr_perror("failed in asprintf, %s\n", fname);
		ret = -1;
		goto exit;
	}

	mem_fd = open (fname, O_RDWR);
	if (mem_fd < 0) {
		pr_perror("Can't open %s for pid %d\n", fname, thread_data->pid);
		free (fname);
		ret = -errno;
		goto exit;
	}
	plugin_log_msg("Opened %s file for pid = %d\n", fname, thread_data->pid);
	free (fname);

	for (i = 0; i < thread_data->num_of_bos; i++) {
		void *addr;
		if (bo_buckets[i].gpu_id != thread_data->gpu_id)
			continue;

		num_bos++;

		if (!(bo_buckets[i].bo_alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_VRAM) &&
			!(bo_buckets[i].bo_alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_GTT))
			continue;

		if (bo_buckets[i].bo_alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_VRAM) {
			/* Attempt sDMA based VRAM write, return 0 on success */
			if (!attempt_sdma_read_write(bo_buckets, bo_info_test,
					i, thread_data->h_dev, SDMA_OP_VRAM_WRITE)) {
				pr_info("\n** Successfully filled the BO using sDMA: bo_buckets[%d] ** \n", i);
				continue;
			}

			pr_info("Failed to fill the BO using sDMA, retry with HDP: bo_buckets[%d] \n", i);
		}

		if (bo_info_test[i]->bo_alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC) {

			plugin_log_msg("amdgpu_plugin: large bar write possible\n");

			addr = mmap(NULL,
					bo_buckets[i].bo_size,
					PROT_WRITE,
					MAP_SHARED,
					thread_data->drm_fd,
					restored_bo_offsets_array[i]);
			if (addr == MAP_FAILED) {
				pr_perror("amdgpu_plugin: mmap failed\n");
				ret = -errno;
				goto exit;
			}

			/* direct memcpy is possible on large bars */
			memcpy(addr, (void *)bo_info_test[i]->bo_rawdata.data, bo_info_test[i]->bo_size);
			munmap(addr, bo_info_test[i]->bo_size);
		} else {
			/* Use indirect host data path via /proc/pid/mem on small pci bar GPUs or
			 * for Buffer Objects that don't have HostAccess permissions.
			 */
			plugin_log_msg("amdgpu_plugin: using PROCPIDMEM to restore BO contents\n");
			addr = mmap(NULL,
				    bo_info_test[i]->bo_size,
				    PROT_NONE,
				    MAP_SHARED,
				    thread_data->drm_fd,
				    restored_bo_offsets_array[i]);

			if (addr == MAP_FAILED) {
				pr_perror("amdgpu_plugin: mmap failed\n");
				ret = -errno;
				goto exit;
			}

			if (lseek (mem_fd, (off_t) addr, SEEK_SET) == -1) {
				pr_perror("Can't lseek for bo_offset for pid = %d\n", thread_data->pid);
				ret = -errno;
				goto exit;
			}

			plugin_log_msg("Attempt writting now\n");
			if (write(mem_fd, bo_info_test[i]->bo_rawdata.data, bo_info_test[i]->bo_size) !=
			    bo_info_test[i]->bo_size) {
				pr_perror("Can't write buffer\n");
				ret = -errno;
				goto exit;
			}
			munmap(addr, bo_info_test[i]->bo_size);
		}
	}

exit:
	pr_info("amdgpu_plugin: Thread[0x%x] done num_bos:%d ret:%d\n",
			thread_data->gpu_id, num_bos, ret);

	if (mem_fd >= 0)
		close(mem_fd);
	thread_data->ret = ret;
	return NULL;
};

int check_hsakmt_shared_mem(uint64_t *shared_mem_size, uint32_t *shared_mem_magic)
{
	int ret;
	struct stat st;

	ret = stat(HSAKMT_SHM_PATH, &st);
	if (ret) {
		*shared_mem_size = 0;
		return 0;
	}

	*shared_mem_size = st.st_size;

	/* First 4 bytes of shared file is the magic */
	ret = read_file(HSAKMT_SHM_PATH, shared_mem_magic, sizeof(*shared_mem_magic));
	if (ret)
		pr_perror("amdgpu_plugin: Failed to read shared mem magic\n");
	else
		plugin_log_msg("amdgpu_plugin: Shared mem magic:0x%x\n", *shared_mem_magic);

	return 0;
}

int restore_hsakmt_shared_mem(const uint64_t shared_mem_size, const uint32_t shared_mem_magic)
{
	int ret, fd;
	struct stat st;
	sem_t *sem = SEM_FAILED;

	if (!shared_mem_size)
		return 0;

	if (!stat(HSAKMT_SHM_PATH, &st)) {
		pr_debug("amdgpu_plugin: %s already exists\n", HSAKMT_SHM_PATH);
	} else {
		pr_info("Warning:%s was missing. Re-creating new file but we may lose perf counters\n",
										HSAKMT_SHM_PATH);
		fd = shm_open(HSAKMT_SHM, O_CREAT | O_RDWR, 0666);

		ret = ftruncate(fd, shared_mem_size);
		if (ret < 0) {
			pr_err("amdgpu_plugin: Failed to truncate shared mem %s\n", HSAKMT_SHM);
			close(fd);
			return -errno;
		}

		ret = write(fd, &shared_mem_magic, sizeof(shared_mem_magic));
		if (ret != sizeof(shared_mem_magic)) {
			pr_perror("amdgpu_plugin: Failed to restore shared mem magic\n");
			close(fd);
			return -errno;
		}

		close(fd);
	}

	sem = sem_open(HSAKMT_SEM, O_CREAT, 0666, 1);
	if (sem == SEM_FAILED) {
		pr_perror("Failed to create %s\n", HSAKMT_SEM);
		return -EACCES;
	}
	sem_close(sem);
	return 0;
}

int amdgpu_plugin_dump_file(int fd, int id)
{
	struct kfd_ioctl_criu_helper_args helper_args = {0};
	struct kfd_criu_devinfo_bucket *devinfo_bucket_ptr;
	struct kfd_criu_ev_bucket *ev_buckets_ptr = NULL;
	struct kfd_ioctl_criu_dumper_args args = {0};
	struct kfd_criu_bo_buckets *bo_bucket_ptr;
	struct kfd_criu_q_bucket *q_bucket_ptr;
	amdgpu_device_handle h_dev;
	char img_path[PATH_MAX];
	struct stat st, st_kfd;
	uint32_t major, minor;
	unsigned char *buf;
	size_t len;
	int ret;

	struct thread_data thread_datas[NUM_OF_SUPPORTED_GPUS];
	memset(thread_datas, 0, sizeof(thread_datas));

	pr_debug("amdgpu_plugin: Enter cr_plugin_dump_file()- ID = 0x%x\n", id);
	ret = 0;
	CriuKfd *e;

	if (fstat(fd, &st) == -1) {
		pr_perror("amdgpu_plugin: fstat error");
		return -1;
	}

	ret = stat("/dev/kfd", &st_kfd);
	if (ret == -1) {
		pr_perror("amdgpu_plugin: fstat error for /dev/kfd\n");
		return -1;
	}

	if (topology_parse(&src_topology, "Checkpoint"))
		return -1;

	/* We call topology_determine_iolinks to validate io_links. If io_links are not valid
	   we do not store them inside the checkpointed images */
	if (topology_determine_iolinks(&src_topology)) {
		pr_err("Failed to determine iolinks from topology\n");
		return -1;
	}

	/* Check whether this plugin was called for kfd or render nodes */
	if (major(st.st_rdev) != major(st_kfd.st_rdev) ||
		 minor(st.st_rdev) != 0) {
		/* This is RenderD dumper plugin, save the render minor and gpu_id */
		CriuRenderNode rd = CRIU_RENDER_NODE__INIT;
		struct tp_node *tp_node;

		pr_info("amdgpu_plugin: Dumper called for /dev/dri/renderD%d, FD = %d, ID = %d\n",
			minor(st.st_rdev), fd, id);

		tp_node = sys_get_node_by_render_minor(&src_topology, minor(st.st_rdev));
		if (!tp_node) {
			pr_err("amdgpu_plugin: Failed to find a device with minor number = %d\n",
				minor(st.st_rdev));

			return -ENODEV;
		}

		rd.gpu_id = maps_get_dest_gpu(&checkpoint_maps, tp_node->gpu_id);
		if (!rd.gpu_id) {
			return -ENODEV;
			goto failed;
		}

		len = criu_render_node__get_packed_size(&rd);
		buf = xmalloc(len);
		if (!buf)
			return -ENOMEM;

		criu_render_node__pack(&rd, buf);

		snprintf(img_path, sizeof(img_path), "renderDXXX.%d.img", id);
		ret = write_file(img_path,  buf, len);
		if (ret) {
			xfree(buf);
			return ret;
		}

		xfree(buf);

		/* Need to return success here so that criu can call plugins for renderD nodes */
		return ret;
	}

	pr_info("amdgpu_plugin: %s : %s() called for fd = %d\n", CR_PLUGIN_DESC.name,
		  __func__, major(st.st_rdev));

	if (kmtIoctl(fd, AMDKFD_IOC_CRIU_HELPER, &helper_args) == -1) {
		pr_perror("amdgpu_plugin: failed to call helper ioctl\n");
		return -1;
	}

	args.num_of_devices = helper_args.num_of_devices;
	devinfo_bucket_ptr = xmalloc(helper_args.num_of_devices * sizeof(*devinfo_bucket_ptr));

	if (!devinfo_bucket_ptr) {
		pr_perror("amdgpu_plugin: failed to allocate devinfo for dumper ioctl\n");
		return -ENOMEM;
	}
	args.kfd_criu_devinfo_buckets_ptr = (uintptr_t)devinfo_bucket_ptr;

	pr_info("amdgpu_plugin: num of bos = %llu\n", helper_args.num_of_bos);

	bo_bucket_ptr = xmalloc(helper_args.num_of_bos * sizeof(*bo_bucket_ptr));

	if (!bo_bucket_ptr) {
		pr_perror("amdgpu_plugin: failed to allocate args for dumper ioctl\n");
		return -ENOMEM;
	}

	args.num_of_bos = helper_args.num_of_bos;
	args.kfd_criu_bo_buckets_ptr = (uintptr_t)bo_bucket_ptr;

	pr_info("amdgpu_plugin: num of queues = %u\n", helper_args.num_of_queues);

	q_bucket_ptr = xmalloc(helper_args.num_of_queues * sizeof(*q_bucket_ptr));
	if (!q_bucket_ptr) {
		pr_perror("amdgpu_plugin: failed to allocate args for dumper ioctl\n");
		return -1;
	}

	args.num_of_queues = helper_args.num_of_queues;
	args.kfd_criu_q_buckets_ptr = (uintptr_t)q_bucket_ptr;

	if (helper_args.queues_data_size) {
		args.queues_data_ptr = (uintptr_t)xmalloc(helper_args.queues_data_size);
		if (!args.queues_data_ptr) {
			pr_perror("amdgpu_plugin: failed to allocate args for dumper ioctl\n");
			return -1;
		}
		args.queues_data_size = helper_args.queues_data_size;
		pr_info("amdgpu_plugin: queues data size:%llu\n", args.queues_data_size);
	}

	if (helper_args.num_of_events) {
		ev_buckets_ptr = xmalloc(helper_args.num_of_events * sizeof(*ev_buckets_ptr));
		args.num_of_events = helper_args.num_of_events;
	}

	args.kfd_criu_ev_buckets_ptr = (uintptr_t)ev_buckets_ptr;

	/* call dumper ioctl, pass num of BOs to dump */
        if (kmtIoctl(fd, AMDKFD_IOC_CRIU_DUMPER, &args) == -1) {
		pr_perror("amdgpu_plugin: failed to call kfd ioctl from plugin dumper for fd = %d\n", major(st.st_rdev));
		xfree(bo_bucket_ptr);
		return -1;
	}

	pr_info("amdgpu_plugin: success in calling dumper ioctl\n");

	e = xmalloc(sizeof(*e));
	if (!e) {
		pr_err("Failed to allocate proto structure\n");
		xfree(bo_bucket_ptr);
		return -ENOMEM;
	}

	criu_kfd__init(e);
	e->pid = helper_args.task_pid;

	/* When checkpointing on a node where there was already a checkpoint-restore before, the
	 * user_gpu_id and actual_gpu_id will be different.
	 *
	 * We store the user_gpu_id in the stored image files so that the stored images always have
	 * the gpu_id's of the node where the application was first launched. */
	for (int i = 0; i < args.num_of_devices; i++)
		maps_add_gpu_entry(&checkpoint_maps, devinfo_bucket_ptr[i].actual_gpu_id,
				   devinfo_bucket_ptr[i].user_gpu_id);

	ret = allocate_devinfo_entries(e, src_topology.num_nodes);
	if (ret) {
		ret = -ENOMEM;
		goto failed;
	}

	/* Store local topology information */
	ret = topology_to_devinfo(&src_topology, devinfo_bucket_ptr,
					&checkpoint_maps, e->devinfo_entries);
	if (ret)
		goto failed;

	e->num_of_gpus = args.num_of_devices;
	e->num_of_cpus = src_topology.num_nodes - args.num_of_devices;

	ret = allocate_bo_info_test(e, helper_args.num_of_bos, bo_bucket_ptr);
	if (ret)
		return -1;

	for (int i = 0; i < helper_args.num_of_bos; i++)
	{
		(e->bo_info_test[i])->bo_addr = (bo_bucket_ptr)[i].bo_addr;
		(e->bo_info_test[i])->bo_size = (bo_bucket_ptr)[i].bo_size;
		(e->bo_info_test[i])->bo_offset = (bo_bucket_ptr)[i].bo_offset;
		(e->bo_info_test[i])->bo_alloc_flags = (bo_bucket_ptr)[i].bo_alloc_flags;
		(e->bo_info_test[i])->idr_handle = (bo_bucket_ptr)[i].idr_handle;
		(e->bo_info_test[i])->user_addr = (bo_bucket_ptr)[i].user_addr;

		e->bo_info_test[i]->gpu_id = maps_get_dest_gpu(&checkpoint_maps,
							       bo_bucket_ptr[i].gpu_id);
		if (!e->bo_info_test[i]->gpu_id) {
			ret = -ENODEV;
			goto failed;
		}
	}
	e->num_of_bos = helper_args.num_of_bos;

	plugin_log_msg("Dumping bo_info_test \n");
	for (int i = 0; i < helper_args.num_of_bos; i++)
	{
		plugin_log_msg("e->bo_info_test[%d]:\n", i);
		plugin_log_msg("bo_addr = 0x%lx, bo_size = 0x%lx, bo_offset = 0x%lx, gpu_id = 0x%x, "
			"bo_alloc_flags = 0x%x, idr_handle = 0x%x\n",
		  (e->bo_info_test[i])->bo_addr,
		  (e->bo_info_test[i])->bo_size,
		  (e->bo_info_test[i])->bo_offset,
		  (e->bo_info_test[i])->gpu_id,
		  (e->bo_info_test[i])->bo_alloc_flags,
		  (e->bo_info_test[i])->idr_handle);
		plugin_log_msg("(bo_bucket_ptr)[%d]:\n", i);
		plugin_log_msg("bo_addr = 0x%llx, bo_size = 0x%llx, bo_offset = 0x%llx, "
			"gpu_id = 0x%x, bo_alloc_flags = 0x%x, idr_handle = 0x%x\n",
		  (bo_bucket_ptr)[i].bo_addr,
		  (bo_bucket_ptr)[i].bo_size,
		  (bo_bucket_ptr)[i].bo_offset,
		  (bo_bucket_ptr)[i].gpu_id,
		  (bo_bucket_ptr)[i].bo_alloc_flags,
		  (bo_bucket_ptr)[i].idr_handle);

	}

	for (int i = 0; i < helper_args.num_of_devices; i++) {
		struct tp_node *dev;
		int ret_thread = 0;
		thread_datas[i].gpu_id = devinfo_bucket_ptr[i].actual_gpu_id;
		thread_datas[i].bo_buckets = bo_bucket_ptr;
		thread_datas[i].bo_info_test = e->bo_info_test;
		thread_datas[i].pid = e->pid;
		thread_datas[i].num_of_bos = helper_args.num_of_bos;

		dev = sys_get_node_by_gpu_id(&src_topology, thread_datas[i].gpu_id);
		if (!dev) {
			ret = -ENODEV;
			goto failed;
		}

		thread_datas[i].drm_fd = open_drm_render_device(dev->drm_render_minor);
		if (thread_datas[i].drm_fd < 0) {
			ret = thread_datas[i].drm_fd;
			goto failed;
		}

		ret = amdgpu_device_initialize(thread_datas[i].drm_fd, &major, &minor, &h_dev);
		if (ret) {
			pr_perror("failed to initialize device");
			return -EFAULT;
		}
		thread_datas[i].h_dev = h_dev;

		ret_thread = pthread_create(&thread_datas[i].thread, NULL, dump_bo_contents, (void*) &thread_datas[i]);
		if (ret_thread) {
			pr_err("Failed to create thread[%i]\n", i);
			ret = -ret_thread;
			goto failed;
		}
	}

	for (int i = 0; i < helper_args.num_of_devices; i++) {
		pthread_join(thread_datas[i].thread, NULL);
		pr_info("Thread[0x%x] finished ret:%d\n", thread_datas[i].gpu_id, thread_datas[i].ret);
		amdgpu_device_deinitialize(thread_datas[i].h_dev);

		if (thread_datas[i].drm_fd >= 0)
			close(thread_datas[i].drm_fd);

		if (thread_datas[i].ret) {
			ret = thread_datas[i].ret;
			goto failed;
		}
	}

	ret = allocate_q_entries(e, helper_args.num_of_queues);
	if (ret)
		return ret;

	e->num_of_queues = helper_args.num_of_queues;

	for (int i = 0; i < e->num_of_queues; i++)
	{
		uint8_t *queue_data_ptr = (uint8_t *)args.queues_data_ptr
					+ q_bucket_ptr[i].queues_data_offset;

		plugin_log_msg("Dumping Queue[%d]:\n", i);
		plugin_log_msg("\tgpu_id:%x type:%x format:%x q_id:%x q_address:%llx ",
			q_bucket_ptr[i].gpu_id,
			q_bucket_ptr[i].type,
			q_bucket_ptr[i].format,
			q_bucket_ptr[i].q_id,
			q_bucket_ptr[i].q_address);

		e->q_entries[i]->gpu_id = maps_get_dest_gpu(&checkpoint_maps, q_bucket_ptr[i].gpu_id);
		if (!e->q_entries[i]->gpu_id) {
			ret = -ENODEV;
			goto failed;
		}

		e->q_entries[i]->type = q_bucket_ptr[i].type;
		e->q_entries[i]->format = q_bucket_ptr[i].format;
		e->q_entries[i]->q_id = q_bucket_ptr[i].q_id;
		e->q_entries[i]->q_address = q_bucket_ptr[i].q_address;
		e->q_entries[i]->q_size = q_bucket_ptr[i].q_size;
		e->q_entries[i]->priority = q_bucket_ptr[i].priority;
		e->q_entries[i]->q_percent = q_bucket_ptr[i].q_percent;
		e->q_entries[i]->read_ptr_addr = q_bucket_ptr[i].read_ptr_addr;
		e->q_entries[i]->write_ptr_addr = q_bucket_ptr[i].write_ptr_addr;
		e->q_entries[i]->doorbell_id = q_bucket_ptr[i].doorbell_id;
		e->q_entries[i]->doorbell_off = q_bucket_ptr[i].doorbell_off;
		e->q_entries[i]->is_gws = q_bucket_ptr[i].is_gws;
		e->q_entries[i]->sdma_id = q_bucket_ptr[i].sdma_id;
		e->q_entries[i]->eop_ring_buffer_address = q_bucket_ptr[i].eop_ring_buffer_address;
		e->q_entries[i]->eop_ring_buffer_size = q_bucket_ptr[i].eop_ring_buffer_size;
		e->q_entries[i]->ctx_save_restore_area_address = q_bucket_ptr[i].ctx_save_restore_area_address;
		e->q_entries[i]->ctx_save_restore_area_size = q_bucket_ptr[i].ctx_save_restore_area_size;
		e->q_entries[i]->ctl_stack_size = q_bucket_ptr[i].ctl_stack_size;

		e->q_entries[i]->cu_mask.len = q_bucket_ptr[i].cu_mask_size;
		e->q_entries[i]->cu_mask.data = queue_data_ptr;

		e->q_entries[i]->mqd.len = q_bucket_ptr[i].mqd_size;
		e->q_entries[i]->mqd.data = queue_data_ptr + q_bucket_ptr[i].cu_mask_size;

		e->q_entries[i]->ctl_stack.len = q_bucket_ptr[i].ctl_stack_size;
		e->q_entries[i]->ctl_stack.data = queue_data_ptr + q_bucket_ptr[i].cu_mask_size + q_bucket_ptr[i].mqd_size;
	}

	e->event_page_offset = args.event_page_offset;
	pr_info("amdgpu_plugin: number of events:%d\n", args.num_of_events);

	if (args.num_of_events) {
		ret = allocate_ev_entries(e, args.num_of_events);
		if (ret)
			return ret;

		for (int i = 0; i < args.num_of_events; i++) {
			e->ev_entries[i]->event_id = ev_buckets_ptr[i].event_id;
			e->ev_entries[i]->auto_reset = ev_buckets_ptr[i].auto_reset;
			e->ev_entries[i]->type = ev_buckets_ptr[i].type;
			e->ev_entries[i]->signaled = ev_buckets_ptr[i].signaled;

			if (e->ev_entries[i]->type == KFD_IOC_EVENT_MEMORY) {
				e->ev_entries[i]->mem_exc_fail_not_present =
					ev_buckets_ptr[i].memory_exception_data.failure.NotPresent;
				e->ev_entries[i]->mem_exc_fail_read_only =
					ev_buckets_ptr[i].memory_exception_data.failure.ReadOnly;
				e->ev_entries[i]->mem_exc_fail_no_execute =
					ev_buckets_ptr[i].memory_exception_data.failure.NoExecute;
				e->ev_entries[i]->mem_exc_va =
					ev_buckets_ptr[i].memory_exception_data.va;
				if (ev_buckets_ptr[i].memory_exception_data.gpu_id) {
					e->ev_entries[i]->mem_exc_gpu_id =
						maps_get_dest_gpu(&checkpoint_maps,
						ev_buckets_ptr[i].memory_exception_data.gpu_id);
					if (!&e->ev_entries[i]->mem_exc_gpu_id) {
						ret = -ENODEV;
						goto failed;
					}
				}
			} else if (e->ev_entries[i]->type == KFD_IOC_EVENT_HW_EXCEPTION) {
				e->ev_entries[i]->hw_exc_reset_type =
					ev_buckets_ptr[i].hw_exception_data.reset_type;
				e->ev_entries[i]->hw_exc_reset_cause =
					ev_buckets_ptr[i].hw_exception_data.reset_cause;
				e->ev_entries[i]->hw_exc_memory_lost =
					ev_buckets_ptr[i].hw_exception_data.memory_lost;
				if (ev_buckets_ptr[i].hw_exception_data.gpu_id) {
					e->ev_entries[i]->hw_exc_gpu_id =
						maps_get_dest_gpu(&checkpoint_maps,
						ev_buckets_ptr[i].hw_exception_data.gpu_id);

					if (!e->ev_entries[i]->hw_exc_gpu_id) {
						ret = -ENODEV;
						goto failed;
					}
				}
			}
		}
	}

	ret = check_hsakmt_shared_mem(&e->shared_mem_size, &e->shared_mem_magic);
	if (ret)
		goto failed;

	snprintf(img_path, sizeof(img_path), "kfd.%d.img", id);
	pr_info("amdgpu_plugin: img_path = %s", img_path);

	len = criu_kfd__get_packed_size(e);

	pr_info("amdgpu_plugin: Len = %ld\n", len);

	buf = xmalloc(len);
	if (!buf) {
		pr_perror("failed to allocate memory\n");
		ret = -ENOMEM;
		goto failed;
	}

	criu_kfd__pack(e, buf);

	ret = write_file(img_path,  buf, len);
	if (ret)
		ret = -1;

	xfree(buf);
failed:
	xfree(devinfo_bucket_ptr);
	xfree(bo_bucket_ptr);
	xfree(q_bucket_ptr);
	if (ev_buckets_ptr)
		xfree(ev_buckets_ptr);
	free_e(e);
	pr_info("amdgpu_plugin: Exiting from dumper for fd = %d\n", major(st.st_rdev));
        return ret;

}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__DUMP_EXT_FILE, amdgpu_plugin_dump_file)

int amdgpu_plugin_restore_file(int id)
{
	struct kfd_criu_devinfo_bucket *devinfo_bucket_ptr = NULL;
	int fd;
	struct kfd_ioctl_criu_restorer_args args = {0};
	struct kfd_criu_bo_buckets *bo_bucket_ptr;
	struct kfd_criu_q_bucket *q_bucket_ptr;
	struct kfd_criu_ev_bucket *ev_bucket_ptr = NULL;
	__u64 *restored_bo_offsets_array;
	char img_path[PATH_MAX];
	struct stat filestat;
	unsigned char *buf;
	CriuRenderNode *rd;
	CriuKfd *e;
	struct thread_data thread_datas[NUM_OF_SUPPORTED_GPUS];
	memset(thread_datas, 0, sizeof(thread_datas));
	uint32_t major, minor;
	amdgpu_device_handle h_dev;

	pr_info("amdgpu_plugin: Initialized kfd plugin restorer with ID = %d\n", id);

	snprintf(img_path, sizeof(img_path), "kfd.%d.img", id);

	if (stat(img_path, &filestat) == -1) {
		struct tp_node *tp_node;
		uint32_t target_gpu_id;

		pr_perror("open(%s)", img_path);

		/* This is restorer plugin for renderD nodes. Criu doesn't guarantee that they will
		 * be called before the plugin is called for kfd file descriptor.
		 * TODO: Currently, this code will only work if this function is called for /dev/kfd
		 * first as we assume restore_maps is already filled. Need to fix this later.
		 */
		snprintf(img_path, sizeof(img_path), "renderDXXX.%d.img", id);

		if (stat(img_path, &filestat) == -1)
		{
			pr_perror("Failed to read file stats\n");
			return -1;
		}
		pr_info("renderD file size on disk = %ld\n", filestat.st_size);

		buf = xmalloc(filestat.st_size);
		if (!buf) {
			pr_perror("Failed to allocate memory\n");
			return -ENOMEM;
		}

		if (read_file(img_path, buf, filestat.st_size)) {
			pr_perror("Unable to read from %s", img_path);
			xfree(buf);
			return -1;
		}

		rd = criu_render_node__unpack(NULL, filestat.st_size, buf);
		if (rd == NULL) {
			pr_perror("Unable to parse the KFD message %d", id);
			fd = -EBADFD;
			goto fail;
		}

		pr_info("amdgpu_plugin: render node gpu_id = 0x%04x\n", rd->gpu_id);

		target_gpu_id = maps_get_dest_gpu(&restore_maps, rd->gpu_id);
		if (!target_gpu_id) {
			fd = -ENODEV;
			goto fail;
		}

		tp_node = sys_get_node_by_gpu_id(&dest_topology, target_gpu_id);
		if (!tp_node) {
			fd = -ENODEV;
			goto fail;
		}

		pr_info("amdgpu_plugin: render node destination gpu_id = 0x%04x\n", tp_node->gpu_id);

		fd = open_drm_render_device(tp_node->drm_render_minor);
		if (fd < 0)
			pr_err("amdgpu_plugin: Failed to open render device (minor:%d)\n",
									tp_node->drm_render_minor);
fail:
		criu_render_node__free_unpacked(rd,  NULL);
		xfree(buf);
		return fd;
	}

	fd = open("/dev/kfd", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		pr_perror("failed to open kfd in plugin");
		return -1;
	}
	pr_info("amdgpu_plugin: Opened kfd, fd = %d\n", fd);
	pr_info("kfd img file size on disk = %ld\n", filestat.st_size);

	buf = xmalloc(filestat.st_size);
	if (!buf) {
		pr_perror("Failed to allocate memory\n");
		return -ENOMEM;
	}

	if (read_file(img_path, buf, filestat.st_size)) {
		xfree(buf);
		return -1;
	}

	e = criu_kfd__unpack(NULL, filestat.st_size, buf);
	if (e == NULL) {
		pr_err("Unable to parse the KFD message %#x\n", id);
		xfree(buf);
		return -1;
	}

	plugin_log_msg("amdgpu_plugin: read image file data\n");

	if (devinfo_to_topology(e->devinfo_entries, e->num_of_gpus + e->num_of_cpus, &src_topology)) {
		pr_err("Failed to convert stored device information to topology\n");
		xfree(buf);
		return -1;
	}

	if (topology_parse(&dest_topology, "Local")) {
		pr_err("Failed to parse local system topology\n");
		xfree(buf);
		return -1;
	}

	args.num_of_devices = e->num_of_gpus;

	devinfo_bucket_ptr = xmalloc(args.num_of_devices * sizeof(*devinfo_bucket_ptr));
	if (!devinfo_bucket_ptr) {
		fd = -ENOMEM;
		goto clean;
	}
	args.kfd_criu_devinfo_buckets_ptr = (uintptr_t)devinfo_bucket_ptr;

	if (set_restore_gpu_maps(&src_topology, &dest_topology, &restore_maps)) {
		fd = -EBADFD;
		goto clean;
	}

	int bucket_index = 0;
	for (int entries_index = 0; entries_index < e->num_of_gpus + e->num_of_cpus; entries_index++) {
		struct tp_node *tp_node;
		int drm_fd;

		if (!e->devinfo_entries[entries_index]->gpu_id)
			continue;

		devinfo_bucket_ptr[bucket_index].user_gpu_id = e->devinfo_entries[entries_index]->gpu_id;

		devinfo_bucket_ptr[bucket_index].actual_gpu_id =
				maps_get_dest_gpu(&restore_maps, e->devinfo_entries[entries_index]->gpu_id);

		if (!devinfo_bucket_ptr[bucket_index].actual_gpu_id) {
			fd = -ENODEV;
			goto clean;
		}

		tp_node = sys_get_node_by_gpu_id(&dest_topology,
							devinfo_bucket_ptr[bucket_index].actual_gpu_id);
		if (!tp_node) {
			fd = -ENODEV;
			goto clean;
		}

		drm_fd = open_drm_render_device(tp_node->drm_render_minor);
		if (drm_fd < 0) {
			fd = -drm_fd;
			goto clean;
		}
		devinfo_bucket_ptr[bucket_index].drm_fd = drm_fd;
		bucket_index++;
	}

	for (int i = 0; i < e->num_of_bos; i++ )
	{
		plugin_log_msg("reading e->bo_info_test[%d]:\n", i);
		plugin_log_msg("bo_addr = 0x%lx, bo_size = 0x%lx, bo_offset = 0x%lx, "
			"gpu_id = 0x%x, bo_alloc_flags = 0x%x, idr_handle = 0x%x user_addr=0x%lx\n",
		  (e->bo_info_test[i])->bo_addr,
		  (e->bo_info_test[i])->bo_size,
		  (e->bo_info_test[i])->bo_offset,s
		  (e->bo_info_test[i])->gpu_id,
		  (e->bo_info_test[i])->bo_alloc_flags,
		  (e->bo_info_test[i])->idr_handle,
		  (e->bo_info_test[i])->user_addr);
	}

	bo_bucket_ptr = xmalloc(e->num_of_bos * sizeof(*bo_bucket_ptr));
	if (!bo_bucket_ptr) {
		pr_perror("amdgpu_plugin: failed to allocate args for restorer ioctl\n");
		return -1;
	}

	for (int i = 0; i < e->num_of_bos; i++)
	{
		(bo_bucket_ptr)[i].bo_addr = (e->bo_info_test[i])->bo_addr;
		(bo_bucket_ptr)[i].bo_size = (e->bo_info_test[i])->bo_size;
		(bo_bucket_ptr)[i].bo_offset = (e->bo_info_test[i])->bo_offset;
		(bo_bucket_ptr)[i].bo_alloc_flags = (e->bo_info_test[i])->bo_alloc_flags;
		(bo_bucket_ptr)[i].idr_handle = (e->bo_info_test[i])->idr_handle;
		(bo_bucket_ptr)[i].user_addr = (e->bo_info_test[i])->user_addr;

		bo_bucket_ptr[i].gpu_id =
				maps_get_dest_gpu(&restore_maps, e->bo_info_test[i]->gpu_id);
		if (!bo_bucket_ptr[i].gpu_id) {
			fd = -ENODEV;
			goto clean;
		}
	}

	args.num_of_bos = e->num_of_bos;
	args.kfd_criu_bo_buckets_ptr = (uintptr_t)bo_bucket_ptr;

	restored_bo_offsets_array = xmalloc(sizeof(uint64_t) * e->num_of_bos);
	if (!restored_bo_offsets_array) {
		xfree(bo_bucket_ptr);
		return -ENOMEM;
	}

	args.restored_bo_array_ptr = (uint64_t)restored_bo_offsets_array;

	q_bucket_ptr = xmalloc(e->num_of_queues * sizeof(*q_bucket_ptr));
        if (!q_bucket_ptr) {
               pr_perror("amdgpu_plugin: failed to allocate args for dumper ioctl\n");
               return -1;
	}

	pr_info("Number of queues:%u\n", e->num_of_queues);

	args.queues_data_size = 0;
	for (int i = 0; i < e->num_of_queues; i++ ) {
		args.queues_data_size += e->q_entries[i]->cu_mask.len
					+ e->q_entries[i]->mqd.len
					+ e->q_entries[i]->ctl_stack.len;
	}

	pr_info("Queues data size:%llu\n", args.queues_data_size);

	args.queues_data_ptr = (uintptr_t)xmalloc(args.queues_data_size);
	if (!args.queues_data_ptr) {
		pr_perror("amdgpu_plugin: failed to allocate args for dumper ioctl\n");
		return -1;
	}

	uint32_t queues_data_offset = 0;

	for (int i = 0; i < e->num_of_queues; i++ )
	{
		uint8_t *queue_data;
		plugin_log_msg("Restoring Queue[%d]:\n", i);
		plugin_log_msg("\tgpu_id:%x type:%x format:%x q_id:%x q_address:%lx "
			"cu_mask_size:%lx mqd_size:%lx ctl_stack_size:%lx\n",
			e->q_entries[i]->gpu_id,
			e->q_entries[i]->type,
			e->q_entries[i]->format,
			e->q_entries[i]->q_id,
			e->q_entries[i]->q_address,
			e->q_entries[i]->cu_mask.len,
			e->q_entries[i]->mqd.len,
			e->q_entries[i]->ctl_stack.len);

		q_bucket_ptr[i].gpu_id = maps_get_dest_gpu(&restore_maps, e->q_entries[i]->gpu_id);
		if (!q_bucket_ptr[i].gpu_id) {
			fd = -ENODEV;
			goto clean;
		}

		q_bucket_ptr[i].type = e->q_entries[i]->type;
		q_bucket_ptr[i].format = e->q_entries[i]->format;
		q_bucket_ptr[i].q_id = e->q_entries[i]->q_id;
		q_bucket_ptr[i].q_address = e->q_entries[i]->q_address;
		q_bucket_ptr[i].q_size = e->q_entries[i]->q_size;
		q_bucket_ptr[i].priority = e->q_entries[i]->priority;
		q_bucket_ptr[i].q_percent = e->q_entries[i]->q_percent;
		q_bucket_ptr[i].read_ptr_addr = e->q_entries[i]->read_ptr_addr;
		q_bucket_ptr[i].write_ptr_addr = e->q_entries[i]->write_ptr_addr;
		q_bucket_ptr[i].doorbell_id = e->q_entries[i]->doorbell_id;
		q_bucket_ptr[i].doorbell_off = e->q_entries[i]->doorbell_off;
		q_bucket_ptr[i].is_gws = e->q_entries[i]->is_gws;
		q_bucket_ptr[i].sdma_id = e->q_entries[i]->sdma_id;
		q_bucket_ptr[i].eop_ring_buffer_address = e->q_entries[i]->eop_ring_buffer_address;
		q_bucket_ptr[i].eop_ring_buffer_size = e->q_entries[i]->eop_ring_buffer_size;
		q_bucket_ptr[i].ctx_save_restore_area_address = e->q_entries[i]->ctx_save_restore_area_address;
		q_bucket_ptr[i].ctx_save_restore_area_size = e->q_entries[i]->ctx_save_restore_area_size;
		q_bucket_ptr[i].ctl_stack_size = e->q_entries[i]->ctl_stack_size;

		q_bucket_ptr[i].queues_data_offset = queues_data_offset;
		queue_data = (uint8_t *)args.queues_data_ptr + queues_data_offset;

		q_bucket_ptr[i].cu_mask_size = e->q_entries[i]->cu_mask.len;
		memcpy(queue_data,
			e->q_entries[i]->cu_mask.data,
			e->q_entries[i]->cu_mask.len);

		q_bucket_ptr[i].mqd_size = e->q_entries[i]->mqd.len;
		memcpy(queue_data + e->q_entries[i]->cu_mask.len,
			e->q_entries[i]->mqd.data,
			e->q_entries[i]->mqd.len);

		q_bucket_ptr[i].ctl_stack_size = e->q_entries[i]->ctl_stack.len;
		memcpy(queue_data + e->q_entries[i]->cu_mask.len + e->q_entries[i]->mqd.len,
			e->q_entries[i]->ctl_stack.data,
			e->q_entries[i]->ctl_stack.len);

		queues_data_offset += e->q_entries[i]->cu_mask.len
					+ e->q_entries[i]->mqd.len
					+ e->q_entries[i]->ctl_stack.len;

	}

	args.num_of_queues = e->num_of_queues;
	args.kfd_criu_q_buckets_ptr = (uintptr_t)q_bucket_ptr;

	args.event_page_offset = e->event_page_offset;

	pr_info("Number of events:%u\n", e->num_of_events);
	if (e->num_of_events) {
		ev_bucket_ptr = xmalloc(e->num_of_events * sizeof(*ev_bucket_ptr));
		if (!ev_bucket_ptr) {
			pr_perror("amdgpu_plugin: failed to allocate events for restore ioctl\n");
			return -1;
		}

		for (int i = 0; i < e->num_of_events; i++ )
		{
			ev_bucket_ptr[i].event_id = e->ev_entries[i]->event_id;
			ev_bucket_ptr[i].auto_reset = e->ev_entries[i]->auto_reset;
			ev_bucket_ptr[i].type = e->ev_entries[i]->type;
			ev_bucket_ptr[i].signaled = e->ev_entries[i]->signaled;

			if (e->ev_entries[i]->type == KFD_IOC_EVENT_MEMORY) {
				ev_bucket_ptr[i].memory_exception_data.failure.NotPresent =
						e->ev_entries[i]->mem_exc_fail_not_present;
				ev_bucket_ptr[i].memory_exception_data.failure.ReadOnly =
						e->ev_entries[i]->mem_exc_fail_read_only;
				ev_bucket_ptr[i].memory_exception_data.failure.NoExecute =
						e->ev_entries[i]->mem_exc_fail_no_execute;
				ev_bucket_ptr[i].memory_exception_data.va =
						e->ev_entries[i]->mem_exc_va;
				if (e->ev_entries[i]->mem_exc_gpu_id) {
					ev_bucket_ptr[i].memory_exception_data.gpu_id =
						maps_get_dest_gpu(&restore_maps,
								  e->ev_entries[i]->mem_exc_gpu_id);
					if (!ev_bucket_ptr[i].memory_exception_data.gpu_id) {
						fd = -ENODEV;
						goto clean;
					}
				}
			} else if (e->ev_entries[i]->type == KFD_IOC_EVENT_HW_EXCEPTION) {
				ev_bucket_ptr[i].hw_exception_data.reset_type =
					e->ev_entries[i]->hw_exc_reset_type;
				ev_bucket_ptr[i].hw_exception_data.reset_cause =
					e->ev_entries[i]->hw_exc_reset_cause;
				ev_bucket_ptr[i].hw_exception_data.memory_lost =
					e->ev_entries[i]->hw_exc_memory_lost;

				if (e->ev_entries[i]->hw_exc_gpu_id) {
					ev_bucket_ptr[i].hw_exception_data.gpu_id =
						maps_get_dest_gpu(&restore_maps,
								 e->ev_entries[i]->hw_exc_gpu_id);

					if (!ev_bucket_ptr[i].memory_exception_data.gpu_id) {
						fd = -ENODEV;
						goto clean;
					}
				}
			}
		}

		args.num_of_events = e->num_of_events;
		args.kfd_criu_ev_buckets_ptr = (uintptr_t)ev_bucket_ptr;
	}

	if (kmtIoctl(fd, AMDKFD_IOC_CRIU_RESTORER, &args) == -1) {
		pr_perror("amdgpu_plugin: failed to call kfd ioctl from plugin restorer for id = %d\n", id);
		fd = -EBADFD;
		goto clean;
	}

	for (int i = 0; i < e->num_of_bos; i++)
	{
		if (e->bo_info_test[i]->bo_alloc_flags &
			(KFD_IOC_ALLOC_MEM_FLAGS_VRAM |
			 KFD_IOC_ALLOC_MEM_FLAGS_GTT |
			 KFD_IOC_ALLOC_MEM_FLAGS_MMIO_REMAP |
			 KFD_IOC_ALLOC_MEM_FLAGS_DOORBELL)) {

			struct tp_node *tp_node;
			struct vma_metadata *vma_md;
			vma_md = xmalloc(sizeof(*vma_md));
			if (!vma_md)
				return -ENOMEM;

			memset(vma_md, 0, sizeof(*vma_md));

			vma_md->old_pgoff = bo_bucket_ptr[i].bo_offset;
			vma_md->vma_entry = bo_bucket_ptr[i].bo_addr;

			tp_node = sys_get_node_by_gpu_id(&dest_topology, bo_bucket_ptr[i].gpu_id);
			if (!tp_node) {
				pr_err("Failed to find node with gpu_id:0x%04x\n", bo_bucket_ptr[i].gpu_id);
				fd = -ENODEV;
				goto clean;
			}
			vma_md->new_minor = tp_node->drm_render_minor;

			vma_md->new_pgoff = restored_bo_offsets_array[i];

			plugin_log_msg("amdgpu_plugin: adding vma_entry:addr:0x%lx old-off:0x%lx \
					new_off:0x%lx new_minor:%d\n", vma_md->vma_entry,
					vma_md->old_pgoff, vma_md->new_pgoff, vma_md->new_minor);

			list_add_tail(&vma_md->list, &update_vma_info_list);
		}
	} /* mmap done for VRAM BO */

	if (restore_hsakmt_shared_mem(e->shared_mem_size, e->shared_mem_magic)) {
		fd = -EBADFD;
		goto clean;
	}

	for (int i = 0; i < e->num_of_gpus; i++) {
		int ret, ret_thread = 0;

		thread_datas[i].gpu_id = devinfo_bucket_ptr[i].actual_gpu_id;
		thread_datas[i].bo_buckets = bo_bucket_ptr;
		thread_datas[i].bo_info_test = e->bo_info_test;
		thread_datas[i].pid = e->pid;
		thread_datas[i].num_of_bos = e->num_of_bos;
		thread_datas[i].restored_bo_offsets = restored_bo_offsets_array;
		thread_datas[i].drm_fd = devinfo_bucket_ptr[i].drm_fd;

		ret = amdgpu_device_initialize(thread_datas[i].drm_fd, &major, &minor, &h_dev);
		if (ret) {
			pr_perror("failed to initialize device");
			fd = -EBADFD;
			goto clean;
		}

		thread_datas[i].h_dev = h_dev;
		ret_thread = pthread_create(&thread_datas[i].thread, NULL, restore_bo_contents, (void*) &thread_datas[i]);
		if (ret_thread) {
			pr_err("Failed to create thread[%i]\n", i);
			fd = -ret_thread;
			goto clean;
		}
	}

	for (int i = 0; i < e->num_of_gpus; i++) {
		pthread_join(thread_datas[i].thread, NULL);
		pr_info("Thread[0x%x] finished ret:%d\n", thread_datas[i].gpu_id, thread_datas[i].ret);
		amdgpu_device_deinitialize(thread_datas[i].h_dev);

		if (devinfo_bucket_ptr[i].drm_fd >= 0)
			close(devinfo_bucket_ptr[i].drm_fd);

		if (thread_datas[i].ret) {
			fd = thread_datas[i].ret;
			goto clean;
		}
	}

clean:
	xfree(devinfo_bucket_ptr);
	if (ev_bucket_ptr)
		xfree(ev_bucket_ptr);
	if (q_bucket_ptr)
		xfree(q_bucket_ptr);
	xfree(restored_bo_offsets_array);
	xfree(bo_bucket_ptr);
	xfree(buf);
	if (args.queues_data_ptr)
		xfree((void*)args.queues_data_ptr);

	criu_kfd__free_unpacked(e, NULL);
	pr_info("amdgpu_plugin: returning kfd fd from plugin, fd = %d\n", fd);
	return fd;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__RESTORE_EXT_FILE, amdgpu_plugin_restore_file)

/* return 0 if no match found
 * return -1 for error.
 * return 1 if vmap map must be adjusted. */
int amdgpu_plugin_update_vmamap(const char *old_path, char *new_path, const uint64_t addr,
				const uint64_t old_offset, uint64_t *new_offset)
{
	struct vma_metadata *vma_md;
	char path[PATH_MAX];
	char *p_begin;
	char *p_end;
	bool is_kfd = false, is_renderD = false;


	pr_info("amdgpu_plugin: Enter %s\n", __func__);

	strncpy(path, old_path, sizeof(path));

	p_begin = path;
	p_end = p_begin + strlen(path);

	/*
	 * Paths sometimes have double forward slashes (e.g //dev/dri/renderD*)
	 * replace all '//' with '/'.
	 */
	while (p_begin < p_end - 1) {
		if (*p_begin == '/' && *(p_begin + 1) == '/')
			memmove(p_begin, p_begin + 1, p_end - p_begin);
		else
			p_begin++;
	}

	if (!strncmp(path, "/dev/dri/renderD", strlen("/dev/dri/renderD")))
		is_renderD = true;

	if (!strcmp(path, "/dev/kfd"))
		is_kfd = true;

	if (!is_renderD && !is_kfd) {
		pr_info("Skipping unsupported path:%s addr:%lx old_offset:%lx\n", old_path, addr, old_offset);
		return 0;
	}

	list_for_each_entry(vma_md, &update_vma_info_list, list) {
		if (addr == vma_md->vma_entry && old_offset == vma_md->old_pgoff) {
			*new_offset = vma_md->new_pgoff;

			if (is_renderD)
				sprintf(new_path, "/dev/dri/renderD%d", vma_md->new_minor);
			else
				strcpy(new_path, old_path);

			pr_info("amdgpu_plugin: old_pgoff= 0x%lx new_pgoff = 0x%lx old_path = %s new_path = %s\n",
				vma_md->old_pgoff, vma_md->new_pgoff, old_path, new_path);

			return 1;
		}
	}
	pr_info("No match for addr:0x%lx offset:%lx\n", addr, old_offset);
	return 0;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__UPDATE_VMA_MAP, amdgpu_plugin_update_vmamap)

int amdgpu_plugin_resume_devices_late(int target_pid)
{
	struct kfd_ioctl_criu_resume_args args = {0};
	int fd, ret = 0;

	pr_info("amdgpu_plugin: Inside %s for target pid = %d\n", __func__, target_pid);

	fd = open("/dev/kfd", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		pr_perror("failed to open kfd in plugin");
		return -1;
	}

	args.pid = target_pid;
	pr_info("amdgpu_plugin: Calling IOCTL to start notifiers and queues\n");
	if (kmtIoctl(fd, AMDKFD_IOC_CRIU_RESUME, &args) == -1) {
		pr_perror("restore late ioctl failed\n");
		ret = -1;
	}

	close(fd);
	return ret;
}

CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__RESUME_DEVICES_LATE, amdgpu_plugin_resume_devices_late)
