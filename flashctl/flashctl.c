#include "flashctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#ifdef __ANDROID__
#include <android/log.h>
#define FL_LOG_TAG "libflashctl"
#define FL_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, FL_LOG_TAG, __VA_ARGS__)
#define FL_LOGI(...) __android_log_print(ANDROID_LOG_INFO, FL_LOG_TAG, __VA_ARGS__)
#else
#define FL_LOGE(fmt, ...) fprintf(stderr, "[flashctl ERROR] " fmt "\n", ##__VA_ARGS__)
#define FL_LOGI(fmt, ...) fprintf(stderr, "[flashctl INFO] " fmt "\n", ##__VA_ARGS__)
#endif

/* Device and Sysfs Paths */
#define SUBDEV_PATH      "/dev/v4l-subdev15"
#define VIDEO_PATH       "/dev/video0"
#define SYSFS_CHECK_PATH "/sys/class/video4linux/v4l-subdev15/name"

/* Safety Limits & Timers */
#define FL_MAX_LEVEL                44   /* 0x2C - verified stock maximum */
#define FL_WATCHDOG_TIMEOUT_SEC     5    /* 5s inactivity limit */
#define FL_MAX_ON_TIME_SEC          60   /* 60s max continuous on-time */
#define FL_COOLDOWN_SEC             15   /* 15s cooldown after watchdog cutoff */
#define FL_HEAT_WINDOW_SEC          120  /* 120s sliding window for cumulative heat */
#define FL_HEAT_MAX_ON_SEC          90   /* 90s max on-time within sliding window */

/* IOCTL Definitions */
#define BASE_VIDIOC_PRIVATE             192
#define VIDIOC_CAM_CONTROL              _IOWR('V', BASE_VIDIOC_PRIVATE, struct cam_control)

#define CAM_COMMON_OPCODE_BASE          0x100
#define CAM_ACQUIRE_DEV                 (CAM_COMMON_OPCODE_BASE + 0x2)  /* 258 */
#define CAM_START_DEV                   (CAM_COMMON_OPCODE_BASE + 0x3)  /* 259 */
#define CAM_STOP_DEV                    (CAM_COMMON_OPCODE_BASE + 0x4)  /* 260 */
#define CAM_CONFIG_DEV                  (CAM_COMMON_OPCODE_BASE + 0x5)  /* 261 */
#define CAM_RELEASE_DEV                 (CAM_COMMON_OPCODE_BASE + 0x6)  /* 262 */

#define CAM_COMMON_OPCODE_MAX           (CAM_COMMON_OPCODE_BASE + 0xa)  /* 266 */
#define CAM_REQ_MGR_CREATE_SESSION      (CAM_COMMON_OPCODE_MAX + 2)     /* 268 */
#define CAM_REQ_MGR_DESTROY_SESSION     (CAM_COMMON_OPCODE_MAX + 3)     /* 269 */
#define CAM_REQ_MGR_ALLOC_BUF           (CAM_COMMON_OPCODE_MAX + 9)     /* 275 */
#define CAM_REQ_MGR_RELEASE_BUF         (CAM_COMMON_OPCODE_MAX + 11)    /* 277 */

#define CAM_HANDLE_USER_POINTER         1

/* Flash packet opcodes */
#define CAM_FLASH_PACKET_OPCODE_INIT                   0
#define CAM_FLASH_PACKET_OPCODE_SET_OPS                1
#define CAM_FLASH_PACKET_OPCODE_NON_REALTIME_SET_OPS   2
#define CAM_FLASH_PACKET_OPCODE_STREAM_OFF             3

/* Command buffer types */
#define CAM_CMD_BUF_I2C                 7

/* Sensor command types */
#define CAMERA_SENSOR_CMD_TYPE_PWR_UP       2
#define CAMERA_SENSOR_CMD_TYPE_PWR_DOWN     3
#define CAMERA_SENSOR_CMD_TYPE_I2C_INFO     4
#define CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR  5
#define CAMERA_SENSOR_CMD_TYPE_WAIT         9

#define CAMERA_SENSOR_I2C_OP_RNDM_WR        1
#define CAMERA_SENSOR_I2C_TYPE_BYTE         1
#define CAMERA_SENSOR_WAIT_OP_SW_UCND       3

#define SENSOR_CUSTOM_GPIO2                 11

/* Memory alloc flags */
#define CAM_MEM_FLAG_HW_READ_WRITE      (1 << 0)
#define CAM_MEM_FLAG_CMD_BUF_TYPE       (1 << 3)
#define CAM_MEM_FLAG_KMD_ACCESS         (1 << 4)
#define CAM_MEM_FLAG_UMD_ACCESS         (1 << 6)
#define CAM_MEM_FLAG_CACHE              (1 << 10)
#define ALLOC_FLAGS (CAM_MEM_FLAG_CMD_BUF_TYPE | CAM_MEM_FLAG_KMD_ACCESS | \
                     CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_CACHE)

#define CAM_MEM_MMU_MAX_HANDLE          16

/* Kernel structures */
struct cam_control {
    uint32_t op_code;
    uint32_t size;
    uint32_t handle_type;
    uint32_t reserved;
    uint64_t handle;
};

struct cam_req_mgr_session_info {
    int32_t session_hdl;
    int32_t reserved;
};

struct cam_mem_alloc_out_params {
    uint32_t buf_handle;
    int32_t  fd;
    uint64_t vaddr;
};

struct cam_mem_mgr_alloc_cmd {
    uint64_t len;
    uint64_t align;
    int32_t  mmu_hdls[CAM_MEM_MMU_MAX_HANDLE];
    uint32_t num_hdl;
    uint32_t flags;
    struct cam_mem_alloc_out_params out;
};

struct cam_mem_mgr_release_cmd {
    int32_t  buf_handle;
    uint32_t reserved;
};

struct cam_sensor_acquire_dev {
    uint32_t session_handle;
    uint32_t device_handle;
    uint32_t handle_type;
    uint32_t reserved;
    uint64_t info_handle;
} __attribute__((packed));

struct cam_start_stop_dev_cmd {
    int32_t  session_handle;
    int32_t  dev_handle;
};

struct cam_release_dev_cmd {
    int32_t  session_handle;
    int32_t  dev_handle;
};

struct cam_config_dev_cmd {
    int32_t  session_handle;
    int32_t  dev_handle;
    uint64_t offset;
    uint64_t packet_handle;
};

struct cam_packet_header {
    uint32_t op_code;
    uint32_t size;
    uint64_t request_id;
    uint32_t flags;
    uint32_t padding;
};

struct cam_cmd_buf_desc {
    int32_t  mem_handle;
    uint32_t offset;
    uint32_t size;
    uint32_t length;
    uint32_t type;
    uint32_t meta_data;
};

struct cam_packet {
    struct cam_packet_header header;
    uint32_t cmd_buf_offset;
    uint32_t num_cmd_buf;
    uint32_t io_configs_offset;
    uint32_t num_io_configs;
    uint32_t patch_offset;
    uint32_t num_patches;
    uint32_t kmd_cmd_buf_index;
    uint32_t kmd_cmd_buf_offset;
    uint64_t payload[1];
};

struct cam_cmd_i2c_info {
    uint32_t slave_addr;
    uint8_t  i2c_freq_mode;
    uint8_t  cmd_type;
    uint16_t reserved;
} __attribute__((packed));

struct cam_power_settings {
    uint16_t power_seq_type;
    uint16_t reserved;
    uint32_t config_val_low;
    uint32_t config_val_high;
} __attribute__((packed));

struct cam_cmd_power {
    uint32_t count;
    uint8_t  reserved;
    uint8_t  cmd_type;
    uint16_t more_reserved;
    struct cam_power_settings power_settings[1];
} __attribute__((packed));

struct cam_cmd_unconditional_wait {
    int16_t  delay;
    int16_t  reserved;
    uint8_t  op_code;
    uint8_t  cmd_type;
    uint16_t reserved1;
} __attribute__((packed));

struct i2c_rdwr_header {
    uint32_t count;
    uint8_t  op_code;
    uint8_t  cmd_type;
    uint8_t  data_type;
    uint8_t  addr_type;
} __attribute__((packed));

struct i2c_random_wr_payload {
    uint32_t reg_addr;
    uint32_t reg_data;
} __attribute__((packed));

/* Memory buffer tracking */
struct mem_buffer {
    uint32_t handle;
    int32_t  fd;
    size_t   size;
    void    *ptr;
};

#define NUM_BUFFERS 10

/* Cumulative Heat Protection History */
#define MAX_HEAT_HISTORY 32
struct heat_interval {
    double start_mono;
    double end_mono;
};

/* Library Internal State */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool            g_is_on = false;
static bool            g_is_shutting_down = false;
static bool            g_device_started = false;
static int             g_current_level = 0;
static int             g_video_fd = -1;
static int             g_subdev_fd = -1;
static int32_t         g_session_hdl = 0;
static int32_t         g_dev_hdl = 0;
static struct mem_buffer g_bufs[NUM_BUFFERS];
static int             g_num_bufs_allocated = 0;

/* Off and On configurations */
static struct cam_config_dev_cmd g_cfg_torch_off;
static struct cam_config_dev_cmd g_cfg_torch_on;
static struct i2c_random_wr_payload *g_on_payload = NULL;

/* Watchdog & Safety Timers */
static pthread_t       g_wd_thread;
static bool            g_wd_thread_active = false;
static bool            g_wd_running = false;
static pthread_cond_t  g_wd_cond;
static bool            g_wd_cond_initialized = false;

static struct timespec g_on_start_time;
static struct timespec g_last_heartbeat;
static struct timespec g_cooldown_until = {0, 0};

static struct heat_interval g_heat_history[MAX_HEAT_HISTORY];
static int                 g_heat_count = 0;

/* Forward declarations */
static int fl_off_locked(bool triggered_by_watchdog);
static void *watchdog_thread_fn(void *arg);

static double timespec_to_double(const struct timespec *ts) {
    return (double)ts->tv_sec + (double)ts->tv_nsec / 1e9;
}

static double timespec_diff_sec(const struct timespec *start, const struct timespec *end) {
    return (double)(end->tv_sec - start->tv_sec) + (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

/* Calculate cumulative on-time in the last FL_HEAT_WINDOW_SEC (120s) */
static double calculate_cumulative_heat(double now_sec) {
    double window_start = now_sec - (double)FL_HEAT_WINDOW_SEC;
    double total = 0.0;

    for (int i = 0; i < g_heat_count; i++) {
        double s = g_heat_history[i].start_mono;
        double e = g_heat_history[i].end_mono;
        if (e < window_start) {
            continue;
        }
        if (s < window_start) {
            s = window_start;
        }
        if (e > s) {
            total += (e - s);
        }
    }

    if (g_is_on) {
        double cur_start = timespec_to_double(&g_on_start_time);
        if (cur_start < window_start) {
            cur_start = window_start;
        }
        if (now_sec > cur_start) {
            total += (now_sec - cur_start);
        }
    }

    return total;
}

static void record_heat_interval(double start_mono, double end_mono) {
    if (end_mono <= start_mono) return;

    if (g_heat_count < MAX_HEAT_HISTORY) {
        g_heat_history[g_heat_count].start_mono = start_mono;
        g_heat_history[g_heat_count].end_mono = end_mono;
        g_heat_count++;
    } else {
        /* Shift ring buffer */
        memmove(&g_heat_history[0], &g_heat_history[1], sizeof(struct heat_interval) * (MAX_HEAT_HISTORY - 1));
        g_heat_history[MAX_HEAT_HISTORY - 1].start_mono = start_mono;
        g_heat_history[MAX_HEAT_HISTORY - 1].end_mono = end_mono;
    }
}

static void ensure_cond_initialized(void) {
    if (!g_wd_cond_initialized) {
        pthread_condattr_t cattr;
        pthread_condattr_init(&cattr);
        pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
        pthread_cond_init(&g_wd_cond, &cattr);
        pthread_condattr_destroy(&cattr);
        g_wd_cond_initialized = true;
    }
}

static int alloc_buffer_internal(int video_fd, size_t size, struct mem_buffer *buf) {
    struct cam_mem_mgr_alloc_cmd alloc_cmd;
    memset(&alloc_cmd, 0, sizeof(alloc_cmd));
    alloc_cmd.len = (size + 31) & ~31;
    alloc_cmd.align = 32;
    alloc_cmd.num_hdl = 0;
    alloc_cmd.flags = ALLOC_FLAGS;

    struct cam_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op_code = CAM_REQ_MGR_ALLOC_BUF;
    ctrl.size = sizeof(alloc_cmd);
    ctrl.handle_type = CAM_HANDLE_USER_POINTER;
    ctrl.handle = (uint64_t)(uintptr_t)&alloc_cmd;

    if (ioctl(video_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
        int err = errno;
        FL_LOGE("ioctl CAM_REQ_MGR_ALLOC_BUF failed: errno %d (%s)", err, strerror(err));
        return -1;
    }

    buf->handle = alloc_cmd.out.buf_handle;
    buf->fd = alloc_cmd.out.fd;
    buf->size = alloc_cmd.len;
    buf->ptr = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, buf->fd, 0);
    if (buf->ptr == MAP_FAILED) {
        int err = errno;
        FL_LOGE("mmap failed for buffer handle 0x%x fd %d: errno %d (%s)",
                buf->handle, buf->fd, err, strerror(err));
        buf->ptr = NULL;
        return -1;
    }
    memset(buf->ptr, 0, buf->size);
    return 0;
}

static void free_buffer_internal(int video_fd, struct mem_buffer *buf) {
    if (buf->ptr && buf->ptr != MAP_FAILED) {
        munmap(buf->ptr, buf->size);
        buf->ptr = NULL;
    }
    if (buf->fd >= 0) {
        close(buf->fd);
        buf->fd = -1;
    }
    if (buf->handle && video_fd >= 0) {
        struct cam_mem_mgr_release_cmd rel_cmd;
        memset(&rel_cmd, 0, sizeof(rel_cmd));
        rel_cmd.buf_handle = buf->handle;

        struct cam_control ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.op_code = CAM_REQ_MGR_RELEASE_BUF;
        ctrl.size = sizeof(rel_cmd);
        ctrl.handle_type = CAM_HANDLE_USER_POINTER;
        ctrl.handle = (uint64_t)(uintptr_t)&rel_cmd;
        if (ioctl(video_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
            int err = errno;
            FL_LOGE("ioctl CAM_REQ_MGR_RELEASE_BUF failed for handle 0x%x: errno %d (%s)",
                    buf->handle, err, strerror(err));
        }
        buf->handle = 0;
    }
}

int fl_on(int level) {
    if (level < 1 || level > FL_MAX_LEVEL) {
        FL_LOGE("fl_on: level %d out of range (1..%d)", level, FL_MAX_LEVEL);
        return FL_ERR_RANGE;
    }

    pthread_mutex_lock(&g_mutex);

    /* 0. Hardware Sanity Check (must be cam-flash) */
    int chk_fd = open(SYSFS_CHECK_PATH, O_RDONLY);
    if (chk_fd < 0) {
        int err = errno;
        FL_LOGE("Hardware check failed: cannot open %s: errno %d (%s)", SYSFS_CHECK_PATH, err, strerror(err));
        pthread_mutex_unlock(&g_mutex);
        return FL_ERR_UNSUPPORTED;
    }
    char name_buf[64] = {0};
    ssize_t n_read = read(chk_fd, name_buf, sizeof(name_buf) - 1);
    close(chk_fd);
    if (n_read <= 0 || strstr(name_buf, "cam-flash") == NULL) {
        FL_LOGE("Hardware check failed: subdev15 name is '%.32s', expected 'cam-flash'", name_buf);
        pthread_mutex_unlock(&g_mutex);
        return FL_ERR_UNSUPPORTED;
    }

    /* Check state */
    if (g_is_on || g_is_shutting_down) {
        FL_LOGE("fl_on: torch is already active or shutting down");
        pthread_mutex_unlock(&g_mutex);
        return FL_ERR_STATE;
    }

    /* Check cooldown */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double now_sec = timespec_to_double(&now);
    double cooldown_until_sec = timespec_to_double(&g_cooldown_until);

    if (now_sec < cooldown_until_sec) {
        double remaining = cooldown_until_sec - now_sec;
        FL_LOGE("fl_on: safety cutoff cooldown active (%.1f s remaining)", remaining);
        pthread_mutex_unlock(&g_mutex);
        return FL_ERR_COOLDOWN;
    }



    /* Open video0 (CRM) */
    g_video_fd = open(VIDEO_PATH, O_RDWR);
    if (g_video_fd < 0) {
        int err = errno;
        FL_LOGE("open %s failed: errno %d (%s)", VIDEO_PATH, err, strerror(err));
        pthread_mutex_unlock(&g_mutex);
        return (err == EALREADY || err == EBUSY) ? FL_ERR_BUSY : FL_ERR_IO;
    }

    /* Open subdev15 (cam_flash) */
    g_subdev_fd = open(SUBDEV_PATH, O_RDWR);
    if (g_subdev_fd < 0) {
        int err = errno;
        FL_LOGE("open %s failed: errno %d (%s)", SUBDEV_PATH, err, strerror(err));
        close(g_video_fd);
        g_video_fd = -1;
        pthread_mutex_unlock(&g_mutex);
        return (err == EBUSY) ? FL_ERR_BUSY : FL_ERR_IO;
    }

    /* 1. CRM CREATE_SESSION */
    struct cam_req_mgr_session_info session_info;
    memset(&session_info, 0, sizeof(session_info));
    struct cam_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op_code = CAM_REQ_MGR_CREATE_SESSION;
    ctrl.size = sizeof(session_info);
    ctrl.handle_type = CAM_HANDLE_USER_POINTER;
    ctrl.handle = (uint64_t)(uintptr_t)&session_info;

    if (ioctl(g_video_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
        int err = errno;
        FL_LOGE("ioctl CAM_REQ_MGR_CREATE_SESSION failed: errno %d (%s)", err, strerror(err));
        goto fail_open;
    }
    g_session_hdl = session_info.session_hdl;

    /* 2. Allocate buffers */
    g_num_bufs_allocated = 0;
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (alloc_buffer_internal(g_video_fd, 1024, &g_bufs[i]) < 0) {
            FL_LOGE("Failed allocating buffer index %d", i);
            goto fail_session;
        }
        g_num_bufs_allocated++;
    }

    /* 3. CAM_ACQUIRE_DEV */
    struct cam_sensor_acquire_dev acq_cmd;
    memset(&acq_cmd, 0, sizeof(acq_cmd));
    acq_cmd.session_handle = (uint32_t)g_session_hdl;
    acq_cmd.device_handle = 0;
    acq_cmd.handle_type = 0;
    acq_cmd.reserved = 0;
    acq_cmd.info_handle = 0;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op_code = CAM_ACQUIRE_DEV;
    ctrl.size = sizeof(acq_cmd);
    ctrl.handle_type = CAM_HANDLE_USER_POINTER;
    ctrl.handle = (uint64_t)(uintptr_t)&acq_cmd;

    if (ioctl(g_subdev_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
        int err = errno;
        FL_LOGE("ioctl CAM_ACQUIRE_DEV failed: errno %d (%s)", err, strerror(err));
        /* Device is held by camera or stock flashlight */
        goto fail_acquire_busy;
    }
    g_dev_hdl = (int32_t)acq_cmd.device_handle;

    /* 4. Packet 1: INIT */
    /* 4a. CmdBuf 1: I2C_INFO (slave 0xC8, freq 1) */
    struct cam_cmd_i2c_info *info = (struct cam_cmd_i2c_info *)g_bufs[1].ptr;
    info->slave_addr = 0xC8;
    info->i2c_freq_mode = 1;
    info->cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_INFO;
    info->reserved = 0;

    /* 4b. CmdBuf 2: Exact verified 56-byte PWR_UP / PWR_DOWN buffer */
    uint8_t *pwr_buf = (uint8_t *)g_bufs[2].ptr;
    struct cam_cmd_power *pwr_up = (struct cam_cmd_power *)&pwr_buf[0];
    pwr_up->count = 1;
    pwr_up->reserved = 0;
    pwr_up->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    pwr_up->more_reserved = 0;
    pwr_up->power_settings[0].power_seq_type = SENSOR_CUSTOM_GPIO2; /* 11 -> tlmm 134 */
    pwr_up->power_settings[0].reserved = 0;
    pwr_up->power_settings[0].config_val_low = 1;
    pwr_up->power_settings[0].config_val_high = 0;

    struct cam_cmd_unconditional_wait *wait1 = (struct cam_cmd_unconditional_wait *)&pwr_buf[20];
    wait1->delay = 1;
    wait1->reserved = 0;
    wait1->op_code = CAMERA_SENSOR_WAIT_OP_SW_UCND;
    wait1->cmd_type = CAMERA_SENSOR_CMD_TYPE_WAIT;
    wait1->reserved1 = 0;

    struct cam_cmd_power *pwr_down = (struct cam_cmd_power *)&pwr_buf[28];
    pwr_down->count = 1;
    pwr_down->reserved = 0;
    pwr_down->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    pwr_down->more_reserved = 0;
    pwr_down->power_settings[0].power_seq_type = SENSOR_CUSTOM_GPIO2; /* 11 -> tlmm 134 */
    pwr_down->power_settings[0].reserved = 0;
    pwr_down->power_settings[0].config_val_low = 0;
    pwr_down->power_settings[0].config_val_high = 0;

    struct cam_cmd_unconditional_wait *wait2 = (struct cam_cmd_unconditional_wait *)&pwr_buf[48];
    wait2->delay = 1;
    wait2->reserved = 0;
    wait2->op_code = CAMERA_SENSOR_WAIT_OP_SW_UCND;
    wait2->cmd_type = CAMERA_SENSOR_CMD_TYPE_WAIT;
    wait2->reserved1 = 0;

    /* 4c. CmdBuf 3: Init I2C writes (0x01=0x00, 0x03=0x56, 0x04=level) */
    uint8_t *i2c_init_buf = (uint8_t *)g_bufs[3].ptr;
    struct i2c_rdwr_header *init_hdr = (struct i2c_rdwr_header *)i2c_init_buf;
    init_hdr->count = 3;
    init_hdr->op_code = CAMERA_SENSOR_I2C_OP_RNDM_WR;
    init_hdr->cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR;
    init_hdr->data_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
    init_hdr->addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
    struct i2c_random_wr_payload *init_payload = (struct i2c_random_wr_payload *)&i2c_init_buf[sizeof(struct i2c_rdwr_header)];
    init_payload[0].reg_addr = 0x01; init_payload[0].reg_data = 0x00;
    init_payload[1].reg_addr = 0x03; init_payload[1].reg_data = 0x56;
    init_payload[2].reg_addr = 0x04; init_payload[2].reg_data = (uint32_t)level;

    /* 4d. Packet 1 Header & Descriptors */
    struct cam_packet *pkt_init = (struct cam_packet *)g_bufs[0].ptr;
    pkt_init->header.op_code = CAM_FLASH_PACKET_OPCODE_INIT;
    pkt_init->header.size = sizeof(struct cam_packet) + 4 * sizeof(struct cam_cmd_buf_desc);
    pkt_init->header.request_id = 0;
    pkt_init->cmd_buf_offset = 0;
    pkt_init->num_cmd_buf = 4;

    struct cam_cmd_buf_desc *desc_init = (struct cam_cmd_buf_desc *)&pkt_init->payload[0];
    desc_init[0].mem_handle = g_bufs[0].handle;
    desc_init[0].offset = 0;
    desc_init[0].size = 0;
    desc_init[0].length = 0;
    desc_init[0].type = CAM_CMD_BUF_I2C;
    desc_init[0].meta_data = 0;

    desc_init[1].mem_handle = g_bufs[1].handle;
    desc_init[1].offset = 0;
    desc_init[1].size = sizeof(struct cam_cmd_i2c_info);
    desc_init[1].length = sizeof(struct cam_cmd_i2c_info);
    desc_init[1].type = CAM_CMD_BUF_I2C;
    desc_init[1].meta_data = 0;

    desc_init[2].mem_handle = g_bufs[2].handle;
    desc_init[2].offset = 0;
    desc_init[2].size = 56;
    desc_init[2].length = 56;
    desc_init[2].type = CAM_CMD_BUF_I2C;
    desc_init[2].meta_data = 0;

    desc_init[3].mem_handle = g_bufs[3].handle;
    desc_init[3].offset = 0;
    desc_init[3].size = sizeof(struct i2c_rdwr_header) + 3 * sizeof(struct i2c_random_wr_payload);
    desc_init[3].length = desc_init[3].size;
    desc_init[3].type = CAM_CMD_BUF_I2C;
    desc_init[3].meta_data = 0;

    struct cam_config_dev_cmd cfg_init;
    memset(&cfg_init, 0, sizeof(cfg_init));
    cfg_init.session_handle = g_session_hdl;
    cfg_init.dev_handle = g_dev_hdl;
    cfg_init.offset = 0;
    cfg_init.packet_handle = g_bufs[0].handle;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op_code = CAM_CONFIG_DEV;
    ctrl.size = sizeof(cfg_init);
    ctrl.handle_type = CAM_HANDLE_USER_POINTER;
    ctrl.handle = (uint64_t)(uintptr_t)&cfg_init;

    if (ioctl(g_subdev_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
        int err = errno;
        FL_LOGE("ioctl CAM_CONFIG_DEV (INIT) failed: errno %d (%s)", err, strerror(err));
        goto fail_acquire;
    }

    /* 5. Packet 2: STREAM_OFF (0x01=0x00) */
    uint8_t *off_buf = (uint8_t *)g_bufs[5].ptr;
    struct i2c_rdwr_header *off_hdr = (struct i2c_rdwr_header *)off_buf;
    off_hdr->count = 1;
    off_hdr->op_code = CAMERA_SENSOR_I2C_OP_RNDM_WR;
    off_hdr->cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR;
    off_hdr->data_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
    off_hdr->addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
    struct i2c_random_wr_payload *off_payload = (struct i2c_random_wr_payload *)&off_buf[sizeof(struct i2c_rdwr_header)];
    off_payload[0].reg_addr = 0x01; off_payload[0].reg_data = 0x00;

    struct cam_packet *pkt_streamoff = (struct cam_packet *)g_bufs[4].ptr;
    pkt_streamoff->header.op_code = CAM_FLASH_PACKET_OPCODE_STREAM_OFF;
    pkt_streamoff->header.size = sizeof(struct cam_packet) + sizeof(struct cam_cmd_buf_desc);
    pkt_streamoff->header.request_id = 0;
    pkt_streamoff->cmd_buf_offset = 0;
    pkt_streamoff->num_cmd_buf = 1;

    struct cam_cmd_buf_desc *desc_streamoff = (struct cam_cmd_buf_desc *)&pkt_streamoff->payload[0];
    desc_streamoff[0].mem_handle = g_bufs[5].handle;
    desc_streamoff[0].offset = 0;
    desc_streamoff[0].size = sizeof(struct i2c_rdwr_header) + sizeof(struct i2c_random_wr_payload);
    desc_streamoff[0].length = desc_streamoff[0].size;
    desc_streamoff[0].type = CAM_CMD_BUF_I2C;
    desc_streamoff[0].meta_data = 0;

    struct cam_config_dev_cmd cfg_streamoff;
    memset(&cfg_streamoff, 0, sizeof(cfg_streamoff));
    cfg_streamoff.session_handle = g_session_hdl;
    cfg_streamoff.dev_handle = g_dev_hdl;
    cfg_streamoff.offset = 0;
    cfg_streamoff.packet_handle = g_bufs[4].handle;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op_code = CAM_CONFIG_DEV;
    ctrl.size = sizeof(cfg_streamoff);
    ctrl.handle_type = CAM_HANDLE_USER_POINTER;
    ctrl.handle = (uint64_t)(uintptr_t)&cfg_streamoff;

    if (ioctl(g_subdev_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
        int err = errno;
        FL_LOGE("ioctl CAM_CONFIG_DEV (STREAM_OFF) failed: errno %d (%s)", err, strerror(err));
        goto fail_acquire;
    }

    /* 6. CAM_START_DEV */
    struct cam_start_stop_dev_cmd start_cmd;
    memset(&start_cmd, 0, sizeof(start_cmd));
    start_cmd.session_handle = g_session_hdl;
    start_cmd.dev_handle = g_dev_hdl;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op_code = CAM_START_DEV;
    ctrl.size = sizeof(start_cmd);
    ctrl.handle_type = CAM_HANDLE_USER_POINTER;
    ctrl.handle = (uint64_t)(uintptr_t)&start_cmd;

    if (ioctl(g_subdev_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
        int err = errno;
        FL_LOGE("ioctl CAM_START_DEV failed: errno %d (%s)", err, strerror(err));
        goto fail_acquire;
    }
    g_device_started = true;

    /* 7. Pre-fill Packet 4: Torch OFF (0x01=0x00) */
    uint8_t *torch_off_buf = (uint8_t *)g_bufs[9].ptr;
    struct i2c_rdwr_header *torch_off_hdr = (struct i2c_rdwr_header *)torch_off_buf;
    torch_off_hdr->count = 1;
    torch_off_hdr->op_code = CAMERA_SENSOR_I2C_OP_RNDM_WR;
    torch_off_hdr->cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR;
    torch_off_hdr->data_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
    torch_off_hdr->addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
    struct i2c_random_wr_payload *torch_off_payload = (struct i2c_random_wr_payload *)&torch_off_buf[sizeof(struct i2c_rdwr_header)];
    torch_off_payload[0].reg_addr = 0x01; torch_off_payload[0].reg_data = 0x00;

    struct cam_packet *pkt_torch_off = (struct cam_packet *)g_bufs[8].ptr;
    pkt_torch_off->header.op_code = CAM_FLASH_PACKET_OPCODE_NON_REALTIME_SET_OPS;
    pkt_torch_off->header.size = sizeof(struct cam_packet) + sizeof(struct cam_cmd_buf_desc);
    pkt_torch_off->header.request_id = 0;
    pkt_torch_off->cmd_buf_offset = 0;
    pkt_torch_off->num_cmd_buf = 1;

    struct cam_cmd_buf_desc *desc_torch_off = (struct cam_cmd_buf_desc *)&pkt_torch_off->payload[0];
    desc_torch_off[0].mem_handle = g_bufs[9].handle;
    desc_torch_off[0].offset = 0;
    desc_torch_off[0].size = sizeof(struct i2c_rdwr_header) + sizeof(struct i2c_random_wr_payload);
    desc_torch_off[0].length = desc_torch_off[0].size;
    desc_torch_off[0].type = CAM_CMD_BUF_I2C;
    desc_torch_off[0].meta_data = 0;

    memset(&g_cfg_torch_off, 0, sizeof(g_cfg_torch_off));
    g_cfg_torch_off.session_handle = g_session_hdl;
    g_cfg_torch_off.dev_handle = g_dev_hdl;
    g_cfg_torch_off.offset = 0;
    g_cfg_torch_off.packet_handle = g_bufs[8].handle;

    /* 8. Construct Packet 3: Torch ON (0x06=0x88, 0x04=level, 0x01=0x02) */
    uint8_t *on_buf = (uint8_t *)g_bufs[7].ptr;
    struct i2c_rdwr_header *on_hdr = (struct i2c_rdwr_header *)on_buf;
    on_hdr->count = 3;
    on_hdr->op_code = CAMERA_SENSOR_I2C_OP_RNDM_WR;
    on_hdr->cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR;
    on_hdr->data_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
    on_hdr->addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
    g_on_payload = (struct i2c_random_wr_payload *)&on_buf[sizeof(struct i2c_rdwr_header)];
    g_on_payload[0].reg_addr = 0x06; g_on_payload[0].reg_data = 0x88;
    g_on_payload[1].reg_addr = 0x04; g_on_payload[1].reg_data = (uint32_t)level;
    g_on_payload[2].reg_addr = 0x01; g_on_payload[2].reg_data = 0x02;

    struct cam_packet *pkt_on = (struct cam_packet *)g_bufs[6].ptr;
    pkt_on->header.op_code = CAM_FLASH_PACKET_OPCODE_NON_REALTIME_SET_OPS;
    pkt_on->header.size = sizeof(struct cam_packet) + sizeof(struct cam_cmd_buf_desc);
    pkt_on->header.request_id = 0;
    pkt_on->cmd_buf_offset = 0;
    pkt_on->num_cmd_buf = 1;

    struct cam_cmd_buf_desc *desc_on = (struct cam_cmd_buf_desc *)&pkt_on->payload[0];
    desc_on[0].mem_handle = g_bufs[7].handle;
    desc_on[0].offset = 0;
    desc_on[0].size = sizeof(struct i2c_rdwr_header) + 3 * sizeof(struct i2c_random_wr_payload);
    desc_on[0].length = desc_on[0].size;
    desc_on[0].type = CAM_CMD_BUF_I2C;
    desc_on[0].meta_data = 0;

    memset(&g_cfg_torch_on, 0, sizeof(g_cfg_torch_on));
    g_cfg_torch_on.session_handle = g_session_hdl;
    g_cfg_torch_on.dev_handle = g_dev_hdl;
    g_cfg_torch_on.offset = 0;
    g_cfg_torch_on.packet_handle = g_bufs[6].handle;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op_code = CAM_CONFIG_DEV;
    ctrl.size = sizeof(g_cfg_torch_on);
    ctrl.handle_type = CAM_HANDLE_USER_POINTER;
    ctrl.handle = (uint64_t)(uintptr_t)&g_cfg_torch_on;

    if (ioctl(g_subdev_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
        int err = errno;
        FL_LOGE("ioctl CAM_CONFIG_DEV (TORCH ON) failed: errno %d (%s)", err, strerror(err));
        goto fail_start;
    }

    /* Success: mark state, record timestamps, start watchdog */
    g_is_on = true;
    g_current_level = level;
    clock_gettime(CLOCK_MONOTONIC, &g_on_start_time);
    g_last_heartbeat = g_on_start_time;

    ensure_cond_initialized();
    g_wd_running = true;
    if (pthread_create(&g_wd_thread, NULL, watchdog_thread_fn, NULL) != 0) {
        FL_LOGE("pthread_create failed for watchdog thread");
        /* Safe teardown if watchdog thread cannot be spawned */
        fl_off_locked(false);
        pthread_mutex_unlock(&g_mutex);
        return FL_ERR_THREAD;
    }
    g_wd_thread_active = true;

    pthread_mutex_unlock(&g_mutex);
    return FL_OK;

fail_start:
    if (g_device_started) {
        struct cam_start_stop_dev_cmd stop_cmd;
        memset(&stop_cmd, 0, sizeof(stop_cmd));
        stop_cmd.session_handle = g_session_hdl;
        stop_cmd.dev_handle = g_dev_hdl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.op_code = CAM_STOP_DEV;
        ctrl.size = sizeof(stop_cmd);
        ctrl.handle_type = CAM_HANDLE_USER_POINTER;
        ctrl.handle = (uint64_t)(uintptr_t)&stop_cmd;
        ioctl(g_subdev_fd, VIDIOC_CAM_CONTROL, &ctrl);
        g_device_started = false;
    }
fail_acquire:
    if (g_dev_hdl != 0) {
        struct cam_release_dev_cmd rel_cmd;
        memset(&rel_cmd, 0, sizeof(rel_cmd));
        rel_cmd.session_handle = g_session_hdl;
        rel_cmd.dev_handle = g_dev_hdl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.op_code = CAM_RELEASE_DEV;
        ctrl.size = sizeof(rel_cmd);
        ctrl.handle_type = CAM_HANDLE_USER_POINTER;
        ctrl.handle = (uint64_t)(uintptr_t)&rel_cmd;
        ioctl(g_subdev_fd, VIDIOC_CAM_CONTROL, &ctrl);
        g_dev_hdl = 0;
    }
fail_session:
    for (int i = 0; i < g_num_bufs_allocated; i++) {
        free_buffer_internal(g_video_fd, &g_bufs[i]);
    }
    g_num_bufs_allocated = 0;
    if (g_session_hdl != 0) {
        struct cam_req_mgr_session_info s_info;
        memset(&s_info, 0, sizeof(s_info));
        s_info.session_hdl = g_session_hdl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.op_code = CAM_REQ_MGR_DESTROY_SESSION;
        ctrl.size = sizeof(s_info);
        ctrl.handle_type = CAM_HANDLE_USER_POINTER;
        ctrl.handle = (uint64_t)(uintptr_t)&s_info;
        ioctl(g_video_fd, VIDIOC_CAM_CONTROL, &ctrl);
        g_session_hdl = 0;
    }
fail_open:
    if (g_subdev_fd >= 0) {
        close(g_subdev_fd);
        g_subdev_fd = -1;
    }
    if (g_video_fd >= 0) {
        close(g_video_fd);
        g_video_fd = -1;
    }
    pthread_mutex_unlock(&g_mutex);
    return FL_ERR_IO;

fail_acquire_busy:
    /* Specific full cleanup path when CAM_ACQUIRE_DEV fails due to device busy */
    for (int i = 0; i < g_num_bufs_allocated; i++) {
        free_buffer_internal(g_video_fd, &g_bufs[i]);
    }
    g_num_bufs_allocated = 0;
    if (g_session_hdl != 0) {
        struct cam_req_mgr_session_info s_info;
        memset(&s_info, 0, sizeof(s_info));
        s_info.session_hdl = g_session_hdl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.op_code = CAM_REQ_MGR_DESTROY_SESSION;
        ctrl.size = sizeof(s_info);
        ctrl.handle_type = CAM_HANDLE_USER_POINTER;
        ctrl.handle = (uint64_t)(uintptr_t)&s_info;
        ioctl(g_video_fd, VIDIOC_CAM_CONTROL, &ctrl);
        g_session_hdl = 0;
    }
    if (g_subdev_fd >= 0) {
        close(g_subdev_fd);
        g_subdev_fd = -1;
    }
    if (g_video_fd >= 0) {
        close(g_video_fd);
        g_video_fd = -1;
    }
    pthread_mutex_unlock(&g_mutex);
    return FL_ERR_BUSY;
}

int fl_set_level(int level) {
    if (level < 1 || level > FL_MAX_LEVEL) {
        FL_LOGE("fl_set_level: level %d out of range (1..%d)", level, FL_MAX_LEVEL);
        return FL_ERR_RANGE;
    }

    pthread_mutex_lock(&g_mutex);

    if (!g_is_on || g_is_shutting_down || !g_on_payload) {
        FL_LOGE("fl_set_level: torch is not on");
        pthread_mutex_unlock(&g_mutex);
        return FL_ERR_STATE;
    }

    /* Update register 0x04 value */
    g_on_payload[1].reg_data = (uint32_t)level;

    struct cam_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op_code = CAM_CONFIG_DEV;
    ctrl.size = sizeof(g_cfg_torch_on);
    ctrl.handle_type = CAM_HANDLE_USER_POINTER;
    ctrl.handle = (uint64_t)(uintptr_t)&g_cfg_torch_on;

    if (ioctl(g_subdev_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
        int err = errno;
        FL_LOGE("ioctl CAM_CONFIG_DEV (set level %d) failed: errno %d (%s)", level, err, strerror(err));
        pthread_mutex_unlock(&g_mutex);
        return FL_ERR_IO;
    }

    g_current_level = level;

    /* Reset watchdog inactivity timer */
    clock_gettime(CLOCK_MONOTONIC, &g_last_heartbeat);

    pthread_mutex_unlock(&g_mutex);
    return FL_OK;
}

int fl_heartbeat(void) {
    pthread_mutex_lock(&g_mutex);

    if (!g_is_on || g_is_shutting_down) {
        pthread_mutex_unlock(&g_mutex);
        return FL_ERR_STATE;
    }

    clock_gettime(CLOCK_MONOTONIC, &g_last_heartbeat);

    pthread_mutex_unlock(&g_mutex);
    return FL_OK;
}

/* Internal fl_off helper: assumes g_mutex is locked upon entry */
static int fl_off_locked(bool triggered_by_watchdog) {
    if (!g_is_on) {
        return FL_OK;
    }

    /* Record interval for cumulative heat tracking */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    record_heat_interval(timespec_to_double(&g_on_start_time), timespec_to_double(&now));

    /* Mark shutting down and is_on = false BEFORE unlocking to join thread */
    g_is_shutting_down = true;
    g_is_on = false;
    g_current_level = 0;

    if (!triggered_by_watchdog && g_wd_thread_active) {
        pthread_t tid = g_wd_thread;
        g_wd_thread_active = false;
        g_wd_running = false;
        pthread_cond_broadcast(&g_wd_cond);

        /* Unlock temporarily to join watchdog thread cleanly without deadlock */
        pthread_mutex_unlock(&g_mutex);
        pthread_join(tid, NULL);
        pthread_mutex_lock(&g_mutex);
    } else if (triggered_by_watchdog) {
        /* Watchdog is executing this on its own thread: detach itself so no leak */
        pthread_detach(pthread_self());
        g_wd_thread_active = false;
        g_wd_running = false;
    }

    struct cam_control ctrl;

    /* 1. Send explicit Torch OFF packet (0x01=0x00) */
    if (g_subdev_fd >= 0 && g_cfg_torch_off.packet_handle != 0) {
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.op_code = CAM_CONFIG_DEV;
        ctrl.size = sizeof(g_cfg_torch_off);
        ctrl.handle_type = CAM_HANDLE_USER_POINTER;
        ctrl.handle = (uint64_t)(uintptr_t)&g_cfg_torch_off;
        if (ioctl(g_subdev_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
            int err = errno;
            FL_LOGE("ioctl CAM_CONFIG_DEV (TORCH OFF) failed: errno %d (%s)", err, strerror(err));
        }
    }

    /* 2. CAM_STOP_DEV */
    if (g_subdev_fd >= 0 && g_dev_hdl != 0 && g_device_started) {
        struct cam_start_stop_dev_cmd stop_cmd;
        memset(&stop_cmd, 0, sizeof(stop_cmd));
        stop_cmd.session_handle = g_session_hdl;
        stop_cmd.dev_handle = g_dev_hdl;

        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.op_code = CAM_STOP_DEV;
        ctrl.size = sizeof(stop_cmd);
        ctrl.handle_type = CAM_HANDLE_USER_POINTER;
        ctrl.handle = (uint64_t)(uintptr_t)&stop_cmd;
        if (ioctl(g_subdev_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
            int err = errno;
            FL_LOGE("ioctl CAM_STOP_DEV failed: errno %d (%s)", err, strerror(err));
        }
        g_device_started = false;
    }

    /* 3. CAM_RELEASE_DEV */
    if (g_subdev_fd >= 0 && g_dev_hdl != 0) {
        struct cam_release_dev_cmd rel_cmd;
        memset(&rel_cmd, 0, sizeof(rel_cmd));
        rel_cmd.session_handle = g_session_hdl;
        rel_cmd.dev_handle = g_dev_hdl;

        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.op_code = CAM_RELEASE_DEV;
        ctrl.size = sizeof(rel_cmd);
        ctrl.handle_type = CAM_HANDLE_USER_POINTER;
        ctrl.handle = (uint64_t)(uintptr_t)&rel_cmd;
        if (ioctl(g_subdev_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
            int err = errno;
            FL_LOGE("ioctl CAM_RELEASE_DEV failed: errno %d (%s)", err, strerror(err));
        }
        g_dev_hdl = 0;
    }

    /* 4. Release all buffers */
    for (int i = 0; i < g_num_bufs_allocated; i++) {
        free_buffer_internal(g_video_fd, &g_bufs[i]);
    }
    g_num_bufs_allocated = 0;

    /* 5. Destroy CRM Session */
    if (g_video_fd >= 0 && g_session_hdl != 0) {
        struct cam_req_mgr_session_info s_info;
        memset(&s_info, 0, sizeof(s_info));
        s_info.session_hdl = g_session_hdl;

        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.op_code = CAM_REQ_MGR_DESTROY_SESSION;
        ctrl.size = sizeof(s_info);
        ctrl.handle_type = CAM_HANDLE_USER_POINTER;
        ctrl.handle = (uint64_t)(uintptr_t)&s_info;
        if (ioctl(g_video_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) {
            int err = errno;
            FL_LOGE("ioctl CAM_REQ_MGR_DESTROY_SESSION failed: errno %d (%s)", err, strerror(err));
        }
        g_session_hdl = 0;
    }

    /* 6. Close file descriptors */
    if (g_subdev_fd >= 0) {
        close(g_subdev_fd);
        g_subdev_fd = -1;
    }
    if (g_video_fd >= 0) {
        close(g_video_fd);
        g_video_fd = -1;
    }

    g_on_payload = NULL;
    g_is_shutting_down = false;
    return FL_OK;
}

int fl_off(void) {
    pthread_mutex_lock(&g_mutex);
    int rc = fl_off_locked(false);
    pthread_mutex_unlock(&g_mutex);
    return rc;
}

static void *watchdog_thread_fn(void *arg) {
    (void)arg;

    pthread_mutex_lock(&g_mutex);
    while (g_wd_running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        double inactivity_sec = timespec_diff_sec(&g_last_heartbeat, &now);

        if (inactivity_sec >= (double)FL_WATCHDOG_TIMEOUT_SEC) {
            FL_LOGI("Watchdog: inactivity cutoff after %.1f s (no heartbeat)", inactivity_sec);
            g_cooldown_until.tv_sec = now.tv_sec + FL_COOLDOWN_SEC;
            g_cooldown_until.tv_nsec = now.tv_nsec;
            fl_off_locked(true);
            break;
        }

        /* Monotonic deadline: at most 1 second in the future */
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += 1;

        pthread_cond_timedwait(&g_wd_cond, &g_mutex, &deadline);
    }
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

int fl_is_on(void) {
    pthread_mutex_lock(&g_mutex);
    int state = (g_is_on && !g_is_shutting_down) ? 1 : 0;
    pthread_mutex_unlock(&g_mutex);
    return state;
}

struct fl_status_info fl_status(void) {
    struct fl_status_info info;
    memset(&info, 0, sizeof(info));

    pthread_mutex_lock(&g_mutex);

    info.is_on = (g_is_on && !g_is_shutting_down) ? 1 : 0;
    info.current_level = info.is_on ? g_current_level : 0;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (info.is_on) {
        double elapsed = timespec_diff_sec(&g_on_start_time, &now);
        info.seconds_on = (elapsed > 0.0) ? (int)elapsed : 0;
    } else {
        info.seconds_on = 0;
    }

    double now_sec = timespec_to_double(&now);
    double cooldown_until_sec = timespec_to_double(&g_cooldown_until);
    int cd_rem = 0;
    if (now_sec < cooldown_until_sec) {
        cd_rem = (int)(cooldown_until_sec - now_sec + 0.999);
    }
    info.cooldown_remaining = cd_rem;

    pthread_mutex_unlock(&g_mutex);
    return info;
}

int fl_max_level(void) {
    return FL_MAX_LEVEL;
}

const char *fl_strerror(int err) {
    switch (err) {
        case FL_OK:               return "Success";
        case FL_ERR_BUSY:         return "Camera or stock flashlight in use (device busy / CAM_ACQUIRE_DEV failed)";
        case FL_ERR_RANGE:        return "Brightness level out of range (1..44)";
        case FL_ERR_STATE:        return "Invalid operation for current torch state";
        case FL_ERR_IO:           return "Hardware ioctl or system call failed";
        case FL_ERR_COOLDOWN:     return "Thermal or cumulative heat cooldown period active";
        case FL_ERR_NOMEM:        return "Buffer or memory allocation failed";
        case FL_ERR_THREAD:       return "Watchdog thread failure";
        case FL_ERR_UNSUPPORTED:  return "Hardware unsupported (subdev15 is not cam-flash)";
        default:                  return "Unknown error";
    }
}
