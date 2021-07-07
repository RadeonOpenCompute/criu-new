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

#include "criu-plugin.h"
#include "criu-amdgpu.pb-c.h"

#include "kfd_ioctl.h"
#include "xmalloc.h"
#include "criu-log.h"

#include "common/list.h"
#include "amdgpu_plugin_topology.h"

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
	for (int i = 0; i < e->n_bo_entries; i++) {
		if (e->bo_entries[i]) {
			if (e->bo_entries[i]->private_data.data)
				xfree(e->bo_entries[i]->private_data.data);

			if (e->bo_entries[i]->rawdata.data)
				xfree(e->bo_entries[i]->rawdata.data);

			xfree(e->bo_entries[i]);
		}
	}

	for (int i = 0; i < e->n_device_entries; i++) {
		if (e->device_entries[i]) {
			if (e->device_entries[i]->private_data.data)
				xfree(e->device_entries[i]->private_data.data);

			for (int j = 0; j < e->device_entries[i]->n_iolinks; j++)
				xfree(e->device_entries[i]->iolinks[j]);

			xfree(e->device_entries[i]);
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

	if (e->process_entry) {
		if (e->process_entry->private_data.data)
			xfree(e->process_entry->private_data.data);

		xfree(e->process_entry);
	}
	xfree(e);
}

static int allocate_process_entry(CriuKfd *e)
{
	ProcessEntry *entry = xzalloc(sizeof(*entry));
	if (!entry) {
		pr_err("Failed to allocate entry\n");
		return -ENOMEM;
	}

	process_entry__init(entry);
	e->process_entry = entry;
	return 0;
}

static int allocate_device_entries(CriuKfd *e, int num_of_devices)
{
	e->device_entries = xmalloc(sizeof(DeviceEntry*) * num_of_devices);
	if (!e->device_entries) {
		pr_err("Failed to allocate device_entries\n");
		return -ENOMEM;
	}

	for (int i = 0; i < num_of_devices; i++)
	{
		DeviceEntry *entry = xzalloc(sizeof(*entry));
		if (!entry) {
			pr_err("Failed to allocate entry\n");
			return -ENOMEM;
		}

		device_entry__init(entry);

		e->device_entries[i] = entry;
		e->n_device_entries++;
	}
	return 0;
}

static int allocate_bo_entries(CriuKfd *e, int num_bos, struct kfd_criu_bo_bucket *bo_bucket_ptr)
{
	e->bo_entries = xmalloc(sizeof(BoEntry*) * num_bos);
	if (!e->bo_entries) {
		pr_err("Failed to allocate bo_info\n");
		return -ENOMEM;
	}

	for (int i = 0; i < num_bos; i++) {
		BoEntry *entry = xzalloc(sizeof(*entry));
		if (!entry) {
			pr_err("Failed to allocate botest\n");
			return -ENOMEM;
		}

		bo_entry__init(entry);

		if ((bo_bucket_ptr)[i].alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_VRAM ||
		    (bo_bucket_ptr)[i].alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_GTT) {
			entry->rawdata.data = xmalloc((bo_bucket_ptr)[i].size);
			entry->rawdata.len = (bo_bucket_ptr)[i].size;
		}

		e->bo_entries[i] = entry;
		e->n_bo_entries++;

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

int topology_to_devinfo(struct tp_system *sys,
			struct device_maps *maps,
			DeviceEntry **deviceEntries)
{
	uint32_t devinfo_index = 0;
	struct tp_node *node;

	list_for_each_entry(node, &sys->nodes, listm_system) {
		DeviceEntry *devinfo = deviceEntries[devinfo_index++];

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

int devinfo_to_topology(DeviceEntry *devinfos[], uint32_t num_devices, struct tp_system *sys)
{
	for (int i = 0; i < num_devices; i++) {
		struct tp_node *node;
		DeviceEntry *devinfo = devinfos[i];

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
	struct kfd_criu_bo_bucket *bo_buckets;
	BoEntry **bo_entries;
	int drm_fd;
	int ret;
};

void *dump_bo_contents(void *_thread_data)
{
	int i, ret = 0;
	int num_bos = 0;
	struct thread_data* thread_data = (struct thread_data*) _thread_data;
	struct kfd_criu_bo_bucket *bo_buckets = thread_data->bo_buckets;
	BoEntry **bo_info = thread_data->bo_entries;
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
		if (!(bo_buckets[i].alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_VRAM) &&
		    !(bo_buckets[i].alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_GTT))
			continue;

		if (bo_info[i]->alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC) {
			void *addr;

			plugin_log_msg("amdgpu_plugin: large bar read possible\n");

			addr = mmap(NULL,
					bo_buckets[i].size,
					PROT_READ,
					MAP_SHARED,
					thread_data->drm_fd,
					bo_buckets[i].offset);
			if (addr == MAP_FAILED) {
				pr_perror("amdgpu_plugin: mmap failed\n");
				ret = -errno;
				goto exit;
			}

			/* direct memcpy is possible on large bars */
			memcpy(bo_info[i]->rawdata.data, addr, bo_buckets[i].size);
			munmap(addr, bo_buckets[i].size);
		} else {
			plugin_log_msg("Now try reading BO contents with /proc/pid/mem\n");
			if (lseek (mem_fd, (off_t) bo_buckets[i].addr, SEEK_SET) == -1) {
				pr_perror("Can't lseek for BO offset for pid = %d", thread_data->pid);
				ret = -errno;
				goto exit;
			}
			plugin_log_msg("Try to read file now\n");

			if (read(mem_fd, bo_info[i]->rawdata.data,
				bo_info[i]->size) != bo_info[i]->size) {
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
	struct kfd_criu_bo_bucket *bo_buckets = thread_data->bo_buckets;
	BoEntry **bo_info = thread_data->bo_entries;
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

		if (!(bo_buckets[i].alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_VRAM) &&
			!(bo_buckets[i].alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_GTT))
			continue;

		if (bo_info[i]->alloc_flags & KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC) {

			plugin_log_msg("amdgpu_plugin: large bar write possible\n");

			addr = mmap(NULL,
					bo_buckets[i].size,
					PROT_WRITE,
					MAP_SHARED,
					thread_data->drm_fd,
					bo_buckets[i].restored_offset);
			if (addr == MAP_FAILED) {
				pr_perror("amdgpu_plugin: mmap failed\n");
				ret = -errno;
				goto exit;
			}

			/* direct memcpy is possible on large bars */
			memcpy(addr, (void *)bo_info[i]->rawdata.data, bo_info[i]->size);
			munmap(addr, bo_info[i]->size);
		} else {
			/* Use indirect host data path via /proc/pid/mem on small pci bar GPUs or
			 * for Buffer Objects that don't have HostAccess permissions.
			 */
			plugin_log_msg("amdgpu_plugin: using PROCPIDMEM to restore BO contents\n");
			addr = mmap(NULL,
				    bo_info[i]->size,
				    PROT_NONE,
				    MAP_SHARED,
				    thread_data->drm_fd,
				    bo_buckets[i].restored_offset);

			if (addr == MAP_FAILED) {
				pr_perror("amdgpu_plugin: mmap failed\n");
				ret = -errno;
				goto exit;
			}

			if (lseek (mem_fd, (off_t) addr, SEEK_SET) == -1) {
				pr_perror("Can't lseek for BO offset for pid = %d", thread_data->pid);
				ret = -errno;
				goto exit;
			}

			plugin_log_msg("Attempt writting now\n");
			if (write(mem_fd, bo_info[i]->rawdata.data, bo_info[i]->size) !=
			    bo_info[i]->size) {
				pr_perror("Can't write buffer\n");
				ret = -errno;
				goto exit;
			}
			munmap(addr, bo_info[i]->size);
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

static int init_dumper_args(struct kfd_ioctl_criu_dumper_args *args, __u32 type, __u64 index_start,
			    __u64 num_objects, __u64 objects_size)
{
	memset(args, 0, sizeof(*args));

	args->type = type;
	/* Partial object lists not supported for now so index_start should always be 0 */
	args->objects_index_start = index_start;

	args->num_objects = num_objects;
	args->objects_size = objects_size;

	args->objects = (uintptr_t)xzalloc(args->objects_size);
	if (!args->objects)
		return -ENOMEM;

	return 0;
}

static int init_restorer_args(struct kfd_ioctl_criu_restorer_args *args, __u32 type,
			      __u64 index_start, __u64 num_objects, __u64 objects_size)
{
	memset(args, 0, sizeof(*args));

	args->type = type;
	/* Partial object lists not supported for now so index_start should always be 0 */
	args->objects_index_start = index_start;

	args->num_objects = num_objects;
	args->objects_size = objects_size;

	args->objects = (uintptr_t)xzalloc(args->objects_size);
	if (!args->objects)
		return -ENOMEM;

	return 0;
}


static int dump_process(int fd, struct kfd_ioctl_criu_process_info_args *info_args, CriuKfd *e)
{
	struct kfd_criu_process_bucket *process_bucket;
	struct kfd_ioctl_criu_dumper_args args;
	uint8_t *priv_data;
	int ret = 0;

	pr_debug("Dump process\n");

	ret = init_dumper_args(&args, KFD_CRIU_OBJECT_TYPE_PROCESS, 0, 1,
			 sizeof(*process_bucket) + info_args->process_priv_data_size);

	if (ret)
		goto exit;

	ret = kmtIoctl(fd, AMDKFD_IOC_CRIU_DUMPER, &args);
	if (ret) {
		pr_perror("amdgpu_plugin: Failed to call dumper (process) ioctl");
		goto exit;
	}

	ret = allocate_process_entry(e);
	if (ret)
		goto exit;

	process_bucket = (struct kfd_criu_process_bucket*)args.objects;
	/* First private data starts after all buckets */
	priv_data = (uint8_t *)args.objects + sizeof(*process_bucket);

	e->process_entry->private_data.len = process_bucket->priv_data_size;
	e->process_entry->private_data.data = xmalloc(e->process_entry->private_data.len);
	if (!e->process_entry->private_data.data) {
		ret = -ENOMEM;
		goto exit;
	}

	memcpy(e->process_entry->private_data.data,
		priv_data + process_bucket->priv_data_offset,
		e->process_entry->private_data.len);
exit:
	xfree((void*)args.objects);
	pr_info("Dumped process %s (ret:%d)\n", ret ? "Failed" : "Ok", ret);
	return ret;
}

static int dump_devices(int fd,
			struct kfd_ioctl_criu_process_info_args *info_args,
			CriuKfd *e)
{
	struct kfd_criu_device_bucket *device_buckets;
	struct kfd_ioctl_criu_dumper_args args;
	uint8_t *priv_data;
	int ret = 0, i;

	pr_debug("Dumping %d devices\n", info_args->total_devices);

	ret = init_dumper_args(&args, KFD_CRIU_OBJECT_TYPE_DEVICE, 0, info_args->total_devices,
			       (info_args->total_devices * sizeof(*device_buckets)) +
				info_args->devices_priv_data_size);
	if (ret)
		goto exit;

	ret = kmtIoctl(fd, AMDKFD_IOC_CRIU_DUMPER, &args);
	if (ret) {
		pr_perror("amdgpu_plugin: Failed to call dumper (devices) ioctl");
		goto exit;
	}

	device_buckets = (struct kfd_criu_device_bucket*)args.objects;
	/* First private data starts after all buckets */
	priv_data = (uint8_t *)args.objects + (args.num_objects * sizeof(*device_buckets));

	/* When checkpointing on a node where there was already a checkpoint-restore before, the
	 * user_gpu_id and actual_gpu_id will be different.
	 *
	 * We store the user_gpu_id in the stored image files so that the stored images always have
	 * the gpu_id's of the node where the application was first launched.
	 */
	for (int i = 0; i < args.num_objects; i++)
		maps_add_gpu_entry(&checkpoint_maps,
				   device_buckets[i].actual_gpu_id,
				   device_buckets[i].user_gpu_id);

	e->num_of_gpus = info_args->total_devices;
	e->num_of_cpus = src_topology.num_nodes - info_args->total_devices;

	/* The ioctl will only return entries for GPUs, but we also store entries for CPUs and the
	 * information for CPUs is obtained from parsing system topology
	 */
	ret = allocate_device_entries(e, src_topology.num_nodes);
	if (ret) {
		ret = -ENOMEM;
		goto exit;
	}

	pr_debug("Number of CPUs:%d GPUs:%d\n", e->num_of_cpus, e->num_of_gpus);

	/* Store topology information that was obtained from parsing /sys/class/kfd/kfd/topology/ */
	ret = topology_to_devinfo(&src_topology, &checkpoint_maps, e->device_entries);
	if (ret)
		goto exit;

	/* Add private data obtained from IOCTL for each GPU */
	for (i = 0; i < args.num_objects; i++) {
		int j;
		struct kfd_criu_device_bucket *device_bucket = &device_buckets[i];
		pr_debug("Device[%d] user_gpu_id:%x actual_gpu_id:%x\n",
					i,
					device_bucket->user_gpu_id,
					device_bucket->actual_gpu_id);

		for (j = 0; j < src_topology.num_nodes; j++) {
			DeviceEntry *devinfo = e->device_entries[j];
			if (device_bucket->user_gpu_id != devinfo->gpu_id)
				continue;

			devinfo->private_data.len = device_bucket->priv_data_size;
			devinfo->private_data.data = xmalloc(devinfo->private_data.len);

			if (!devinfo->private_data.data) {
				ret = -ENOMEM;
				goto exit;
			}
			memcpy(devinfo->private_data.data,
				priv_data + device_bucket->priv_data_offset,
				devinfo->private_data.len);
		}
	}
exit:
	xfree((void*)args.objects);
	pr_info("Dumped devices %s (ret:%d)\n", ret ? "Failed" : "Ok", ret);
	return ret;
}

static int dump_bos(int fd, struct kfd_ioctl_criu_process_info_args *info_args, CriuKfd *e)
{
	struct kfd_ioctl_criu_dumper_args args = {0};
	struct kfd_criu_bo_bucket *bo_buckets;
	struct thread_data *thread_datas;
	uint8_t *priv_data;
	int ret = 0, i;

	pr_debug("Dumping %lld BOs\n", info_args->total_bos);

	thread_datas = xzalloc(sizeof(*thread_datas) * e->num_of_gpus);
	if (!thread_datas) {
		ret = -ENOMEM;
		goto exit;
	}

	ret = init_dumper_args(&args, KFD_CRIU_OBJECT_TYPE_BO, 0, info_args->total_bos,
			       (info_args->total_bos * sizeof(*bo_buckets)) +
			        info_args->bos_priv_data_size);

	if (ret)
		goto exit;

	ret = kmtIoctl(fd, AMDKFD_IOC_CRIU_DUMPER, &args);
	if (ret) {
		pr_perror("amdgpu_plugin: Failed to call dumper (bos) ioctl");
		goto exit;
	}

	bo_buckets = (struct kfd_criu_bo_bucket*)args.objects;
	/* First private data starts after all buckets */
	priv_data = (uint8_t *)args.objects + (args.num_objects * sizeof(*bo_buckets));

	e->num_of_bos = info_args->total_bos;
	ret = allocate_bo_entries(e, e->num_of_bos, bo_buckets);
	if (ret) {
		ret = -ENOMEM;
		goto exit;
	}

	for (i = 0; i < args.num_objects; i++) {
		struct kfd_criu_bo_bucket *bo_bucket = &bo_buckets[i];
		BoEntry *boinfo = e->bo_entries[i];

		boinfo->private_data.len = bo_bucket->priv_data_size;
		boinfo->private_data.data = xmalloc(boinfo->private_data.len);

		if (!boinfo->private_data.data) {
			ret = -ENOMEM;
			goto exit;
		}
		memcpy(boinfo->private_data.data,
			priv_data + bo_bucket->priv_data_offset,
			boinfo->private_data.len);

		plugin_log_msg("BO [%d] gpu_id:%x addr:%llx size:%llx offset:%llx dmabuf_handle:%d\n",
					i,
					bo_bucket->gpu_id,
					bo_bucket->addr,
					bo_bucket->size,
					bo_bucket->offset,
					bo_bucket->dmabuf_handle);

		boinfo->gpu_id = maps_get_dest_gpu(&checkpoint_maps, bo_bucket->gpu_id);
		if (!boinfo->gpu_id) {
			ret = -ENODEV;
			goto exit;
		}
		boinfo->addr = bo_bucket->addr;
		boinfo->size = bo_bucket->size;
		boinfo->offset = bo_bucket->offset;
		boinfo->alloc_flags = bo_bucket->alloc_flags;
	}

	for (int i = 0; i < e->num_of_gpus; i++) {
		struct tp_node *dev;
		int ret_thread = 0;

		dev = sys_get_node_by_index(&src_topology, i);
		if (!dev) {
			ret = -ENODEV;
			goto exit;
		}

		thread_datas[i].gpu_id = dev->gpu_id;
		thread_datas[i].bo_buckets = bo_buckets;
		thread_datas[i].bo_entries = e->bo_entries;
		thread_datas[i].pid = e->pid;
		thread_datas[i].num_of_bos = info_args->total_bos;
		thread_datas[i].drm_fd = node_get_drm_render_device(dev);
		if (thread_datas[i].drm_fd < 0) {
			ret = thread_datas[i].drm_fd;
			goto exit;
		}

		ret_thread = pthread_create(&thread_datas[i].thread, NULL, dump_bo_contents, (void*) &thread_datas[i]);
		if (ret_thread) {
			pr_err("Failed to create thread[%i]\n", i);
			ret = -ret_thread;
			goto exit;
		}
	}

	for (int i = 0; i <  e->num_of_gpus; i++) {
		pthread_join(thread_datas[i].thread, NULL);
		pr_info("Thread[0x%x] finished ret:%d\n", thread_datas[i].gpu_id, thread_datas[i].ret);

		if (thread_datas[i].drm_fd >= 0)
			close(thread_datas[i].drm_fd);

		if (thread_datas[i].ret) {
			ret = thread_datas[i].ret;
			goto exit;
		}
	}
exit:
	xfree((void*)args.objects);
	xfree(thread_datas);
	pr_info("Dumped bos %s (ret:%d)\n", ret ? "failed" : "ok", ret);
	return ret;
}

int amdgpu_plugin_dump_file(int fd, int id)
{
	struct kfd_ioctl_criu_process_info_args info_args = {0};
	struct kfd_ioctl_criu_dumper_args args = {0};
	struct kfd_criu_q_bucket *q_bucket_ptr;
	struct kfd_criu_ev_bucket *ev_buckets_ptr = NULL;
	int ret;
	char img_path[PATH_MAX];
	struct stat st, st_kfd;
	unsigned char *buf;
	size_t len;

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

	if (kmtIoctl(fd, AMDKFD_IOC_CRIU_PROCESS_INFO, &info_args) == -1) {
		pr_perror("amdgpu_plugin: Failed to call process info ioctl");
		return -1;
	}

	pr_info("amdgpu_plugin: devices:%d bos:%lld queues:%d events:%d svm-range:%lld\n",
			info_args.total_devices, info_args.total_bos, info_args.total_queues,
			info_args.total_events, info_args.total_svm_ranges);

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
		return -1;
	}

	pr_info("amdgpu_plugin: success in calling dumper ioctl\n");

	e = xmalloc(sizeof(*e));
	if (!e) {
		pr_err("Failed to allocate proto structure\n");
		return -ENOMEM;
	}

	criu_kfd__init(e);
	e->pid = info_args.task_pid;

	ret = dump_process(fd, &info_args, e);
	if (ret)
		goto exit;

	ret = dump_devices(fd, &info_args, e);
	if (ret)
		goto exit;

	ret = dump_bos(fd, &info_args, e);
	if (ret)
		goto exit;

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
exit:
failed:
	sys_close_drm_render_devices(&src_topology);
	xfree(q_bucket_ptr);
	if (ev_buckets_ptr)
		xfree(ev_buckets_ptr);
	free_e(e);
	pr_info("amdgpu_plugin: Exiting from dumper for fd = %d\n", major(st.st_rdev));
        return ret;

}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__DUMP_EXT_FILE, amdgpu_plugin_dump_file)

static int restore_process(int fd, CriuKfd *e)
{
	struct kfd_criu_process_bucket *process_bucket;
	struct kfd_ioctl_criu_restorer_args args;
	uint8_t *priv_data;
	int ret = 0;

	pr_debug("Restore process\n");

	ret = init_restorer_args(&args, KFD_CRIU_OBJECT_TYPE_PROCESS, 0, 1,
			sizeof(*process_bucket) + e->process_entry->private_data.len);

	if (ret)
		goto exit;

	process_bucket = (struct kfd_criu_process_bucket*)args.objects;
	/* First private data starts after all buckets */
	priv_data = (uint8_t *)args.objects + sizeof(*process_bucket);

	process_bucket->priv_data_offset = 0;
	process_bucket->priv_data_size = e->process_entry->private_data.len;

	memcpy(priv_data, e->process_entry->private_data.data, e->process_entry->private_data.len);

	ret = kmtIoctl(fd, AMDKFD_IOC_CRIU_RESTORER, &args);
	if (ret) {
		pr_perror("amdgpu_plugin: Failed to call restorer (process) ioctl");
		goto exit;
	}

exit:
	pr_info("Restore process %s (ret:%d)\n", ret ? "Failed" : "Ok", ret);
	return ret;
}

/* Restore per-device information */
static int restore_devices(int fd, CriuKfd *e)
{
	struct kfd_ioctl_criu_restorer_args args = {0};
	struct kfd_criu_device_bucket *device_buckets;
	int ret = 0, bucket_index = 0;
	uint64_t priv_data_offset = 0;
	uint64_t objects_size = 0;
	uint8_t *priv_data;

	pr_debug("Restoring %d devices\n", e->num_of_gpus);

	for (int i = 0; i < e->num_of_cpus + e->num_of_gpus; i++) {
		/* Skip CPUs */
		if (!e->device_entries[i]->gpu_id)
			continue;

		objects_size += sizeof(*device_buckets) +
				     e->device_entries[i]->private_data.len;
	}

	ret = init_restorer_args(&args, KFD_CRIU_OBJECT_TYPE_DEVICE, 0, e->num_of_gpus, objects_size);
	if (ret)
		goto exit;

	device_buckets = (struct kfd_criu_device_bucket*) args.objects;
	priv_data = (uint8_t *)args.objects + (args.num_objects * sizeof(*device_buckets));

	for (int entries_i = 0; entries_i < e->num_of_cpus + e->num_of_gpus; entries_i++) {
		struct kfd_criu_device_bucket *device_bucket;
		DeviceEntry *devinfo = e->device_entries[entries_i];
		struct tp_node *tp_node;

		if (!devinfo->gpu_id)
			continue;

		device_bucket = &device_buckets[bucket_index++];

		device_bucket->priv_data_size = devinfo->private_data.len;
		device_bucket->priv_data_offset = priv_data_offset;

		priv_data_offset += device_bucket->priv_data_size;

		memcpy(priv_data + device_bucket->priv_data_offset,
		       devinfo->private_data.data,
		       device_bucket->priv_data_size);

		device_bucket->user_gpu_id = devinfo->gpu_id;
		device_bucket->actual_gpu_id = maps_get_dest_gpu(&restore_maps, devinfo->gpu_id);
		if (!device_bucket->actual_gpu_id) {
			ret = -ENODEV;
			goto exit;
		}

		tp_node = sys_get_node_by_gpu_id(&dest_topology, device_bucket->actual_gpu_id);
		if (!tp_node) {
			ret = -ENODEV;
			goto exit;
		}

		device_bucket->drm_fd = node_get_drm_render_device(tp_node);
		if (device_bucket->drm_fd < 0) {
			ret = device_bucket->drm_fd;
			goto exit;
		}
	}

	ret = kmtIoctl(fd, AMDKFD_IOC_CRIU_RESTORER, &args);
	if (ret) {
		pr_perror("amdgpu_plugin: Failed to call restorer (devices) ioctl");
		goto exit;
	}

exit:
	xfree((void*)args.objects);
	pr_info("Restore devices %s (ret:%d)\n", ret ? "Failed" : "Ok", ret);
	return ret;
}

static int restore_bos(int fd, CriuKfd *e)
{
	struct kfd_ioctl_criu_restorer_args args = {0};
	struct kfd_criu_bo_bucket *bo_buckets;
	struct thread_data *thread_datas;
	uint64_t priv_data_offset = 0;
	uint64_t objects_size = 0;
	uint8_t *priv_data;
	int ret = 0;

	pr_debug("Restoring %ld BOs\n", e->num_of_bos);

	thread_datas = xzalloc(sizeof(*thread_datas) * e->num_of_gpus);
	if (!thread_datas) {
		ret = -ENOMEM;
		goto exit;
	}

	for (int i = 0; i < e->num_of_bos; i++)
		objects_size += sizeof(*bo_buckets) + e->bo_entries[i]->private_data.len;

	ret = init_restorer_args(&args, KFD_CRIU_OBJECT_TYPE_BO, 0, e->num_of_bos, objects_size);
	if (ret)
		goto exit;

	bo_buckets = (struct kfd_criu_bo_bucket*) args.objects;
	priv_data = (uint8_t *)args.objects + (args.num_objects * sizeof(*bo_buckets));

	for (int i = 0; i < args.num_objects; i++) {
		struct kfd_criu_bo_bucket *bo_bucket = &bo_buckets[i];
		BoEntry *boinfo = e->bo_entries[i];

		bo_bucket->priv_data_size = boinfo->private_data.len;
		bo_bucket->priv_data_offset = priv_data_offset;
		priv_data_offset += bo_bucket->priv_data_size;

		memcpy(priv_data + bo_bucket->priv_data_offset,
		       boinfo->private_data.data,
		       bo_bucket->priv_data_size);

		bo_bucket->gpu_id = maps_get_dest_gpu(&restore_maps, boinfo->gpu_id);
		if (!bo_bucket->gpu_id) {
			ret = -ENODEV;
			goto exit;
		}

		bo_bucket->addr = boinfo->addr;
		bo_bucket->size = boinfo->size;
		bo_bucket->offset = boinfo->offset;
		bo_bucket->alloc_flags = boinfo->alloc_flags;

		plugin_log_msg("BO [%d] gpu_id:%x addr:%llx size:%llx offset:%llx\n",
					i,
					bo_bucket->gpu_id,
					bo_bucket->addr,
					bo_bucket->size,
					bo_bucket->offset);
	}

	ret = kmtIoctl(fd, AMDKFD_IOC_CRIU_RESTORER, &args);
	if (ret) {
		pr_perror("amdgpu_plugin: Failed to call restorer (bos) ioctl");
		goto exit;
	}

	for (int i = 0; i < args.num_objects; i++) {
		struct kfd_criu_bo_bucket *bo_bucket = &bo_buckets[i];
		if (bo_bucket->alloc_flags & (KFD_IOC_ALLOC_MEM_FLAGS_VRAM |
					      KFD_IOC_ALLOC_MEM_FLAGS_GTT |
					      KFD_IOC_ALLOC_MEM_FLAGS_MMIO_REMAP |
					      KFD_IOC_ALLOC_MEM_FLAGS_DOORBELL)) {

			struct tp_node *tp_node;
			struct vma_metadata *vma_md;
			vma_md = xmalloc(sizeof(*vma_md));
			if (!vma_md) {
				ret = -ENOMEM;
				goto exit;
			}

			memset(vma_md, 0, sizeof(*vma_md));

			vma_md->old_pgoff = bo_bucket->offset;
			vma_md->vma_entry = bo_bucket->addr;

			tp_node = sys_get_node_by_gpu_id(&dest_topology, bo_bucket->gpu_id);
			if (!tp_node) {
				pr_err("Failed to find node with gpu_id:0x%04x\n", bo_bucket->gpu_id);
				ret = -ENODEV;
				goto exit;
			}

			vma_md->new_minor = tp_node->drm_render_minor;
			vma_md->new_pgoff = bo_bucket->restored_offset;

			plugin_log_msg("amdgpu_plugin: adding vma_entry:addr:0x%lx old-off:0x%lx\
					new_off:0x%lx new_minor:%d\n", vma_md->vma_entry,
					vma_md->old_pgoff, vma_md->new_pgoff, vma_md->new_minor);

			list_add_tail(&vma_md->list, &update_vma_info_list);
		}
	}

	for (int i = 0; i < e->num_of_gpus; i++) {
		struct tp_node *dev;
		int ret_thread = 0;

		dev = sys_get_node_by_index(&dest_topology, i);
		if (!dev) {
			ret = -ENODEV;
			goto exit;
		}

		thread_datas[i].gpu_id = dev->gpu_id;
		thread_datas[i].bo_buckets = bo_buckets;
		thread_datas[i].bo_entries = e->bo_entries;
		thread_datas[i].pid = e->pid;
		thread_datas[i].num_of_bos = e->num_of_bos;

		thread_datas[i].drm_fd = node_get_drm_render_device(dev);
		if (thread_datas[i].drm_fd < 0) {
			ret = -thread_datas[i].drm_fd;
			goto exit;
		}

		ret_thread = pthread_create(&thread_datas[i].thread, NULL, restore_bo_contents, (void*) &thread_datas[i]);
		if (ret_thread) {
			pr_err("Failed to create thread[%i]\n", i);
			fd = -ret_thread;
			goto exit;
		}
	}

	for (int i = 0; i < e->num_of_gpus; i++) {
		pthread_join(thread_datas[i].thread, NULL);
		pr_info("Thread[0x%x] finished ret:%d\n", thread_datas[i].gpu_id, thread_datas[i].ret);

		if (thread_datas[i].ret) {
			ret = thread_datas[i].ret;
			goto exit;
		}
	}
exit:
	xfree(thread_datas);
	xfree((void*)args.objects);
	pr_info("Restore BOs %s (ret:%d)\n", ret ? "Failed" : "Ok", ret);
	return ret;
}

int amdgpu_plugin_restore_file(int id)
{
	int ret = 0, fd;
	struct kfd_ioctl_criu_restorer_args args = {0};
	struct kfd_criu_q_bucket *q_bucket_ptr;
	struct kfd_criu_ev_bucket *ev_bucket_ptr = NULL;
	char img_path[PATH_MAX];
	struct stat filestat;
	unsigned char *buf;
	CriuRenderNode *rd;
	CriuKfd *e;

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

		fd = node_get_drm_render_device(tp_node);
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

	if (set_restore_gpu_maps(&src_topology, &dest_topology, &restore_maps)) {
		fd = -EBADFD;
		goto clean;
	}

	ret = restore_process(fd, e);
	if (ret)
		goto exit;

	ret = restore_devices(fd, e);
	if (ret)
		goto exit;

	ret = restore_bos(fd, e);
	if (ret)
		goto exit;

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

	if (restore_hsakmt_shared_mem(e->shared_mem_size, e->shared_mem_magic)) {
		fd = -EBADFD;
		goto clean;
	}

clean:
exit:
	sys_close_drm_render_devices(&dest_topology);
	if (ev_bucket_ptr)
		xfree(ev_bucket_ptr);
	if (q_bucket_ptr)
		xfree(q_bucket_ptr);
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
