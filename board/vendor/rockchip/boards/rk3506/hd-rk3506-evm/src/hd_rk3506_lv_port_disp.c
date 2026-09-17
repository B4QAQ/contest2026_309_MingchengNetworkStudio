/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_lv_port_disp.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL v9 display port for the HD-RK3506-EVM board.
 *
 * - Opens /dev/fb0 (registered by rk3506_vop_register in the bringup) and
 *   mmaps the framebuffer into a contiguous buffer that LVGL renders into.
 * - The flush callback copies LVGL's rendered chunk into the framebuffer.
 *   Without a connected LCD, FBIO_UPDATE is a no-op, but the pixel data
 *   is still in the framebuffer memory and would be scanned out if a panel
 *   were attached.
 * - A dedicated NuttX task pumps lv_timer_handler() in a sleep loop so LVGL
 *   animations/timers run. A 1 ms tick increment is provided by
 *   CONFIG_USEC_PER_TICK (1 ms) inside the system tick ISR -- LVGL v9 can
 *   read the tick via lv_tick_get() without an explicit tick thread if
 *   LV_TICK_CUSTOM is set; we use the default lv_tick_inc() from a task
 *   here to keep the port self-contained.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nuttx/board.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/video/fb.h>
#include <nuttx/clock.h>

#include <syslog.h>

#include <lvgl/lvgl.h>

#if defined(CONFIG_GRAPHICS_LVGL)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef LV_PORT_DISP_DEV
#  define LV_PORT_DISP_DEV          "/dev/fb0"
#endif

#define LV_PORT_TICK_THREAD_STACK   (4096)
#define LV_PORT_TICK_THREAD_PRIO     100
#define LV_PORT_TIMER_THREAD_STACK   (8192)
#define LV_PORT_TIMER_THREAD_PRIO    100
#define LV_PORT_TIMER_PERIOD_MS      33   /* ~30 Hz redraw */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int               g_fb_fd      = -1;
static void             *g_fb_base    = NULL;
static struct fb_planeinfo_s g_plane;
static struct fb_videoinfo_s g_video;
static lv_display_t     *g_disp       = NULL;

/* One full-screen render buffer (RGB565 == 2 bytes/pixel). 480*854*2 = 820 KB.
 * We allocate it from the heap so LVGL can render into it; on flush we copy
 * to the framebuffer.  A partial-mode buffer (a few rows) would be lighter
 * but requires LVGL to render in stripes; for now the simplest correct
 * behaviour is full-frame.
 */

static lv_color_t       *g_render_buf = NULL;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void rk3506_disp_flush(lv_display_t *disp,
                              const lv_area_t *area,
                              uint8_t *px_map)
{
  if (g_fb_base == NULL || px_map == NULL || area == NULL)
    {
      lv_display_flush_ready(disp);
      return;
    }

  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;
  if (w <= 0 || h <= 0)
    {
      lv_display_flush_ready(disp);
      return;
    }

  /* px_map is laid out as a contiguous w*h block of lv_color16 (RGB565).
   * Copy row-by-row into the framebuffer at (area->x1, area->y1) using
   * the plane's stride.
   */

  const uint8_t *src = px_map;
  uint8_t       *dst = (uint8_t *)g_fb_base
                     + (size_t)area->y1 * g_plane.stride
                     + (size_t)area->x1 * (g_video.fmt == FB_FMT_RGB16_565
                                            ? 2 : 1);
  size_t row_bytes = (size_t)w * (g_video.fmt == FB_FMT_RGB16_565 ? 2 : 1);

  for (int32_t y = 0; y < h; y++)
    {
      memcpy(dst, src, row_bytes);
      src += row_bytes;
      dst += g_plane.stride;
    }

#ifdef CONFIG_FB_UPDATE
  struct fb_area_s upd;
  upd.x = area->x1;
  upd.y = area->y1;
  upd.w = w;
  upd.h = h;
  (void)ioctl(g_fb_fd, FBIO_UPDATE, &upd);
#endif

  lv_display_flush_ready(disp);
}

/****************************************************************************
 * Name: lv_port_timer_thread
 *
 * Description:
 *   Sleeps LV_PORT_TIMER_PERIOD_MS then pumps lv_timer_handler(). This is
 *   the LVGL "main loop" task. Runs at a low priority.
 *
 ****************************************************************************/

static int lv_port_timer_thread(int argc, char *argv[])
{
  while (1)
    {
      lv_timer_handler();
      usleep(LV_PORT_TIMER_PERIOD_MS * 1000);
    }

  return 0;
}

/****************************************************************************
 * Name: lv_port_tick_thread
 *
 * Description:
 *   Increments LVGL's internal millisecond tick every 5 ms. LVGL uses this
 *   for animation timing and the lv_tick_get() clock.
 *
 ****************************************************************************/

static int lv_port_tick_thread(int argc, char *argv[])
{
  while (1)
    {
      lv_tick_inc(5);
      usleep(5 * 1000);
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int hd_rk3506_lv_port_disp_init(void)
{
  int ret;

  syslog(LOG_INFO, "LVGL: hd_rk3506_lv_port_disp_init() enter\n");

  /* Initialise the LVGL core. */

  lv_init();
  syslog(LOG_INFO, "LVGL: lv_init() done\n");

  /* Open the framebuffer registered by rk3506_vop_register(). */

  g_fb_fd = open(LV_PORT_DISP_DEV, O_RDWR);
  if (g_fb_fd < 0)
    {
      int err = errno;
      struct stat st;
      int stat_ret = stat(LV_PORT_DISP_DEV, &st);
      syslog(LOG_ERR, "LVGL: open(%s) failed: errno=%d, stat=%d\n",
             LV_PORT_DISP_DEV, err, stat_ret);
      if (stat_ret == 0)
        {
          syslog(LOG_ERR, "LVGL: stat shows %s exists, mode=0%o, size=%lld\n",
                 LV_PORT_DISP_DEV, (unsigned)(st.st_mode & 0777),
                 (long long)st.st_size);
        }

      /* Probe what is actually under /dev so we can see whether the VOP
       * fb driver registered the device or not.  Read the /dev directory.
       */

      DIR *dir = opendir("/dev");
      if (dir != NULL)
        {
          struct dirent *e;
          int n = 0;
          while ((e = readdir(dir)) != NULL && n < 32)
            {
              syslog(LOG_INFO, "LVGL: /dev/%s\n", e->d_name);
              n++;
            }
          closedir(dir);
        }
      else
        {
          syslog(LOG_ERR, "LVGL: opendir(/dev) failed errno=%d\n", errno);
        }

      set_errno(err);
      return -err;
    }
  syslog(LOG_INFO, "LVGL: open(%s) ok fd=%d\n", LV_PORT_DISP_DEV, g_fb_fd);

  ret = ioctl(g_fb_fd, FBIOGET_VIDEOINFO, (unsigned long)((uintptr_t)&g_video));
  if (ret < 0)
    {
      int err = errno;
      syslog(LOG_ERR, "LVGL: FBIOGET_VIDEOINFO failed: errno=%d\n", err);
      close(g_fb_fd);
      g_fb_fd = -1;
      set_errno(err);
      return -err;
    }
  syslog(LOG_INFO, "LVGL: videoinfo fmt=%d xres=%d yres=%d\n",
         (int)g_video.fmt, (int)g_video.xres, (int)g_video.yres);

  ret = ioctl(g_fb_fd, FBIOGET_PLANEINFO, (unsigned long)((uintptr_t)&g_plane));
  if (ret < 0)
    {
      int err = errno;
      syslog(LOG_ERR, "LVGL: FBIOGET_PLANEINFO failed: errno=%d\n", err);
      close(g_fb_fd);
      g_fb_fd = -1;
      set_errno(err);
      return -err;
    }
  syslog(LOG_INFO, "LVGL: planeinfo stride=%u fblen=%u fbmem=%p\n",
         (unsigned)g_plane.stride, (unsigned)g_plane.fblen, g_plane.fbmem);

  if (g_plane.fbmem == NULL || g_plane.fblen == 0)
    {
      syslog(LOG_ERR, "LVGL: plane has no fbmem (fbmem=%p fblen=%u)\n",
             g_plane.fbmem, (unsigned)g_plane.fblen);
      close(g_fb_fd);
      g_fb_fd = -1;
      return -ENODEV;
    }

  g_fb_base = mmap(NULL, g_plane.fblen, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_FILE, g_fb_fd, 0);
  if (g_fb_base == MAP_FAILED)
    {
      int err = errno;
      syslog(LOG_ERR, "LVGL: mmap failed: errno=%d\n", err);
      close(g_fb_fd);
      g_fb_fd = -1;
      g_fb_base = NULL;
      set_errno(err);
      return -err;
    }
  syslog(LOG_INFO, "LVGL: mmap ok base=%p\n", g_fb_base);

  /* Allocate a render buffer large enough for the whole screen. RGB565
   * is 2 bytes per pixel.  480*854*2 = 819 840 bytes; round up to be
   * safe.
   */

  size_t buf_pixels = (size_t)g_video.xres * (size_t)g_video.yres;
  if (buf_pixels == 0)
    {
      syslog(LOG_ERR, "LVGL: videoinfo has zero resolution\n");
      munmap(g_fb_base, g_plane.fblen);
      close(g_fb_fd);
      g_fb_base = NULL;
      g_fb_fd   = -1;
      return -EINVAL;
    }

  g_render_buf = (lv_color_t *)malloc(buf_pixels * sizeof(lv_color_t));
  if (g_render_buf == NULL)
    {
      syslog(LOG_ERR, "LVGL: malloc(%zu) for render buf failed\n",
             buf_pixels * sizeof(lv_color_t));
      munmap(g_fb_base, g_plane.fblen);
      close(g_fb_fd);
      g_fb_base = NULL;
      g_fb_fd   = -1;
      return -ENOMEM;
    }
  syslog(LOG_INFO, "LVGL: render buf %zu bytes at %p\n",
         buf_pixels * sizeof(lv_color_t), g_render_buf);

  /* Create the LVGL display and bind the flush callback. */

  g_disp = lv_display_create(g_video.xres, g_video.yres);
  if (g_disp == NULL)
    {
      syslog(LOG_ERR, "LVGL: lv_display_create returned NULL\n");
      free(g_render_buf);
      munmap(g_fb_base, g_plane.fblen);
      close(g_fb_fd);
      g_render_buf = NULL;
      g_fb_base    = NULL;
      g_fb_fd      = -1;
      return -ENOMEM;
    }
  syslog(LOG_INFO, "LVGL: lv_display_create ok disp=%p\n", g_disp);

  lv_display_set_flush_cb(g_disp, rk3506_disp_flush);
  lv_display_set_buffers(g_disp, g_render_buf, NULL,
                         buf_pixels * sizeof(lv_color_t),
                         LV_DISPLAY_RENDER_MODE_FULL);
  syslog(LOG_INFO, "LVGL: flush_cb + buffers set, render mode FULL\n");

  /* Start the LVGL timer-handler and tick threads. */

  pid_t pid;
  pid = task_create("lvgl_tick", LV_PORT_TICK_THREAD_PRIO,
                    LV_PORT_TICK_THREAD_STACK, lv_port_tick_thread, NULL);
  if (pid < 0)
    {
      syslog(LOG_INFO, "LVGL: lvgl_tick task_create failed: %d\n", pid);
    }
  else
    {
      syslog(LOG_INFO, "LVGL: lvgl_tick task pid=%d\n", pid);
    }

  pid = task_create("lvgl_timer", LV_PORT_TIMER_THREAD_PRIO,
                    LV_PORT_TIMER_THREAD_STACK, lv_port_timer_thread, NULL);
  if (pid < 0)
    {
      syslog(LOG_INFO, "LVGL: lvgl_timer task_create failed: %d\n", pid);
    }
  else
    {
      syslog(LOG_INFO, "LVGL: lvgl_timer task pid=%d\n", pid);
    }

  syslog(LOG_INFO, "LVGL: init done OK\n");
  return OK;
}

#endif /* CONFIG_GRAPHICS_LVGL */
