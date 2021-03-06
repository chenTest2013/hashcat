/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#include "common.h"
#include "types.h"
#include "timer.h"
#include "memory.h"
#include "logging.h"
#include "locking.h"
#include "ext_ADL.h"
#include "ext_nvapi.h"
#include "ext_nvml.h"
#include "ext_xnvctrl.h"
#include "ext_OpenCL.h"
#include "cpu_md5.h"
#include "interface.h"
#include "tuningdb.h"
#include "thread.h"
#include "opencl.h"
#include "hwmon.h"
#include "restore.h"
#include "hash_management.h"
#include "status.h"
#include "stdout.h"
#include "mpsp.h"
#include "rp_cpu.h"
#include "outfile.h"
#include "potfile.h"
#include "debugfile.h"
#include "loopback.h"
#include "filenames.h"
#include "data.h"
#include "shared.h"
#include "filehandling.h"
#include "convert.h"
#include "dictstat.h"
#include "wordlist.h"

extern hc_global_data_t data;

extern hc_thread_mutex_t mux_hwmon;

extern const int comptime;

char *strstatus (const uint devices_status)
{
  switch (devices_status)
  {
    case  STATUS_INIT:      return ((char *) ST_0000);
    case  STATUS_AUTOTUNE:  return ((char *) ST_0001);
    case  STATUS_RUNNING:   return ((char *) ST_0002);
    case  STATUS_PAUSED:    return ((char *) ST_0003);
    case  STATUS_EXHAUSTED: return ((char *) ST_0004);
    case  STATUS_CRACKED:   return ((char *) ST_0005);
    case  STATUS_ABORTED:   return ((char *) ST_0006);
    case  STATUS_QUIT:      return ((char *) ST_0007);
    case  STATUS_BYPASS:    return ((char *) ST_0008);
  }

  return ((char *) "Uninitialized! Bug!");
}

static uint setup_opencl_platforms_filter (const char *opencl_platforms)
{
  uint opencl_platforms_filter = 0;

  if (opencl_platforms)
  {
    char *platforms = mystrdup (opencl_platforms);

    char *next = strtok (platforms, ",");

    do
    {
      int platform = atoi (next);

      if (platform < 1 || platform > 32)
      {
        log_error ("ERROR: Invalid OpenCL platform %u specified", platform);

        exit (-1);
      }

      opencl_platforms_filter |= 1u << (platform - 1);

    } while ((next = strtok (NULL, ",")) != NULL);

    myfree (platforms);
  }
  else
  {
    opencl_platforms_filter = -1u;
  }

  return opencl_platforms_filter;
}

static u32 setup_devices_filter (const char *opencl_devices)
{
  u32 devices_filter = 0;

  if (opencl_devices)
  {
    char *devices = mystrdup (opencl_devices);

    char *next = strtok (devices, ",");

    do
    {
      int device_id = atoi (next);

      if (device_id < 1 || device_id > 32)
      {
        log_error ("ERROR: Invalid device_id %u specified", device_id);

        exit (-1);
      }

      devices_filter |= 1u << (device_id - 1);

    } while ((next = strtok (NULL, ",")) != NULL);

    myfree (devices);
  }
  else
  {
    devices_filter = -1u;
  }

  return devices_filter;
}

static cl_device_type setup_device_types_filter (const char *opencl_device_types)
{
  cl_device_type device_types_filter = 0;

  if (opencl_device_types)
  {
    char *device_types = mystrdup (opencl_device_types);

    char *next = strtok (device_types, ",");

    do
    {
      int device_type = atoi (next);

      if (device_type < 1 || device_type > 3)
      {
        log_error ("ERROR: Invalid device_type %u specified", device_type);

        exit (-1);
      }

      device_types_filter |= 1u << device_type;

    } while ((next = strtok (NULL, ",")) != NULL);

    myfree (device_types);
  }
  else
  {
    // Do not use CPU by default, this often reduces GPU performance because
    // the CPU is too busy to handle GPU synchronization

    device_types_filter = CL_DEVICE_TYPE_ALL & ~CL_DEVICE_TYPE_CPU;
  }

  return device_types_filter;
}

void load_kernel (const char *kernel_file, int num_devices, size_t *kernel_lengths, const u8 **kernel_sources)
{
  FILE *fp = fopen (kernel_file, "rb");

  if (fp != NULL)
  {
    struct stat st;

    memset (&st, 0, sizeof (st));

    stat (kernel_file, &st);

    u8 *buf = (u8 *) mymalloc (st.st_size + 1);

    size_t num_read = fread (buf, sizeof (u8), st.st_size, fp);

    if (num_read != (size_t) st.st_size)
    {
      log_error ("ERROR: %s: %s", kernel_file, strerror (errno));

      exit (-1);
    }

    fclose (fp);

    buf[st.st_size] = 0;

    for (int i = 0; i < num_devices; i++)
    {
      kernel_lengths[i] = (size_t) st.st_size;

      kernel_sources[i] = buf;
    }
  }
  else
  {
    log_error ("ERROR: %s: %s", kernel_file, strerror (errno));

    exit (-1);
  }

  return;
}

void writeProgramBin (char *dst, u8 *binary, size_t binary_size)
{
  if (binary_size > 0)
  {
    FILE *fp = fopen (dst, "wb");

    lock_file (fp);
    fwrite (binary, sizeof (u8), binary_size, fp);

    fflush (fp);
    fclose (fp);
  }
}

int gidd_to_pw_t (opencl_ctx_t *opencl_ctx, hc_device_param_t *device_param, const u64 gidd, pw_t *pw)
{
  cl_int CL_err = hc_clEnqueueReadBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_pws_buf, CL_TRUE, gidd * sizeof (pw_t), sizeof (pw_t), pw, 0, NULL, NULL);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clEnqueueReadBuffer(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  return 0;
}

int choose_kernel (opencl_ctx_t *opencl_ctx, hc_device_param_t *device_param, hashconfig_t *hashconfig, const uint attack_exec, const uint attack_mode, const uint opts_type, const salt_t *salt_buf, const uint highest_pw_len, const uint pws_cnt, const uint fast_iteration)
{
  cl_int CL_err = CL_SUCCESS;

  if (hashconfig->hash_mode == 2000)
  {
    process_stdout (opencl_ctx, device_param, pws_cnt);

    return 0;
  }

  if (attack_exec == ATTACK_EXEC_INSIDE_KERNEL)
  {
    if (attack_mode == ATTACK_MODE_BF)
    {
      if (opts_type & OPTS_TYPE_PT_BITSLICE)
      {
        const uint size_tm = 32 * sizeof (bs_word_t);

        run_kernel_bzero (opencl_ctx, device_param, device_param->d_tm_c, size_tm);

        run_kernel_tm (opencl_ctx, device_param);

        CL_err = hc_clEnqueueCopyBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_tm_c, device_param->d_bfs_c, 0, 0, size_tm, 0, NULL, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clEnqueueCopyBuffer(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }
    }

    if (highest_pw_len < 16)
    {
      run_kernel (KERN_RUN_1, opencl_ctx, device_param, pws_cnt, true, fast_iteration, hashconfig);
    }
    else if (highest_pw_len < 32)
    {
      run_kernel (KERN_RUN_2, opencl_ctx, device_param, pws_cnt, true, fast_iteration, hashconfig);
    }
    else
    {
      run_kernel (KERN_RUN_3, opencl_ctx, device_param, pws_cnt, true, fast_iteration, hashconfig);
    }
  }
  else
  {
    run_kernel_amp (opencl_ctx, device_param, pws_cnt);

    run_kernel (KERN_RUN_1, opencl_ctx, device_param, pws_cnt, false, 0, hashconfig);

    if (opts_type & OPTS_TYPE_HOOK12)
    {
      run_kernel (KERN_RUN_12, opencl_ctx, device_param, pws_cnt, false, 0, hashconfig);

      CL_err = hc_clEnqueueReadBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_hooks, CL_TRUE, 0, device_param->size_hooks, device_param->hooks_buf, 0, NULL, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clEnqueueReadBuffer(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      // do something with data

      CL_err = hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_hooks, CL_TRUE, 0, device_param->size_hooks, device_param->hooks_buf, 0, NULL, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clEnqueueWriteBuffer(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }

    uint iter = salt_buf->salt_iter;

    uint loop_step = device_param->kernel_loops;

    for (uint loop_pos = 0, slow_iteration = 0; loop_pos < iter; loop_pos += loop_step, slow_iteration++)
    {
      uint loop_left = iter - loop_pos;

      loop_left = MIN (loop_left, loop_step);

      device_param->kernel_params_buf32[28] = loop_pos;
      device_param->kernel_params_buf32[29] = loop_left;

      run_kernel (KERN_RUN_2, opencl_ctx, device_param, pws_cnt, true, slow_iteration, hashconfig);

      while (opencl_ctx->run_thread_level2 == false) break;

      /**
       * speed
       */

      const float iter_part = (float) (loop_pos + loop_left) / iter;

      const u64 perf_sum_all = (u64) (pws_cnt * iter_part);

      double speed_ms;

      hc_timer_get (device_param->timer_speed, speed_ms);

      const u32 speed_pos = device_param->speed_pos;

      device_param->speed_cnt[speed_pos] = perf_sum_all;

      device_param->speed_ms[speed_pos] = speed_ms;

      if (data.benchmark == 1)
      {
        if (speed_ms > 4096) myabort (opencl_ctx);
      }
    }

    if (opts_type & OPTS_TYPE_HOOK23)
    {
      run_kernel (KERN_RUN_23, opencl_ctx, device_param, pws_cnt, false, 0, hashconfig);

      CL_err = hc_clEnqueueReadBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_hooks, CL_TRUE, 0, device_param->size_hooks, device_param->hooks_buf, 0, NULL, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clEnqueueReadBuffer(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      // do something with data

      CL_err = hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_hooks, CL_TRUE, 0, device_param->size_hooks, device_param->hooks_buf, 0, NULL, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clEnqueueWriteBuffer(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }

    run_kernel (KERN_RUN_3, opencl_ctx, device_param, pws_cnt, false, 0, hashconfig);
  }

  return 0;
}

int run_kernel (const uint kern_run, opencl_ctx_t *opencl_ctx, hc_device_param_t *device_param, const uint num, const uint event_update, const uint iteration, hashconfig_t *hashconfig)
{
  cl_int CL_err = CL_SUCCESS;

  uint num_elements = num;

  device_param->kernel_params_buf32[33] = data.combs_mode;
  device_param->kernel_params_buf32[34] = num;

  uint kernel_threads = device_param->kernel_threads;

  while (num_elements % kernel_threads) num_elements++;

  cl_kernel kernel = NULL;

  switch (kern_run)
  {
    case KERN_RUN_1:    kernel = device_param->kernel1;     break;
    case KERN_RUN_12:   kernel = device_param->kernel12;    break;
    case KERN_RUN_2:    kernel = device_param->kernel2;     break;
    case KERN_RUN_23:   kernel = device_param->kernel23;    break;
    case KERN_RUN_3:    kernel = device_param->kernel3;     break;
  }

  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 24, sizeof (cl_uint), device_param->kernel_params[24]);
  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 25, sizeof (cl_uint), device_param->kernel_params[25]);
  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 26, sizeof (cl_uint), device_param->kernel_params[26]);
  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 27, sizeof (cl_uint), device_param->kernel_params[27]);
  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 28, sizeof (cl_uint), device_param->kernel_params[28]);
  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 29, sizeof (cl_uint), device_param->kernel_params[29]);
  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 30, sizeof (cl_uint), device_param->kernel_params[30]);
  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 31, sizeof (cl_uint), device_param->kernel_params[31]);
  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 32, sizeof (cl_uint), device_param->kernel_params[32]);
  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 33, sizeof (cl_uint), device_param->kernel_params[33]);
  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 34, sizeof (cl_uint), device_param->kernel_params[34]);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clSetKernelArg(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  cl_event event;

  if ((hashconfig->opts_type & OPTS_TYPE_PT_BITSLICE) && (data.attack_mode == ATTACK_MODE_BF))
  {
    const size_t global_work_size[3] = { num_elements,        32, 1 };
    const size_t local_work_size[3]  = { kernel_threads / 32, 32, 1 };

    CL_err = hc_clEnqueueNDRangeKernel (opencl_ctx->ocl, device_param->command_queue, kernel, 2, NULL, global_work_size, local_work_size, 0, NULL, &event);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clEnqueueNDRangeKernel(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }
  }
  else
  {
    if (kern_run == KERN_RUN_2)
    {
      if (hashconfig->opti_type & OPTI_TYPE_SLOW_HASH_SIMD)
      {
        num_elements = CEIL (num_elements / device_param->vector_width);
      }
    }

    while (num_elements % kernel_threads) num_elements++;

    const size_t global_work_size[3] = { num_elements,   1, 1 };
    const size_t local_work_size[3]  = { kernel_threads, 1, 1 };

    CL_err = hc_clEnqueueNDRangeKernel (opencl_ctx->ocl, device_param->command_queue, kernel, 1, NULL, global_work_size, local_work_size, 0, NULL, &event);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clEnqueueNDRangeKernel(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }
  }

  CL_err = hc_clFlush (opencl_ctx->ocl, device_param->command_queue);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clFlush(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  if (device_param->nvidia_spin_damp > 0)
  {
    if (opencl_ctx->devices_status == STATUS_RUNNING)
    {
      if (iteration < EXPECTED_ITERATIONS)
      {
        switch (kern_run)
        {
          case KERN_RUN_1: if (device_param->exec_us_prev1[iteration] > 0) usleep ((useconds_t)(device_param->exec_us_prev1[iteration] * device_param->nvidia_spin_damp)); break;
          case KERN_RUN_2: if (device_param->exec_us_prev2[iteration] > 0) usleep ((useconds_t)(device_param->exec_us_prev2[iteration] * device_param->nvidia_spin_damp)); break;
          case KERN_RUN_3: if (device_param->exec_us_prev3[iteration] > 0) usleep ((useconds_t)(device_param->exec_us_prev3[iteration] * device_param->nvidia_spin_damp)); break;
        }
      }
    }
  }

  CL_err = hc_clWaitForEvents (opencl_ctx->ocl, 1, &event);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clWaitForEvents(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  cl_ulong time_start;
  cl_ulong time_end;

  CL_err |= hc_clGetEventProfilingInfo (opencl_ctx->ocl, event, CL_PROFILING_COMMAND_START, sizeof (time_start), &time_start, NULL);
  CL_err |= hc_clGetEventProfilingInfo (opencl_ctx->ocl, event, CL_PROFILING_COMMAND_END,   sizeof (time_end),   &time_end,   NULL);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clGetEventProfilingInfo(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  const double exec_us = (double) (time_end - time_start) / 1000;

  if (opencl_ctx->devices_status == STATUS_RUNNING)
  {
    if (iteration < EXPECTED_ITERATIONS)
    {
      switch (kern_run)
      {
        case KERN_RUN_1: device_param->exec_us_prev1[iteration] = exec_us; break;
        case KERN_RUN_2: device_param->exec_us_prev2[iteration] = exec_us; break;
        case KERN_RUN_3: device_param->exec_us_prev3[iteration] = exec_us; break;
      }
    }
  }

  if (event_update)
  {
    uint exec_pos = device_param->exec_pos;

    device_param->exec_ms[exec_pos] = exec_us / 1000;

    exec_pos++;

    if (exec_pos == EXEC_CACHE)
    {
      exec_pos = 0;
    }

    device_param->exec_pos = exec_pos;
  }

  CL_err = hc_clReleaseEvent (opencl_ctx->ocl, event);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clReleaseEvent(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  CL_err = hc_clFinish (opencl_ctx->ocl, device_param->command_queue);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clFinish(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  return 0;
}

int run_kernel_mp (const uint kern_run, opencl_ctx_t *opencl_ctx, hc_device_param_t *device_param, const uint num)
{
  cl_int CL_err = CL_SUCCESS;

  uint num_elements = num;

  switch (kern_run)
  {
    case KERN_RUN_MP:   device_param->kernel_params_mp_buf32[8]   = num; break;
    case KERN_RUN_MP_R: device_param->kernel_params_mp_r_buf32[8] = num; break;
    case KERN_RUN_MP_L: device_param->kernel_params_mp_l_buf32[9] = num; break;
  }

  // causes problems with special threads like in bcrypt
  // const uint kernel_threads = device_param->kernel_threads;

  uint kernel_threads = device_param->kernel_threads;

  while (num_elements % kernel_threads) num_elements++;

  cl_kernel kernel = NULL;

  switch (kern_run)
  {
    case KERN_RUN_MP:   kernel = device_param->kernel_mp;   break;
    case KERN_RUN_MP_R: kernel = device_param->kernel_mp_r; break;
    case KERN_RUN_MP_L: kernel = device_param->kernel_mp_l; break;
  }

  switch (kern_run)
  {
    case KERN_RUN_MP:   CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 3, sizeof (cl_ulong), device_param->kernel_params_mp[3]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 4, sizeof (cl_uint),  device_param->kernel_params_mp[4]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 5, sizeof (cl_uint),  device_param->kernel_params_mp[5]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 6, sizeof (cl_uint),  device_param->kernel_params_mp[6]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 7, sizeof (cl_uint),  device_param->kernel_params_mp[7]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 8, sizeof (cl_uint),  device_param->kernel_params_mp[8]);
                        break;
    case KERN_RUN_MP_R: CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 3, sizeof (cl_ulong), device_param->kernel_params_mp_r[3]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 4, sizeof (cl_uint),  device_param->kernel_params_mp_r[4]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 5, sizeof (cl_uint),  device_param->kernel_params_mp_r[5]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 6, sizeof (cl_uint),  device_param->kernel_params_mp_r[6]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 7, sizeof (cl_uint),  device_param->kernel_params_mp_r[7]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 8, sizeof (cl_uint),  device_param->kernel_params_mp_r[8]);
                        break;
    case KERN_RUN_MP_L: CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 3, sizeof (cl_ulong), device_param->kernel_params_mp_l[3]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 4, sizeof (cl_uint),  device_param->kernel_params_mp_l[4]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 5, sizeof (cl_uint),  device_param->kernel_params_mp_l[5]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 6, sizeof (cl_uint),  device_param->kernel_params_mp_l[6]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 7, sizeof (cl_uint),  device_param->kernel_params_mp_l[7]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 8, sizeof (cl_uint),  device_param->kernel_params_mp_l[8]);
                        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 9, sizeof (cl_uint),  device_param->kernel_params_mp_l[9]);
                        break;
  }

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clSetKernelArg(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  const size_t global_work_size[3] = { num_elements,   1, 1 };
  const size_t local_work_size[3]  = { kernel_threads, 1, 1 };

  CL_err = hc_clEnqueueNDRangeKernel (opencl_ctx->ocl, device_param->command_queue, kernel, 1, NULL, global_work_size, local_work_size, 0, NULL, NULL);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clEnqueueNDRangeKernel(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  CL_err = hc_clFlush (opencl_ctx->ocl, device_param->command_queue);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clFlush(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  CL_err = hc_clFinish (opencl_ctx->ocl, device_param->command_queue);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clFinish(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  return 0;
}

int run_kernel_tm (opencl_ctx_t *opencl_ctx, hc_device_param_t *device_param)
{
  cl_int CL_err = CL_SUCCESS;

  const uint num_elements = 1024; // fixed

  uint kernel_threads = 32;

  cl_kernel kernel = device_param->kernel_tm;

  const size_t global_work_size[3] = { num_elements,    1, 1 };
  const size_t local_work_size[3]  = { kernel_threads,  1, 1 };

  CL_err = hc_clEnqueueNDRangeKernel (opencl_ctx->ocl, device_param->command_queue, kernel, 1, NULL, global_work_size, local_work_size, 0, NULL, NULL);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clEnqueueNDRangeKernel(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  CL_err = hc_clFlush (opencl_ctx->ocl, device_param->command_queue);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clFlush(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  CL_err = hc_clFinish (opencl_ctx->ocl, device_param->command_queue);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clFinish(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  return 0;
}

int run_kernel_amp (opencl_ctx_t *opencl_ctx, hc_device_param_t *device_param, const uint num)
{
  cl_int CL_err = CL_SUCCESS;

  uint num_elements = num;

  device_param->kernel_params_amp_buf32[5] = data.combs_mode;
  device_param->kernel_params_amp_buf32[6] = num_elements;

  // causes problems with special threads like in bcrypt
  // const uint kernel_threads = device_param->kernel_threads;

  uint kernel_threads = device_param->kernel_threads;

  while (num_elements % kernel_threads) num_elements++;

  cl_kernel kernel = device_param->kernel_amp;

  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 5, sizeof (cl_uint), device_param->kernel_params_amp[5]);
  CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 6, sizeof (cl_uint), device_param->kernel_params_amp[6]);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clSetKernelArg(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  const size_t global_work_size[3] = { num_elements,    1, 1 };
  const size_t local_work_size[3]  = { kernel_threads,  1, 1 };

  CL_err = hc_clEnqueueNDRangeKernel (opencl_ctx->ocl, device_param->command_queue, kernel, 1, NULL, global_work_size, local_work_size, 0, NULL, NULL);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clEnqueueNDRangeKernel(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  CL_err = hc_clFlush (opencl_ctx->ocl, device_param->command_queue);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clFlush(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  CL_err = hc_clFinish (opencl_ctx->ocl, device_param->command_queue);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clFinish(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  return 0;
}

int run_kernel_memset (opencl_ctx_t *opencl_ctx, hc_device_param_t *device_param, cl_mem buf, const uint value, const uint num)
{
  cl_int CL_err = CL_SUCCESS;

  const u32 num16d = num / 16;
  const u32 num16m = num % 16;

  if (num16d)
  {
    device_param->kernel_params_memset_buf32[1] = value;
    device_param->kernel_params_memset_buf32[2] = num16d;

    uint kernel_threads = device_param->kernel_threads;

    uint num_elements = num16d;

    while (num_elements % kernel_threads) num_elements++;

    cl_kernel kernel = device_param->kernel_memset;

    CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 0, sizeof (cl_mem),  (void *) &buf);
    CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 1, sizeof (cl_uint), device_param->kernel_params_memset[1]);
    CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, kernel, 2, sizeof (cl_uint), device_param->kernel_params_memset[2]);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clSetKernelArg(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    const size_t global_work_size[3] = { num_elements,   1, 1 };
    const size_t local_work_size[3]  = { kernel_threads, 1, 1 };

    CL_err = hc_clEnqueueNDRangeKernel (opencl_ctx->ocl, device_param->command_queue, kernel, 1, NULL, global_work_size, local_work_size, 0, NULL, NULL);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clEnqueueNDRangeKernel(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    CL_err = hc_clFlush (opencl_ctx->ocl, device_param->command_queue);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clFlush(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    CL_err = hc_clFinish (opencl_ctx->ocl, device_param->command_queue);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clFinish(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }
  }

  if (num16m)
  {
    u32 tmp[4];

    tmp[0] = value;
    tmp[1] = value;
    tmp[2] = value;
    tmp[3] = value;

    CL_err = hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, buf, CL_TRUE, num16d * 16, num16m, tmp, 0, NULL, NULL);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clEnqueueWriteBuffer(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }
  }

  return 0;
}

int run_kernel_bzero (opencl_ctx_t *opencl_ctx, hc_device_param_t *device_param, cl_mem buf, const size_t size)
{
  return run_kernel_memset (opencl_ctx, device_param, buf, 0, size);
}

int run_copy (opencl_ctx_t *opencl_ctx, hc_device_param_t *device_param, hashconfig_t *hashconfig, const uint pws_cnt)
{
  cl_int CL_err = CL_SUCCESS;

  if (data.attack_kern == ATTACK_KERN_STRAIGHT)
  {
    CL_err = hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_pws_buf, CL_TRUE, 0, pws_cnt * sizeof (pw_t), device_param->pws_buf, 0, NULL, NULL);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clEnqueueWriteBuffer(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }
  }
  else if (data.attack_kern == ATTACK_KERN_COMBI)
  {
    if (data.attack_mode == ATTACK_MODE_COMBI)
    {
      if (data.combs_mode == COMBINATOR_MODE_BASE_RIGHT)
      {
        if (hashconfig->opts_type & OPTS_TYPE_PT_ADD01)
        {
          for (u32 i = 0; i < pws_cnt; i++)
          {
            const u32 pw_len = device_param->pws_buf[i].pw_len;

            u8 *ptr = (u8 *) device_param->pws_buf[i].i;

            ptr[pw_len] = 0x01;
          }
        }
        else if (hashconfig->opts_type & OPTS_TYPE_PT_ADD80)
        {
          for (u32 i = 0; i < pws_cnt; i++)
          {
            const u32 pw_len = device_param->pws_buf[i].pw_len;

            u8 *ptr = (u8 *) device_param->pws_buf[i].i;

            ptr[pw_len] = 0x80;
          }
        }
      }
    }
    else if (data.attack_mode == ATTACK_MODE_HYBRID2)
    {
      if (hashconfig->opts_type & OPTS_TYPE_PT_ADD01)
      {
        for (u32 i = 0; i < pws_cnt; i++)
        {
          const u32 pw_len = device_param->pws_buf[i].pw_len;

          u8 *ptr = (u8 *) device_param->pws_buf[i].i;

          ptr[pw_len] = 0x01;
        }
      }
      else if (hashconfig->opts_type & OPTS_TYPE_PT_ADD80)
      {
        for (u32 i = 0; i < pws_cnt; i++)
        {
          const u32 pw_len = device_param->pws_buf[i].pw_len;

          u8 *ptr = (u8 *) device_param->pws_buf[i].i;

          ptr[pw_len] = 0x80;
        }
      }
    }

    CL_err = hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_pws_buf, CL_TRUE, 0, pws_cnt * sizeof (pw_t), device_param->pws_buf, 0, NULL, NULL);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clEnqueueWriteBuffer(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }
  }
  else if (data.attack_kern == ATTACK_KERN_BF)
  {
    const u64 off = device_param->words_off;

    device_param->kernel_params_mp_l_buf64[3] = off;

    run_kernel_mp (KERN_RUN_MP_L, opencl_ctx, device_param, pws_cnt);
  }

  return 0;
}

int run_cracker (opencl_ctx_t *opencl_ctx, hc_device_param_t *device_param, hashconfig_t *hashconfig, hashes_t *hashes, const uint pws_cnt)
{
  char *line_buf = (char *) mymalloc (HCBUFSIZ_LARGE);

  // init speed timer

  uint speed_pos = device_param->speed_pos;

  #if defined (_POSIX)
  if (device_param->timer_speed.tv_sec == 0)
  {
    hc_timer_set (&device_param->timer_speed);
  }
  #endif

  #if defined (_WIN)
  if (device_param->timer_speed.QuadPart == 0)
  {
    hc_timer_set (&device_param->timer_speed);
  }
  #endif

  // find higest password length, this is for optimization stuff

  uint highest_pw_len = 0;

  if (data.attack_kern == ATTACK_KERN_STRAIGHT)
  {
  }
  else if (data.attack_kern == ATTACK_KERN_COMBI)
  {
  }
  else if (data.attack_kern == ATTACK_KERN_BF)
  {
    highest_pw_len = device_param->kernel_params_mp_l_buf32[4]
                   + device_param->kernel_params_mp_l_buf32[5];
  }

  // loop start: most outer loop = salt iteration, then innerloops (if multi)

  for (uint salt_pos = 0; salt_pos < hashes->salts_cnt; salt_pos++)
  {
    while (opencl_ctx->devices_status == STATUS_PAUSED) hc_sleep (1);

    salt_t *salt_buf = &hashes->salts_buf[salt_pos];

    device_param->kernel_params_buf32[27] = salt_pos;
    device_param->kernel_params_buf32[31] = salt_buf->digests_cnt;
    device_param->kernel_params_buf32[32] = salt_buf->digests_offset;

    FILE *combs_fp = device_param->combs_fp;

    if (data.attack_mode == ATTACK_MODE_COMBI)
    {
      rewind (combs_fp);
    }

    // iteration type

    uint innerloop_step = 0;
    uint innerloop_cnt  = 0;

    if   (hashconfig->attack_exec == ATTACK_EXEC_INSIDE_KERNEL) innerloop_step = device_param->kernel_loops;
    else                                                        innerloop_step = 1;

    if      (data.attack_kern == ATTACK_KERN_STRAIGHT)  innerloop_cnt  = data.kernel_rules_cnt;
    else if (data.attack_kern == ATTACK_KERN_COMBI)     innerloop_cnt  = data.combs_cnt;
    else if (data.attack_kern == ATTACK_KERN_BF)        innerloop_cnt  = data.bfs_cnt;

    // innerloops

    for (uint innerloop_pos = 0; innerloop_pos < innerloop_cnt; innerloop_pos += innerloop_step)
    {
      while (opencl_ctx->devices_status == STATUS_PAUSED) hc_sleep (1);

      uint fast_iteration = 0;

      uint innerloop_left = innerloop_cnt - innerloop_pos;

      if (innerloop_left > innerloop_step)
      {
        innerloop_left = innerloop_step;

        fast_iteration = 1;
      }

      device_param->innerloop_pos  = innerloop_pos;
      device_param->innerloop_left = innerloop_left;

      device_param->kernel_params_buf32[30] = innerloop_left;

      // i think we can get rid of this
      if (innerloop_left == 0)
      {
        puts ("bug, how should this happen????\n");

        continue;
      }

      if (hashes->salts_shown[salt_pos] == 1)
      {
        data.words_progress_done[salt_pos] += (u64) pws_cnt * (u64) innerloop_left;

        continue;
      }

      // initialize amplifiers

      if (data.attack_mode == ATTACK_MODE_COMBI)
      {
        uint i = 0;

        while (i < innerloop_left)
        {
          if (feof (combs_fp)) break;

          int line_len = fgetl (combs_fp, line_buf);

          if (line_len >= PW_MAX1) continue;

          line_len = convert_from_hex (line_buf, line_len);

          char *line_buf_new = line_buf;

          if (run_rule_engine (data.rule_len_r, data.rule_buf_r))
          {
            char rule_buf_out[BLOCK_SIZE] = { 0 };

            int rule_len_out = _old_apply_rule (data.rule_buf_r, data.rule_len_r, line_buf, line_len, rule_buf_out);

            if (rule_len_out < 0)
            {
              data.words_progress_rejected[salt_pos] += pws_cnt;

              continue;
            }

            line_len = rule_len_out;

            line_buf_new = rule_buf_out;
          }

          line_len = MIN (line_len, PW_DICTMAX);

          u8 *ptr = (u8 *) device_param->combs_buf[i].i;

          memcpy (ptr, line_buf_new, line_len);

          memset (ptr + line_len, 0, PW_DICTMAX1 - line_len);

          if (hashconfig->opts_type & OPTS_TYPE_PT_UPPER)
          {
            uppercase (ptr, line_len);
          }

          if (data.combs_mode == COMBINATOR_MODE_BASE_LEFT)
          {
            if (hashconfig->opts_type & OPTS_TYPE_PT_ADD80)
            {
              ptr[line_len] = 0x80;
            }

            if (hashconfig->opts_type & OPTS_TYPE_PT_ADD01)
            {
              ptr[line_len] = 0x01;
            }
          }

          device_param->combs_buf[i].pw_len = line_len;

          i++;
        }

        for (uint j = i; j < innerloop_left; j++)
        {
          device_param->combs_buf[j].i[0] = 0;
          device_param->combs_buf[j].i[1] = 0;
          device_param->combs_buf[j].i[2] = 0;
          device_param->combs_buf[j].i[3] = 0;
          device_param->combs_buf[j].i[4] = 0;
          device_param->combs_buf[j].i[5] = 0;
          device_param->combs_buf[j].i[6] = 0;
          device_param->combs_buf[j].i[7] = 0;

          device_param->combs_buf[j].pw_len = 0;
        }

        innerloop_left = i;
      }
      else if (data.attack_mode == ATTACK_MODE_BF)
      {
        u64 off = innerloop_pos;

        device_param->kernel_params_mp_r_buf64[3] = off;

        run_kernel_mp (KERN_RUN_MP_R, opencl_ctx, device_param, innerloop_left);
      }
      else if (data.attack_mode == ATTACK_MODE_HYBRID1)
      {
        u64 off = innerloop_pos;

        device_param->kernel_params_mp_buf64[3] = off;

        run_kernel_mp (KERN_RUN_MP, opencl_ctx, device_param, innerloop_left);
      }
      else if (data.attack_mode == ATTACK_MODE_HYBRID2)
      {
        u64 off = innerloop_pos;

        device_param->kernel_params_mp_buf64[3] = off;

        run_kernel_mp (KERN_RUN_MP, opencl_ctx, device_param, innerloop_left);
      }

      // copy amplifiers

      if (data.attack_mode == ATTACK_MODE_STRAIGHT)
      {
        cl_int CL_err = hc_clEnqueueCopyBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_rules, device_param->d_rules_c, innerloop_pos * sizeof (kernel_rule_t), 0, innerloop_left * sizeof (kernel_rule_t), 0, NULL, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clEnqueueCopyBuffer(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }
      else if (data.attack_mode == ATTACK_MODE_COMBI)
      {
        cl_int CL_err = hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_combs_c, CL_TRUE, 0, innerloop_left * sizeof (comb_t), device_param->combs_buf, 0, NULL, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clEnqueueWriteBuffer(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }
      else if (data.attack_mode == ATTACK_MODE_BF)
      {
        cl_int CL_err = hc_clEnqueueCopyBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_bfs, device_param->d_bfs_c, 0, 0, innerloop_left * sizeof (bf_t), 0, NULL, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clEnqueueCopyBuffer(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }
      else if (data.attack_mode == ATTACK_MODE_HYBRID1)
      {
        cl_int CL_err = hc_clEnqueueCopyBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_combs, device_param->d_combs_c, 0, 0, innerloop_left * sizeof (comb_t), 0, NULL, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clEnqueueCopyBuffer(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }
      else if (data.attack_mode == ATTACK_MODE_HYBRID2)
      {
        cl_int CL_err = hc_clEnqueueCopyBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_combs, device_param->d_combs_c, 0, 0, innerloop_left * sizeof (comb_t), 0, NULL, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clEnqueueCopyBuffer(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }

      if (data.benchmark == 1)
      {
        hc_timer_set (&device_param->timer_speed);
      }

      int rc = choose_kernel (opencl_ctx, device_param, hashconfig, hashconfig->attack_exec, data.attack_mode, hashconfig->opts_type, salt_buf, highest_pw_len, pws_cnt, fast_iteration);

      if (rc == -1) return -1;

      /**
       * result
       */

      if (data.benchmark == 0)
      {
        check_cracked (opencl_ctx, device_param, hashconfig, hashes, salt_pos);
      }

      /**
       * progress
       */

      u64 perf_sum_all = (u64) pws_cnt * (u64) innerloop_left;

      hc_thread_mutex_lock (opencl_ctx->mux_counter);

      data.words_progress_done[salt_pos] += perf_sum_all;

      hc_thread_mutex_unlock (opencl_ctx->mux_counter);

      /**
       * speed
       */

      double speed_ms;

      hc_timer_get (device_param->timer_speed, speed_ms);

      hc_timer_set (&device_param->timer_speed);

      // current speed

      //hc_thread_mutex_lock (mux_display);

      device_param->speed_cnt[speed_pos] = perf_sum_all;

      device_param->speed_ms[speed_pos] = speed_ms;

      //hc_thread_mutex_unlock (mux_display);

      speed_pos++;

      if (speed_pos == SPEED_CACHE)
      {
        speed_pos = 0;
      }

      /**
       * benchmark
       */

      if (data.benchmark == 1) break;

      if (opencl_ctx->run_thread_level2 == false) break;
    }

    if (opencl_ctx->run_thread_level2 == false) break;
  }

  device_param->speed_pos = speed_pos;

  myfree (line_buf);

  return 0;
}

int opencl_ctx_init (opencl_ctx_t *opencl_ctx, const char *opencl_platforms, const char *opencl_devices, const char *opencl_device_types, const uint opencl_vector_width, const uint opencl_vector_width_chgd, const uint nvidia_spin_damp, const uint nvidia_spin_damp_chgd, const uint workload_profile, const uint kernel_accel, const uint kernel_accel_chgd, const uint kernel_loops, const uint kernel_loops_chgd, const uint keyspace, const uint stdout_flag)
{
  if (keyspace == 1)
  {
    opencl_ctx->disable = 1;

    return 0;
  }

  hc_thread_mutex_init (opencl_ctx->mux_dispatcher);
  hc_thread_mutex_init (opencl_ctx->mux_counter);

  opencl_ctx->devices_status            = STATUS_INIT;
  opencl_ctx->run_main_level1           = true;
  opencl_ctx->run_main_level2           = true;
  opencl_ctx->run_main_level3           = true;
  opencl_ctx->run_thread_level1         = true;
  opencl_ctx->run_thread_level2         = true;
  opencl_ctx->opencl_vector_width_chgd  = opencl_vector_width_chgd;
  opencl_ctx->opencl_vector_width       = opencl_vector_width;
  opencl_ctx->nvidia_spin_damp_chgd     = nvidia_spin_damp_chgd;
  opencl_ctx->nvidia_spin_damp          = nvidia_spin_damp;
  opencl_ctx->kernel_accel_chgd         = kernel_accel_chgd;
  opencl_ctx->kernel_accel              = kernel_accel;
  opencl_ctx->kernel_loops_chgd         = kernel_loops_chgd;
  opencl_ctx->kernel_loops              = kernel_loops;
  opencl_ctx->workload_profile          = workload_profile;

  opencl_ctx->ocl = (OCL_PTR *) mymalloc (sizeof (OCL_PTR));

  hc_device_param_t *devices_param = (hc_device_param_t *) mycalloc (DEVICES_MAX, sizeof (hc_device_param_t));

  opencl_ctx->devices_param = devices_param;

  /**
   * Load and map OpenCL library calls
   * TODO: remove exit() calls in there
   */

  ocl_init (opencl_ctx->ocl);

  /**
   * OpenCL platform selection
   */

  u32 opencl_platforms_filter = setup_opencl_platforms_filter (opencl_platforms);

  opencl_ctx->opencl_platforms_filter = opencl_platforms_filter;

  /**
   * OpenCL device selection
   */

  u32 devices_filter = setup_devices_filter (opencl_devices);

  opencl_ctx->devices_filter = devices_filter;

  /**
   * OpenCL device type selection
   */

  cl_device_type device_types_filter = setup_device_types_filter (opencl_device_types);

  opencl_ctx->device_types_filter = device_types_filter;

  /**
   * OpenCL platforms: detect
   */

  cl_uint         platforms_cnt         = 0;
  cl_platform_id *platforms             = (cl_platform_id *) mycalloc (CL_PLATFORMS_MAX, sizeof (cl_platform_id));
  cl_uint         platform_devices_cnt  = 0;
  cl_device_id   *platform_devices      = (cl_device_id *) mycalloc (DEVICES_MAX, sizeof (cl_device_id));

  cl_int CL_err = hc_clGetPlatformIDs (opencl_ctx->ocl, CL_PLATFORMS_MAX, platforms, &platforms_cnt);

  if (CL_err != CL_SUCCESS)
  {
    log_error ("ERROR: clGetPlatformIDs(): %s\n", val2cstr_cl (CL_err));

    return -1;
  }

  if (platforms_cnt == 0)
  {
    log_info ("");
    log_info ("ATTENTION! No OpenCL compatible platform found");
    log_info ("");
    log_info ("You're probably missing the OpenCL runtime installation");
    log_info ("  AMD users require AMD drivers 14.9 or later (recommended 15.12 or later)");
    log_info ("  Intel users require Intel OpenCL Runtime 14.2 or later (recommended 15.1 or later)");
    log_info ("  NVidia users require NVidia drivers 346.59 or later (recommended 361.x or later)");
    log_info ("");

    return -1;
  }

  if (opencl_platforms_filter != (uint) -1)
  {
    uint platform_cnt_mask = ~(((uint) -1 >> platforms_cnt) << platforms_cnt);

    if (opencl_platforms_filter > platform_cnt_mask)
    {
      log_error ("ERROR: The platform selected by the --opencl-platforms parameter is larger than the number of available platforms (%d)", platforms_cnt);

      return -1;
    }
  }

  if (opencl_device_types == NULL)
  {
    /**
     * OpenCL device types:
     *   In case the user did not specify --opencl-device-types and the user runs hashcat in a system with only a CPU only he probably want to use that CPU.
     */

    cl_device_type device_types_all = 0;

    for (uint platform_id = 0; platform_id < platforms_cnt; platform_id++)
    {
      if ((opencl_platforms_filter & (1u << platform_id)) == 0) continue;

      cl_platform_id platform = platforms[platform_id];

      cl_int CL_err = hc_clGetDeviceIDs (opencl_ctx->ocl, platform, CL_DEVICE_TYPE_ALL, DEVICES_MAX, platform_devices, &platform_devices_cnt);

      if (CL_err != CL_SUCCESS)
      {
        //log_error ("ERROR: clGetDeviceIDs(): %s\n", val2cstr_cl (CL_err));

        //return -1;

        // Silently ignore at this point, it will be reused later and create a note for the user at that point

        continue;
      }

      for (uint platform_devices_id = 0; platform_devices_id < platform_devices_cnt; platform_devices_id++)
      {
        cl_device_id device = platform_devices[platform_devices_id];

        cl_device_type device_type;

        cl_int CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device, CL_DEVICE_TYPE, sizeof (device_type), &device_type, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        device_types_all |= device_type;
      }
    }

    // In such a case, automatically enable CPU device type support, since it's disabled by default.

    if ((device_types_all & (CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_ACCELERATOR)) == 0)
    {
      device_types_filter |= CL_DEVICE_TYPE_CPU;
    }

    // In another case, when the user uses --stdout, using CPU devices is much faster to setup
    // If we have a CPU device, force it to be used

    if (stdout_flag == 1)
    {
      if (device_types_all & CL_DEVICE_TYPE_CPU)
      {
        device_types_filter = CL_DEVICE_TYPE_CPU;
      }
    }
  }

  opencl_ctx->platforms_cnt         = platforms_cnt;
  opencl_ctx->platforms             = platforms;
  opencl_ctx->platform_devices_cnt  = platform_devices_cnt;
  opencl_ctx->platform_devices      = platform_devices;

  return 0;
}

void opencl_ctx_destroy (opencl_ctx_t *opencl_ctx)
{
  if (opencl_ctx->disable == 1) return;

  myfree (opencl_ctx->devices_param);

  ocl_close (opencl_ctx->ocl);

  myfree (opencl_ctx->ocl);

  myfree (opencl_ctx->platforms);

  myfree (opencl_ctx->platform_devices);

  hc_thread_mutex_delete (opencl_ctx->mux_counter);
  hc_thread_mutex_delete (opencl_ctx->mux_dispatcher);

  myfree (opencl_ctx);
}

int opencl_ctx_devices_init (opencl_ctx_t *opencl_ctx, const hashconfig_t *hashconfig, const tuning_db_t *tuning_db, const uint attack_mode, const bool quiet, const bool force, const bool benchmark, const bool opencl_info, const bool machine_readable, const uint algorithm_pos)
{
  if (opencl_ctx->disable == 1) return 0;

  /**
   * OpenCL devices: simply push all devices from all platforms into the same device array
   */

  cl_uint         platforms_cnt         = opencl_ctx->platforms_cnt;
  cl_platform_id *platforms             = opencl_ctx->platforms;
  cl_uint         platform_devices_cnt  = opencl_ctx->platform_devices_cnt;
  cl_device_id   *platform_devices      = opencl_ctx->platform_devices;

  int need_adl     = 0;
  int need_nvml    = 0;
  int need_nvapi   = 0;
  int need_xnvctrl = 0;

  u32 devices_cnt = 0;

  u32 devices_active = 0;

  if (opencl_info)
  {
    fprintf (stdout, "OpenCL Info:\n");
  }

  for (uint platform_id = 0; platform_id < platforms_cnt; platform_id++)
  {
    cl_int CL_err = CL_SUCCESS;

    cl_platform_id platform = platforms[platform_id];

    char platform_vendor[HCBUFSIZ_TINY] = { 0 };

    CL_err = hc_clGetPlatformInfo (opencl_ctx->ocl, platform, CL_PLATFORM_VENDOR, sizeof (platform_vendor), platform_vendor, NULL);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clGetPlatformInfo(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    // find our own platform vendor because pocl and mesa are pushing original vendor_id through opencl
    // this causes trouble with vendor id based macros
    // we'll assign generic to those without special optimization available

    cl_uint platform_vendor_id = 0;

    if (strcmp (platform_vendor, CL_VENDOR_AMD) == 0)
    {
      platform_vendor_id = VENDOR_ID_AMD;
    }
    else if (strcmp (platform_vendor, CL_VENDOR_AMD_USE_INTEL) == 0)
    {
      platform_vendor_id = VENDOR_ID_AMD_USE_INTEL;
    }
    else if (strcmp (platform_vendor, CL_VENDOR_APPLE) == 0)
    {
      platform_vendor_id = VENDOR_ID_APPLE;
    }
    else if (strcmp (platform_vendor, CL_VENDOR_INTEL_BEIGNET) == 0)
    {
      platform_vendor_id = VENDOR_ID_INTEL_BEIGNET;
    }
    else if (strcmp (platform_vendor, CL_VENDOR_INTEL_SDK) == 0)
    {
      platform_vendor_id = VENDOR_ID_INTEL_SDK;
    }
    else if (strcmp (platform_vendor, CL_VENDOR_MESA) == 0)
    {
      platform_vendor_id = VENDOR_ID_MESA;
    }
    else if (strcmp (platform_vendor, CL_VENDOR_NV) == 0)
    {
      platform_vendor_id = VENDOR_ID_NV;
    }
    else if (strcmp (platform_vendor, CL_VENDOR_POCL) == 0)
    {
      platform_vendor_id = VENDOR_ID_POCL;
    }
    else
    {
      platform_vendor_id = VENDOR_ID_GENERIC;
    }

    uint platform_skipped = ((opencl_ctx->opencl_platforms_filter & (1u << platform_id)) == 0);

    CL_err = hc_clGetDeviceIDs (opencl_ctx->ocl, platform, CL_DEVICE_TYPE_ALL, DEVICES_MAX, platform_devices, &platform_devices_cnt);

    if (CL_err != CL_SUCCESS)
    {
      //log_error ("ERROR: clGetDeviceIDs(): %s\n", val2cstr_cl (CL_err));

      //return -1;

      platform_skipped = 2;
    }

    if (opencl_info)
    {
      char platform_name[HCBUFSIZ_TINY] = { 0 };

      CL_err = hc_clGetPlatformInfo (opencl_ctx->ocl, platform, CL_PLATFORM_NAME, HCBUFSIZ_TINY, platform_name, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetPlatformInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      char platform_version[HCBUFSIZ_TINY] = { 0 };

      CL_err = hc_clGetPlatformInfo (opencl_ctx->ocl, platform, CL_PLATFORM_VERSION, sizeof (platform_version), platform_version, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetPlatformInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      fprintf (stdout, "\nPlatform ID #%u\n  Vendor   : %s\n  Name     : %s\n  Version  : %s\n\n", platform_id, platform_vendor, platform_name, platform_version);
    }

    if ((benchmark == 1 || quiet == 0) && (algorithm_pos == 0))
    {
      if (machine_readable == 0)
      {
        if (platform_skipped == 0)
        {
          const int len = log_info ("OpenCL Platform #%u: %s", platform_id + 1, platform_vendor);

          char line[256] = { 0 };

          for (int i = 0; i < len; i++) line[i] = '=';

          log_info (line);
        }
        else if (platform_skipped == 1)
        {
          log_info ("OpenCL Platform #%u: %s, skipped", platform_id + 1, platform_vendor);
          log_info ("");
        }
        else if (platform_skipped == 2)
        {
          log_info ("OpenCL Platform #%u: %s, skipped! No OpenCL compatible devices found", platform_id + 1, platform_vendor);
          log_info ("");
        }
      }
    }

    if (platform_skipped == 1) continue;
    if (platform_skipped == 2) continue;

    hc_device_param_t *devices_param = opencl_ctx->devices_param;

    for (uint platform_devices_id = 0; platform_devices_id < platform_devices_cnt; platform_devices_id++)
    {
      size_t param_value_size = 0;

      const uint device_id = devices_cnt;

      hc_device_param_t *device_param = &devices_param[device_id];

      device_param->platform_vendor_id = platform_vendor_id;

      device_param->device = platform_devices[platform_devices_id];

      device_param->device_id = device_id;

      device_param->platform_devices_id = platform_devices_id;

      device_param->platform = platform;

      // device_type

      cl_device_type device_type;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_TYPE, sizeof (device_type), &device_type, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      device_type &= ~CL_DEVICE_TYPE_DEFAULT;

      device_param->device_type = device_type;

      // device_name

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_NAME, 0, NULL, &param_value_size);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      char *device_name = (char *) mymalloc (param_value_size);

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_NAME, param_value_size, device_name, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      device_param->device_name = device_name;

      // device_vendor

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_VENDOR, 0, NULL, &param_value_size);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      char *device_vendor = (char *) mymalloc (param_value_size);

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_VENDOR, param_value_size, device_vendor, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      device_param->device_vendor = device_vendor;

      cl_uint device_vendor_id = 0;

      if (strcmp (device_vendor, CL_VENDOR_AMD) == 0)
      {
        device_vendor_id = VENDOR_ID_AMD;
      }
      else if (strcmp (device_vendor, CL_VENDOR_AMD_USE_INTEL) == 0)
      {
        device_vendor_id = VENDOR_ID_AMD_USE_INTEL;
      }
      else if (strcmp (device_vendor, CL_VENDOR_APPLE) == 0)
      {
        device_vendor_id = VENDOR_ID_APPLE;
      }
      else if (strcmp (device_vendor, CL_VENDOR_INTEL_BEIGNET) == 0)
      {
        device_vendor_id = VENDOR_ID_INTEL_BEIGNET;
      }
      else if (strcmp (device_vendor, CL_VENDOR_INTEL_SDK) == 0)
      {
        device_vendor_id = VENDOR_ID_INTEL_SDK;
      }
      else if (strcmp (device_vendor, CL_VENDOR_MESA) == 0)
      {
        device_vendor_id = VENDOR_ID_MESA;
      }
      else if (strcmp (device_vendor, CL_VENDOR_NV) == 0)
      {
        device_vendor_id = VENDOR_ID_NV;
      }
      else if (strcmp (device_vendor, CL_VENDOR_POCL) == 0)
      {
        device_vendor_id = VENDOR_ID_POCL;
      }
      else
      {
        device_vendor_id = VENDOR_ID_GENERIC;
      }

      device_param->device_vendor_id = device_vendor_id;

      // device_version

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_VERSION, 0, NULL, &param_value_size);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      char *device_version = (char *) mymalloc (param_value_size);

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_VERSION, param_value_size, device_version, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      device_param->device_version = device_version;

      // device_opencl_version

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_OPENCL_C_VERSION, 0, NULL, &param_value_size);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      char *device_opencl_version = (char *) mymalloc (param_value_size);

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_OPENCL_C_VERSION, param_value_size, device_opencl_version, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      device_param->opencl_v12 = device_opencl_version[9] > '1' || device_opencl_version[11] >= '2';

      // vector_width

      cl_uint vector_width;

      if (opencl_ctx->opencl_vector_width_chgd == 0)
      {
        // tuning db

        tuning_db_entry_t *tuningdb_entry = tuning_db_search (tuning_db, device_param->device_name, device_param->device_type, attack_mode, hashconfig->hash_mode);

        if (tuningdb_entry == NULL || tuningdb_entry->vector_width == -1)
        {
          if (hashconfig->opti_type & OPTI_TYPE_USES_BITS_64)
          {
            CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_NATIVE_VECTOR_WIDTH_LONG, sizeof (vector_width), &vector_width, NULL);

            if (CL_err != CL_SUCCESS)
            {
              log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

              return -1;
            }
          }
          else
          {
            CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_NATIVE_VECTOR_WIDTH_INT,  sizeof (vector_width), &vector_width, NULL);

            if (CL_err != CL_SUCCESS)
            {
              log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

              return -1;
            }
          }
        }
        else
        {
          vector_width = (cl_uint) tuningdb_entry->vector_width;
        }
      }
      else
      {
        vector_width = opencl_ctx->opencl_vector_width;
      }

      if (vector_width > 16) vector_width = 16;

      device_param->vector_width = vector_width;

      // max_compute_units

      cl_uint device_processors;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof (device_processors), &device_processors, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      device_param->device_processors = device_processors;

      // device_maxmem_alloc
      // note we'll limit to 2gb, otherwise this causes all kinds of weird errors because of possible integer overflows in opencl runtimes

      cl_ulong device_maxmem_alloc;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof (device_maxmem_alloc), &device_maxmem_alloc, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      device_param->device_maxmem_alloc = MIN (device_maxmem_alloc, 0x7fffffff);

      // device_global_mem

      cl_ulong device_global_mem;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof (device_global_mem), &device_global_mem, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      device_param->device_global_mem = device_global_mem;

      // max_work_group_size

      size_t device_maxworkgroup_size;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof (device_maxworkgroup_size), &device_maxworkgroup_size, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      device_param->device_maxworkgroup_size = device_maxworkgroup_size;

      // max_clock_frequency

      cl_uint device_maxclock_frequency;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof (device_maxclock_frequency), &device_maxclock_frequency, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      device_param->device_maxclock_frequency = device_maxclock_frequency;

      // device_endian_little

      cl_bool device_endian_little;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_ENDIAN_LITTLE, sizeof (device_endian_little), &device_endian_little, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      if (device_endian_little == CL_FALSE)
      {
        log_info ("- Device #%u: WARNING: Not a little endian device", device_id + 1);

        device_param->skipped = 1;
      }

      // device_available

      cl_bool device_available;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_AVAILABLE, sizeof (device_available), &device_available, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      if (device_available == CL_FALSE)
      {
        log_info ("- Device #%u: WARNING: Device not available", device_id + 1);

        device_param->skipped = 1;
      }

      // device_compiler_available

      cl_bool device_compiler_available;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_COMPILER_AVAILABLE, sizeof (device_compiler_available), &device_compiler_available, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      if (device_compiler_available == CL_FALSE)
      {
        log_info ("- Device #%u: WARNING: No compiler available for device", device_id + 1);

        device_param->skipped = 1;
      }

      // device_execution_capabilities

      cl_device_exec_capabilities device_execution_capabilities;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_EXECUTION_CAPABILITIES, sizeof (device_execution_capabilities), &device_execution_capabilities, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      if ((device_execution_capabilities & CL_EXEC_KERNEL) == 0)
      {
        log_info ("- Device #%u: WARNING: Device does not support executing kernels", device_id + 1);

        device_param->skipped = 1;
      }

      // device_extensions

      size_t device_extensions_size;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_EXTENSIONS, 0, NULL, &device_extensions_size);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      char *device_extensions = mymalloc (device_extensions_size + 1);

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_EXTENSIONS, device_extensions_size, device_extensions, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      if (strstr (device_extensions, "base_atomics") == 0)
      {
        log_info ("- Device #%u: WARNING: Device does not support base atomics", device_id + 1);

        device_param->skipped = 1;
      }

      if (strstr (device_extensions, "byte_addressable_store") == 0)
      {
        log_info ("- Device #%u: WARNING: Device does not support byte addressable store", device_id + 1);

        device_param->skipped = 1;
      }

      myfree (device_extensions);

      // device_local_mem_size

      cl_ulong device_local_mem_size;

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_LOCAL_MEM_SIZE, sizeof (device_local_mem_size), &device_local_mem_size, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      if (device_local_mem_size < 32768)
      {
        log_info ("- Device #%u: WARNING: Device local mem size is too small", device_id + 1);

        device_param->skipped = 1;
      }

      // If there's both an Intel CPU and an AMD OpenCL runtime it's a tricky situation
      // Both platforms support CPU device types and therefore both will try to use 100% of the physical resources
      // This results in both utilizing it for 50%
      // However, Intel has much better SIMD control over their own hardware
      // It makes sense to give them full control over their own hardware

      if (device_type & CL_DEVICE_TYPE_CPU)
      {
        if (device_param->device_vendor_id == VENDOR_ID_AMD_USE_INTEL)
        {
          if (force == 0)
          {
            if (algorithm_pos == 0)
            {
              log_info ("- Device #%u: WARNING: Not a native Intel OpenCL runtime, expect massive speed loss", device_id + 1);
              log_info ("             You can use --force to override this but do not post error reports if you do so");
            }

            device_param->skipped = 1;
          }
        }
      }

      // skipped

      device_param->skipped |= ((opencl_ctx->devices_filter      & (1u << device_id)) == 0);
      device_param->skipped |= ((opencl_ctx->device_types_filter & (device_type))    == 0);

      // driver_version

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DRIVER_VERSION, 0, NULL, &param_value_size);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      char *driver_version = (char *) mymalloc (param_value_size);

      CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DRIVER_VERSION, param_value_size, driver_version, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      device_param->driver_version = driver_version;

      // device_name_chksum

      char *device_name_chksum = (char *) mymalloc (HCBUFSIZ_TINY);

      #if defined (__x86_64__)
      snprintf (device_name_chksum, HCBUFSIZ_TINY - 1, "%u-%u-%u-%s-%s-%s-%u", 64, device_param->platform_vendor_id, device_param->vector_width, device_param->device_name, device_param->device_version, device_param->driver_version, comptime);
      #else
      snprintf (device_name_chksum, HCBUFSIZ_TINY - 1, "%u-%u-%u-%s-%s-%s-%u", 32, device_param->platform_vendor_id, device_param->vector_width, device_param->device_name, device_param->device_version, device_param->driver_version, comptime);
      #endif

      uint device_name_digest[4] = { 0 };

      md5_64 ((uint *) device_name_chksum, device_name_digest);

      snprintf (device_name_chksum, HCBUFSIZ_TINY - 1, "%08x", device_name_digest[0]);

      device_param->device_name_chksum = device_name_chksum;

      // vendor specific

      if (device_param->device_type & CL_DEVICE_TYPE_GPU)
      {
        if ((device_param->platform_vendor_id == VENDOR_ID_AMD) && (device_param->device_vendor_id == VENDOR_ID_AMD))
        {
          need_adl = 1;
        }

        if ((device_param->platform_vendor_id == VENDOR_ID_NV) && (device_param->device_vendor_id == VENDOR_ID_NV))
        {
          need_nvml = 1;

          #if defined (__linux__)
          need_xnvctrl = 1;
          #endif

          #if defined (_WIN)
          need_nvapi = 1;
          #endif
        }
      }

      if (device_type & CL_DEVICE_TYPE_GPU)
      {
        if (device_vendor_id == VENDOR_ID_NV)
        {
          cl_uint kernel_exec_timeout = 0;

          #define CL_DEVICE_KERNEL_EXEC_TIMEOUT_NV            0x4005

          CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_KERNEL_EXEC_TIMEOUT_NV, sizeof (kernel_exec_timeout), &kernel_exec_timeout, NULL);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }

          device_param->kernel_exec_timeout = kernel_exec_timeout;

          cl_uint sm_minor = 0;
          cl_uint sm_major = 0;

          #define CL_DEVICE_COMPUTE_CAPABILITY_MAJOR_NV       0x4000
          #define CL_DEVICE_COMPUTE_CAPABILITY_MINOR_NV       0x4001

          CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_COMPUTE_CAPABILITY_MINOR_NV, sizeof (sm_minor), &sm_minor, NULL);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }

          CL_err = hc_clGetDeviceInfo (opencl_ctx->ocl, device_param->device, CL_DEVICE_COMPUTE_CAPABILITY_MAJOR_NV, sizeof (sm_major), &sm_major, NULL);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clGetDeviceInfo(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }

          device_param->sm_minor = sm_minor;
          device_param->sm_major = sm_major;

          // CPU burning loop damper
          // Value is given as number between 0-100
          // By default 100%

          device_param->nvidia_spin_damp = (double) opencl_ctx->nvidia_spin_damp;

          if (opencl_ctx->nvidia_spin_damp_chgd == 0)
          {
            if (attack_mode == ATTACK_MODE_STRAIGHT)
            {
              /**
               * the workaround is not a friend of rule based attacks
               * the words from the wordlist combined with fast and slow rules cause
               * fluctuations which cause inaccurate wait time estimations
               * using a reduced damping percentage almost compensates this
               */

              device_param->nvidia_spin_damp = 64;
            }
          }

          device_param->nvidia_spin_damp /= 100;
        }
      }

      // display results

      if (opencl_info)
      {
        char *format = "  Device ID #%u\n    Type           : %s\n    Vendor ID      : %u\n    Vendor         : %s\n    Name           : %s\n    Processor(s)   : %u\n    Clock          : %u\n    Memory         : %lu/%lu MB allocatable\n    OpenCL Version : %s\n\n";

        fprintf(stdout, format, device_id,
                ((device_type & CL_DEVICE_TYPE_CPU) ? "Cpu" : ((device_type & CL_DEVICE_TYPE_GPU) ? "Gpu" : "Accelerator")),
                device_vendor_id, device_vendor,
                device_name, device_processors,
                device_maxclock_frequency,
                device_maxmem_alloc/1024/1024, device_global_mem/1024/1024,
                device_opencl_version);
      }

      myfree (device_opencl_version);

      if ((benchmark == 1 || quiet == 0) && (algorithm_pos == 0))
      {
        if (machine_readable == 0)
        {
          if (device_param->skipped == 0)
          {
            log_info ("- Device #%u: %s, %lu/%lu MB allocatable, %uMCU",
                      device_id + 1,
                      device_name,
                      (unsigned int) (device_maxmem_alloc / 1024 / 1024),
                      (unsigned int) (device_global_mem   / 1024 / 1024),
                      (unsigned int)  device_processors);
          }
          else
          {
            log_info ("- Device #%u: %s, skipped",
                      device_id + 1,
                      device_name);
          }
        }
      }

      // common driver check

      if (device_param->skipped == 0)
      {
        if (device_type & CL_DEVICE_TYPE_GPU)
        {
          if (platform_vendor_id == VENDOR_ID_AMD)
          {
            int catalyst_check = (force == 1) ? 0 : 1;

            int catalyst_warn = 0;

            int catalyst_broken = 0;

            if (catalyst_check == 1)
            {
              catalyst_warn = 1;

              // v14.9 and higher
              if (atoi (device_param->driver_version) >= 1573)
              {
                catalyst_warn = 0;
              }

              catalyst_check = 0;
            }

            if (catalyst_broken == 1)
            {
              log_info ("");
              log_info ("ATTENTION! The Catalyst driver installed on your system is known to be broken!");
              log_info ("It passes over cracked hashes and will not report them as cracked");
              log_info ("You are STRONGLY encouraged not to use it");
              log_info ("You can use --force to override this but do not post error reports if you do so");
              log_info ("");

              return -1;
            }

            if (catalyst_warn == 1)
            {
              log_info ("");
              log_info ("ATTENTION! Unsupported or incorrectly installed Catalyst driver detected!");
              log_info ("You are STRONGLY encouraged to use the official supported catalyst driver");
              log_info ("See hashcat's homepage for official supported catalyst drivers");
              #if defined (_WIN)
              log_info ("Also see: http://hashcat.net/wiki/doku.php?id=upgrading_amd_drivers_how_to");
              #endif
              log_info ("You can use --force to override this but do not post error reports if you do so");
              log_info ("");

              return -1;
            }
          }
          else if (platform_vendor_id == VENDOR_ID_NV)
          {
            if (device_param->kernel_exec_timeout != 0)
            {
              if (quiet == 0) log_info ("- Device #%u: WARNING! Kernel exec timeout is not disabled, it might cause you errors of code 702", device_id + 1);
              if (quiet == 0) log_info ("             See the wiki on how to disable it: https://hashcat.net/wiki/doku.php?id=timeout_patch");
            }
          }
        }

        /* turns out pocl still creates segfaults (because of llvm)
        if (device_type & CL_DEVICE_TYPE_CPU)
        {
          if (platform_vendor_id == VENDOR_ID_AMD)
          {
            if (force == 0)
            {
              log_info ("");
              log_info ("ATTENTION! OpenCL support for CPU of catalyst driver is not reliable.");
              log_info ("You are STRONGLY encouraged not to use it");
              log_info ("You can use --force to override this but do not post error reports if you do so");
              log_info ("A good alternative is the free pocl >= v0.13, but make sure to use a LLVM >= v3.8");
              log_info ("");

              return -1;
            }
          }
        }
        */

        /**
         * kernel accel and loops tuning db adjustment
         */

        device_param->kernel_accel_min = 1;
        device_param->kernel_accel_max = 1024;

        device_param->kernel_loops_min = 1;
        device_param->kernel_loops_max = 1024;

        tuning_db_entry_t *tuningdb_entry = tuning_db_search (tuning_db, device_param->device_name, device_param->device_type, attack_mode, hashconfig->hash_mode);

        if (tuningdb_entry != NULL)
        {
          u32 _kernel_accel = tuningdb_entry->kernel_accel;
          u32 _kernel_loops = tuningdb_entry->kernel_loops;

          if (_kernel_accel)
          {
            device_param->kernel_accel_min = _kernel_accel;
            device_param->kernel_accel_max = _kernel_accel;
          }

          if (_kernel_loops)
          {
            if (opencl_ctx->workload_profile == 1)
            {
              _kernel_loops = (_kernel_loops > 8) ? _kernel_loops / 8 : 1;
            }
            else if (opencl_ctx->workload_profile == 2)
            {
              _kernel_loops = (_kernel_loops > 4) ? _kernel_loops / 4 : 1;
            }

            device_param->kernel_loops_min = _kernel_loops;
            device_param->kernel_loops_max = _kernel_loops;
          }
        }

        // commandline parameters overwrite tuningdb entries

        if (opencl_ctx->kernel_accel_chgd == 1)
        {
          device_param->kernel_accel_min = opencl_ctx->kernel_accel;
          device_param->kernel_accel_max = opencl_ctx->kernel_accel;
        }

        if (opencl_ctx->kernel_loops_chgd == 1)
        {
          device_param->kernel_loops_min = opencl_ctx->kernel_loops;
          device_param->kernel_loops_max = opencl_ctx->kernel_loops;
        }

        /**
         * activate device
         */

        devices_active++;
      }

      // next please

      devices_cnt++;
    }

    if ((benchmark == 1 || quiet == 0) && (algorithm_pos == 0))
    {
      if (machine_readable == 0)
      {
        log_info ("");
      }
    }
  }

  if (opencl_info)
  {
    exit(0);
  }

  if (devices_active == 0)
  {
    log_error ("ERROR: No devices found/left");

    return -1;
  }

  // additional check to see if the user has chosen a device that is not within the range of available devices (i.e. larger than devices_cnt)

  if (opencl_ctx->devices_filter != (uint) -1)
  {
    const uint devices_cnt_mask = ~(((uint) -1 >> devices_cnt) << devices_cnt);

    if (opencl_ctx->devices_filter > devices_cnt_mask)
    {
      log_error ("ERROR: The device specified by the --opencl-devices parameter is larger than the number of available devices (%d)", devices_cnt);

      return -1;
    }
  }

  opencl_ctx->devices_cnt    = devices_cnt;
  opencl_ctx->devices_active = devices_active;

  opencl_ctx->need_adl       = need_adl;
  opencl_ctx->need_nvml      = need_nvml;
  opencl_ctx->need_nvapi     = need_nvapi;
  opencl_ctx->need_xnvctrl   = need_xnvctrl;

  return 0;
}

void opencl_ctx_devices_destroy (opencl_ctx_t *opencl_ctx)
{
  for (uint device_id = 0; device_id < opencl_ctx->devices_cnt; device_id++)
  {
    hc_device_param_t *device_param = &opencl_ctx->devices_param[device_id];

    if (device_param->skipped) continue;

    myfree (device_param->device_name);
    myfree (device_param->device_name_chksum);
    myfree (device_param->device_version);
    myfree (device_param->driver_version);
  }

  opencl_ctx->devices_cnt    = 0;
  opencl_ctx->devices_active = 0;

  opencl_ctx->need_adl       = 0;
  opencl_ctx->need_nvml      = 0;
  opencl_ctx->need_nvapi     = 0;
  opencl_ctx->need_xnvctrl   = 0;
}

int opencl_session_begin (opencl_ctx_t *opencl_ctx, const hashconfig_t *hashconfig, const hashes_t *hashes, const session_ctx_t *session_ctx)
{
  for (uint device_id = 0; device_id < opencl_ctx->devices_cnt; device_id++)
  {
    cl_int CL_err = CL_SUCCESS;

    /**
     * host buffer
     */

    hc_device_param_t *device_param = &opencl_ctx->devices_param[device_id];

    if (device_param->skipped) continue;

    /**
     * device properties
     */

    const char *device_name_chksum  = device_param->device_name_chksum;
    const u32   device_processors   = device_param->device_processors;

    /**
     * create context for each device
     */

    cl_context_properties properties[3];

    properties[0] = CL_CONTEXT_PLATFORM;
    properties[1] = (cl_context_properties) device_param->platform;
    properties[2] = 0;

    CL_err = hc_clCreateContext (opencl_ctx->ocl, properties, 1, &device_param->device, NULL, NULL, &device_param->context);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clCreateContext(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    /**
     * create command-queue
     */

    // not supported with NV
    // device_param->command_queue = hc_clCreateCommandQueueWithProperties (device_param->context, device_param->device, NULL);

    CL_err = hc_clCreateCommandQueue (opencl_ctx->ocl, device_param->context, device_param->device, CL_QUEUE_PROFILING_ENABLE, &device_param->command_queue);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clCreateCommandQueue(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    /**
     * kernel threads: some algorithms need a fixed kernel-threads count
     *                 because of shared memory usage or bitslice
     *                 there needs to be some upper limit, otherwise there's too much overhead
     */

    uint kernel_threads = MIN (KERNEL_THREADS_MAX, device_param->device_maxworkgroup_size);

    if (hashconfig->hash_mode ==  8900) kernel_threads = 64; // Scrypt
    if (hashconfig->hash_mode ==  9300) kernel_threads = 64; // Scrypt

    if (device_param->device_type & CL_DEVICE_TYPE_CPU)
    {
      kernel_threads = KERNEL_THREADS_MAX_CPU;
    }

    if (hashconfig->hash_mode ==  1500) kernel_threads = 64; // DES
    if (hashconfig->hash_mode ==  3000) kernel_threads = 64; // DES
    if (hashconfig->hash_mode ==  3100) kernel_threads = 64; // DES
    if (hashconfig->hash_mode ==  3200) kernel_threads = 8;  // Blowfish
    if (hashconfig->hash_mode ==  7500) kernel_threads = 64; // RC4
    if (hashconfig->hash_mode ==  8500) kernel_threads = 64; // DES
    if (hashconfig->hash_mode ==  9000) kernel_threads = 8;  // Blowfish
    if (hashconfig->hash_mode ==  9700) kernel_threads = 64; // RC4
    if (hashconfig->hash_mode ==  9710) kernel_threads = 64; // RC4
    if (hashconfig->hash_mode ==  9800) kernel_threads = 64; // RC4
    if (hashconfig->hash_mode ==  9810) kernel_threads = 64; // RC4
    if (hashconfig->hash_mode == 10400) kernel_threads = 64; // RC4
    if (hashconfig->hash_mode == 10410) kernel_threads = 64; // RC4
    if (hashconfig->hash_mode == 10500) kernel_threads = 64; // RC4
    if (hashconfig->hash_mode == 13100) kernel_threads = 64; // RC4
    if (hashconfig->hash_mode == 14000) kernel_threads = 64; // DES
    if (hashconfig->hash_mode == 14100) kernel_threads = 64; // DES

    device_param->kernel_threads = kernel_threads;

    device_param->hardware_power = device_processors * kernel_threads;

    /**
     * create input buffers on device : calculate size of fixed memory buffers
     */

    size_t size_root_css   = SP_PW_MAX *           sizeof (cs_t);
    size_t size_markov_css = SP_PW_MAX * CHARSIZ * sizeof (cs_t);

    device_param->size_root_css   = size_root_css;
    device_param->size_markov_css = size_markov_css;

    size_t size_results = sizeof (uint);

    device_param->size_results = size_results;

    size_t size_rules   = session_ctx->kernel_rules_cnt * sizeof (kernel_rule_t);
    size_t size_rules_c = KERNEL_RULES                  * sizeof (kernel_rule_t);

    size_t size_plains  = hashes->digests_cnt * sizeof (plain_t);
    size_t size_salts   = hashes->salts_cnt   * sizeof (salt_t);
    size_t size_esalts  = hashes->salts_cnt   * hashconfig->esalt_size;
    size_t size_shown   = hashes->digests_cnt * sizeof (uint);
    size_t size_digests = hashes->digests_cnt * hashconfig->dgst_size;

    device_param->size_plains   = size_plains;
    device_param->size_digests  = size_digests;
    device_param->size_shown    = size_shown;
    device_param->size_salts    = size_salts;

    size_t size_combs = KERNEL_COMBS * sizeof (comb_t);
    size_t size_bfs   = KERNEL_BFS   * sizeof (bf_t);
    size_t size_tm    = 32           * sizeof (bs_word_t);

    // scryptV stuff

    u32 scrypt_tmp_size   = 0;
    u32 scrypt_tmto_final = 0;

    size_t size_scrypt = 4;

    if ((hashconfig->hash_mode == 8900) || (hashconfig->hash_mode == 9300))
    {
      // we need to check that all hashes have the same scrypt settings

      const u32 scrypt_N = hashes->salts_buf[0].scrypt_N;
      const u32 scrypt_r = hashes->salts_buf[0].scrypt_r;
      const u32 scrypt_p = hashes->salts_buf[0].scrypt_p;

      for (uint i = 1; i < hashes->salts_cnt; i++)
      {
        if ((hashes->salts_buf[i].scrypt_N != scrypt_N)
         || (hashes->salts_buf[i].scrypt_r != scrypt_r)
         || (hashes->salts_buf[i].scrypt_p != scrypt_p))
        {
          log_error ("ERROR: Mixed scrypt settings not supported");

          return -1;
        }
      }

      scrypt_tmp_size = (128 * scrypt_r * scrypt_p);

      uint tmto_start = 0;
      uint tmto_stop  = 10;

      if (session_ctx->scrypt_tmto)
      {
        tmto_start = session_ctx->scrypt_tmto;
      }
      else
      {
        // in case the user did not specify the tmto manually
        // use some values known to run best (tested on 290x for AMD and GTX1080 for NV)

        if (hashconfig->hash_mode == 8900)
        {
          if (device_param->device_vendor_id == VENDOR_ID_AMD)
          {
            tmto_start = 3;
          }
          else if (device_param->device_vendor_id == VENDOR_ID_NV)
          {
            tmto_start = 2;
          }
        }
        else if (hashconfig->hash_mode == 9300)
        {
          if (device_param->device_vendor_id == VENDOR_ID_AMD)
          {
            tmto_start = 2;
          }
          else if (device_param->device_vendor_id == VENDOR_ID_NV)
          {
            tmto_start = 4;
          }
        }
      }

      device_param->kernel_accel_min = 1;
      device_param->kernel_accel_max = 8;

      uint tmto;

      for (tmto = tmto_start; tmto < tmto_stop; tmto++)
      {
        size_scrypt = (128 * scrypt_r) * scrypt_N;

        size_scrypt /= 1u << tmto;

        size_scrypt *= device_param->device_processors * device_param->kernel_threads * device_param->kernel_accel_max;

        if ((size_scrypt / 4) > device_param->device_maxmem_alloc)
        {
          if (session_ctx->quiet == 0) log_info ("WARNING: Not enough single-block device memory allocatable to use --scrypt-tmto %d, increasing...", tmto);

          continue;
        }

        if (size_scrypt > device_param->device_global_mem)
        {
          if (session_ctx->quiet == 0) log_info ("WARNING: Not enough total device memory allocatable to use --scrypt-tmto %d, increasing...", tmto);

          continue;
        }

        for (uint salts_pos = 0; salts_pos < hashes->salts_cnt; salts_pos++)
        {
          scrypt_tmto_final = tmto;
        }

        break;
      }

      if (tmto == tmto_stop)
      {
        log_error ("ERROR: Can't allocate enough device memory");

        return -1;
      }

      if (session_ctx->quiet == 0) log_info ("SCRYPT tmto optimizer value set to: %u, mem: %" PRIu64 "\n", scrypt_tmto_final, size_scrypt);
    }

    size_t size_scrypt4 = size_scrypt / 4;

    /**
     * some algorithms need a fixed kernel-loops count
     */

    if (hashconfig->hash_mode == 1500 && session_ctx->attack_mode == ATTACK_MODE_BF)
    {
      const u32 kernel_loops_fixed = 1024;

      device_param->kernel_loops_min = kernel_loops_fixed;
      device_param->kernel_loops_max = kernel_loops_fixed;
    }

    if (hashconfig->hash_mode == 3000 && session_ctx->attack_mode == ATTACK_MODE_BF)
    {
      const u32 kernel_loops_fixed = 1024;

      device_param->kernel_loops_min = kernel_loops_fixed;
      device_param->kernel_loops_max = kernel_loops_fixed;
    }

    if (hashconfig->hash_mode == 8900)
    {
      const u32 kernel_loops_fixed = 1;

      device_param->kernel_loops_min = kernel_loops_fixed;
      device_param->kernel_loops_max = kernel_loops_fixed;
    }

    if (hashconfig->hash_mode == 9300)
    {
      const u32 kernel_loops_fixed = 1;

      device_param->kernel_loops_min = kernel_loops_fixed;
      device_param->kernel_loops_max = kernel_loops_fixed;
    }

    if (hashconfig->hash_mode == 12500)
    {
      const u32 kernel_loops_fixed = ROUNDS_RAR3 / 16;

      device_param->kernel_loops_min = kernel_loops_fixed;
      device_param->kernel_loops_max = kernel_loops_fixed;
    }

    if (hashconfig->hash_mode == 14000 && session_ctx->attack_mode == ATTACK_MODE_BF)
    {
      const u32 kernel_loops_fixed = 1024;

      device_param->kernel_loops_min = kernel_loops_fixed;
      device_param->kernel_loops_max = kernel_loops_fixed;
    }

    if (hashconfig->hash_mode == 14100 && session_ctx->attack_mode == ATTACK_MODE_BF)
    {
      const u32 kernel_loops_fixed = 1024;

      device_param->kernel_loops_min = kernel_loops_fixed;
      device_param->kernel_loops_max = kernel_loops_fixed;
    }

    u32 kernel_accel_min = device_param->kernel_accel_min;
    u32 kernel_accel_max = device_param->kernel_accel_max;

    // find out if we would request too much memory on memory blocks which are based on kernel_accel

    size_t size_pws   = 4;
    size_t size_tmps  = 4;
    size_t size_hooks = 4;

    while (kernel_accel_max >= kernel_accel_min)
    {
      const u32 kernel_power_max = device_processors * kernel_threads * kernel_accel_max;

      // size_pws

      size_pws = kernel_power_max * sizeof (pw_t);

      // size_tmps

      switch (hashconfig->hash_mode)
      {
        case   400: size_tmps = kernel_power_max * sizeof (phpass_tmp_t);          break;
        case   500: size_tmps = kernel_power_max * sizeof (md5crypt_tmp_t);        break;
        case   501: size_tmps = kernel_power_max * sizeof (md5crypt_tmp_t);        break;
        case  1600: size_tmps = kernel_power_max * sizeof (md5crypt_tmp_t);        break;
        case  1800: size_tmps = kernel_power_max * sizeof (sha512crypt_tmp_t);     break;
        case  2100: size_tmps = kernel_power_max * sizeof (dcc2_tmp_t);            break;
        case  2500: size_tmps = kernel_power_max * sizeof (wpa_tmp_t);             break;
        case  3200: size_tmps = kernel_power_max * sizeof (bcrypt_tmp_t);          break;
        case  5200: size_tmps = kernel_power_max * sizeof (pwsafe3_tmp_t);         break;
        case  5800: size_tmps = kernel_power_max * sizeof (androidpin_tmp_t);      break;
        case  6211: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case  6212: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case  6213: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case  6221: size_tmps = kernel_power_max * sizeof (tc64_tmp_t);            break;
        case  6222: size_tmps = kernel_power_max * sizeof (tc64_tmp_t);            break;
        case  6223: size_tmps = kernel_power_max * sizeof (tc64_tmp_t);            break;
        case  6231: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case  6232: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case  6233: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case  6241: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case  6242: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case  6243: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case  6300: size_tmps = kernel_power_max * sizeof (md5crypt_tmp_t);        break;
        case  6400: size_tmps = kernel_power_max * sizeof (sha256aix_tmp_t);       break;
        case  6500: size_tmps = kernel_power_max * sizeof (sha512aix_tmp_t);       break;
        case  6600: size_tmps = kernel_power_max * sizeof (agilekey_tmp_t);        break;
        case  6700: size_tmps = kernel_power_max * sizeof (sha1aix_tmp_t);         break;
        case  6800: size_tmps = kernel_power_max * sizeof (lastpass_tmp_t);        break;
        case  7100: size_tmps = kernel_power_max * sizeof (pbkdf2_sha512_tmp_t);   break;
        case  7200: size_tmps = kernel_power_max * sizeof (pbkdf2_sha512_tmp_t);   break;
        case  7400: size_tmps = kernel_power_max * sizeof (sha256crypt_tmp_t);     break;
        case  7900: size_tmps = kernel_power_max * sizeof (drupal7_tmp_t);         break;
        case  8200: size_tmps = kernel_power_max * sizeof (pbkdf2_sha512_tmp_t);   break;
        case  8800: size_tmps = kernel_power_max * sizeof (androidfde_tmp_t);      break;
        case  8900: size_tmps = kernel_power_max * scrypt_tmp_size;                break;
        case  9000: size_tmps = kernel_power_max * sizeof (pwsafe2_tmp_t);         break;
        case  9100: size_tmps = kernel_power_max * sizeof (lotus8_tmp_t);          break;
        case  9200: size_tmps = kernel_power_max * sizeof (pbkdf2_sha256_tmp_t);   break;
        case  9300: size_tmps = kernel_power_max * scrypt_tmp_size;                break;
        case  9400: size_tmps = kernel_power_max * sizeof (office2007_tmp_t);      break;
        case  9500: size_tmps = kernel_power_max * sizeof (office2010_tmp_t);      break;
        case  9600: size_tmps = kernel_power_max * sizeof (office2013_tmp_t);      break;
        case 10000: size_tmps = kernel_power_max * sizeof (pbkdf2_sha256_tmp_t);   break;
        case 10200: size_tmps = kernel_power_max * sizeof (cram_md5_t);            break;
        case 10300: size_tmps = kernel_power_max * sizeof (saph_sha1_tmp_t);       break;
        case 10500: size_tmps = kernel_power_max * sizeof (pdf14_tmp_t);           break;
        case 10700: size_tmps = kernel_power_max * sizeof (pdf17l8_tmp_t);         break;
        case 10900: size_tmps = kernel_power_max * sizeof (pbkdf2_sha256_tmp_t);   break;
        case 11300: size_tmps = kernel_power_max * sizeof (bitcoin_wallet_tmp_t);  break;
        case 11600: size_tmps = kernel_power_max * sizeof (seven_zip_tmp_t);       break;
        case 11900: size_tmps = kernel_power_max * sizeof (pbkdf2_md5_tmp_t);      break;
        case 12000: size_tmps = kernel_power_max * sizeof (pbkdf2_sha1_tmp_t);     break;
        case 12100: size_tmps = kernel_power_max * sizeof (pbkdf2_sha512_tmp_t);   break;
        case 12200: size_tmps = kernel_power_max * sizeof (ecryptfs_tmp_t);        break;
        case 12300: size_tmps = kernel_power_max * sizeof (oraclet_tmp_t);         break;
        case 12400: size_tmps = kernel_power_max * sizeof (bsdicrypt_tmp_t);       break;
        case 12500: size_tmps = kernel_power_max * sizeof (rar3_tmp_t);            break;
        case 12700: size_tmps = kernel_power_max * sizeof (mywallet_tmp_t);        break;
        case 12800: size_tmps = kernel_power_max * sizeof (pbkdf2_sha256_tmp_t);   break;
        case 12900: size_tmps = kernel_power_max * sizeof (pbkdf2_sha256_tmp_t);   break;
        case 13000: size_tmps = kernel_power_max * sizeof (pbkdf2_sha256_tmp_t);   break;
        case 13200: size_tmps = kernel_power_max * sizeof (axcrypt_tmp_t);         break;
        case 13400: size_tmps = kernel_power_max * sizeof (keepass_tmp_t);         break;
        case 13600: size_tmps = kernel_power_max * sizeof (pbkdf2_sha1_tmp_t);     break;
        case 13711: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13712: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13713: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13721: size_tmps = kernel_power_max * sizeof (tc64_tmp_t);            break;
        case 13722: size_tmps = kernel_power_max * sizeof (tc64_tmp_t);            break;
        case 13723: size_tmps = kernel_power_max * sizeof (tc64_tmp_t);            break;
        case 13731: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13732: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13733: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13741: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13742: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13743: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13751: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13752: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13753: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13761: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13762: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
        case 13763: size_tmps = kernel_power_max * sizeof (tc_tmp_t);              break;
      };

      // size_hooks

      if ((hashconfig->opts_type & OPTS_TYPE_HOOK12) || (hashconfig->opts_type & OPTS_TYPE_HOOK23))
      {
        switch (hashconfig->hash_mode)
        {
        }
      }

      // now check if all device-memory sizes which depend on the kernel_accel_max amplifier are within its boundaries
      // if not, decrease amplifier and try again

      int memory_limit_hit = 0;

      if (size_pws   > device_param->device_maxmem_alloc) memory_limit_hit = 1;
      if (size_tmps  > device_param->device_maxmem_alloc) memory_limit_hit = 1;
      if (size_hooks > device_param->device_maxmem_alloc) memory_limit_hit = 1;

      const u64 size_total
        = session_ctx->bitmap_size
        + session_ctx->bitmap_size
        + session_ctx->bitmap_size
        + session_ctx->bitmap_size
        + session_ctx->bitmap_size
        + session_ctx->bitmap_size
        + session_ctx->bitmap_size
        + session_ctx->bitmap_size
        + size_bfs
        + size_combs
        + size_digests
        + size_esalts
        + size_hooks
        + size_markov_css
        + size_plains
        + size_pws
        + size_pws // not a bug
        + size_results
        + size_root_css
        + size_rules
        + size_rules_c
        + size_salts
        + size_scrypt4
        + size_scrypt4
        + size_scrypt4
        + size_scrypt4
        + size_shown
        + size_tm
        + size_tmps;

      if (size_total > device_param->device_global_mem) memory_limit_hit = 1;

      if (memory_limit_hit == 1)
      {
        kernel_accel_max--;

        continue;
      }

      break;
    }

    if (kernel_accel_max < kernel_accel_min)
    {
      log_error ("- Device #%u: Device does not provide enough allocatable device-memory to handle this attack", device_id + 1);

      return -1;
    }

    device_param->kernel_accel_min = kernel_accel_min;
    device_param->kernel_accel_max = kernel_accel_max;

    /*
    if (kernel_accel_max < kernel_accel)
    {
      if (session_ctx->quiet == 0) log_info ("- Device #%u: Reduced maximum kernel-accel to %u", device_id + 1, kernel_accel_max);

      device_param->kernel_accel = kernel_accel_max;
    }
    */

    device_param->size_bfs     = size_bfs;
    device_param->size_combs   = size_combs;
    device_param->size_rules   = size_rules;
    device_param->size_rules_c = size_rules_c;
    device_param->size_pws     = size_pws;
    device_param->size_tmps    = size_tmps;
    device_param->size_hooks   = size_hooks;

    /**
     * default building options
     */

    if (chdir (session_ctx->cpath_real) == -1)
    {
      log_error ("ERROR: %s: %s", session_ctx->cpath_real, strerror (errno));

      return -1;
    }

    char build_opts[1024] = { 0 };

    #if defined (_WIN)
    snprintf (build_opts, sizeof (build_opts) - 1, "-I \"%s\"", session_ctx->cpath_real);
    #else
    snprintf (build_opts, sizeof (build_opts) - 1, "-I %s", session_ctx->cpath_real);
    #endif

    // include check
    // this test needs to be done manually because of osx opencl runtime
    // if there's a problem with permission, its not reporting back and erroring out silently

    #define files_cnt 15

    const char *files_names[files_cnt] =
    {
      "inc_cipher_aes256.cl",
      "inc_cipher_serpent256.cl",
      "inc_cipher_twofish256.cl",
      "inc_common.cl",
      "inc_comp_multi_bs.cl",
      "inc_comp_multi.cl",
      "inc_comp_single_bs.cl",
      "inc_comp_single.cl",
      "inc_hash_constants.h",
      "inc_hash_functions.cl",
      "inc_rp.cl",
      "inc_rp.h",
      "inc_simd.cl",
      "inc_types.cl",
      "inc_vendor.cl",
    };

    for (int i = 0; i < files_cnt; i++)
    {
      FILE *fd = fopen (files_names[i], "r");

      if (fd == NULL)
      {
        log_error ("ERROR: %s: fopen(): %s", files_names[i], strerror (errno));

        return -1;
      }

      char buf[1];

      size_t n = fread (buf, 1, 1, fd);

      if (n != 1)
      {
        log_error ("ERROR: %s: fread(): %s", files_names[i], strerror (errno));

        return -1;
      }

      fclose (fd);
    }

    // we don't have sm_* on vendors not NV but it doesn't matter

    char build_opts_new[1024] = { 0 };

    #if defined (DEBUG)
    snprintf (build_opts_new, sizeof (build_opts_new) - 1, "%s -D VENDOR_ID=%u -D CUDA_ARCH=%d -D VECT_SIZE=%u -D DEVICE_TYPE=%u -D DGST_R0=%u -D DGST_R1=%u -D DGST_R2=%u -D DGST_R3=%u -D DGST_ELEM=%u -D KERN_TYPE=%u -D _unroll -cl-std=CL1.1", build_opts, device_param->device_vendor_id, (device_param->sm_major * 100) + device_param->sm_minor, device_param->vector_width, (u32) device_param->device_type, hashconfig->dgst_pos0, hashconfig->dgst_pos1, hashconfig->dgst_pos2, hashconfig->dgst_pos3, hashconfig->dgst_size / 4, hashconfig->kern_type);
    #else
    snprintf (build_opts_new, sizeof (build_opts_new) - 1, "%s -D VENDOR_ID=%u -D CUDA_ARCH=%d -D VECT_SIZE=%u -D DEVICE_TYPE=%u -D DGST_R0=%u -D DGST_R1=%u -D DGST_R2=%u -D DGST_R3=%u -D DGST_ELEM=%u -D KERN_TYPE=%u -D _unroll -cl-std=CL1.1 -w", build_opts, device_param->device_vendor_id, (device_param->sm_major * 100) + device_param->sm_minor, device_param->vector_width, (u32) device_param->device_type, hashconfig->dgst_pos0, hashconfig->dgst_pos1, hashconfig->dgst_pos2, hashconfig->dgst_pos3, hashconfig->dgst_size / 4, hashconfig->kern_type);
    #endif

    strncpy (build_opts, build_opts_new, sizeof (build_opts));

    #if defined (DEBUG)
    log_info ("- Device #%u: build_opts '%s'\n", device_id + 1, build_opts);
    #endif

    /**
     * main kernel
     */

    {
      /**
       * kernel source filename
       */

      char source_file[256] = { 0 };

      generate_source_kernel_filename (hashconfig->attack_exec, session_ctx->attack_kern, hashconfig->kern_type, session_ctx->shared_dir, source_file);

      struct stat sst;

      if (stat (source_file, &sst) == -1)
      {
        log_error ("ERROR: %s: %s", source_file, strerror (errno));

        return -1;
      }

      /**
       * kernel cached filename
       */

      char cached_file[256] = { 0 };

      generate_cached_kernel_filename (hashconfig->attack_exec, session_ctx->attack_kern, hashconfig->kern_type, session_ctx->profile_dir, device_name_chksum, cached_file);

      int cached = 1;

      struct stat cst;

      if ((stat (cached_file, &cst) == -1) || cst.st_size == 0)
      {
        cached = 0;
      }

      /**
       * kernel compile or load
       */

      size_t *kernel_lengths = (size_t *) mymalloc (sizeof (size_t));

      const u8 **kernel_sources = (const u8 **) mymalloc (sizeof (u8 *));

      if (opencl_ctx->force_jit_compilation == -1)
      {
        if (cached == 0)
        {
          if (session_ctx->quiet == 0) log_info ("- Device #%u: Kernel %s not found in cache! Building may take a while...", device_id + 1, filename_from_filepath (cached_file));

          load_kernel (source_file, 1, kernel_lengths, kernel_sources);

          CL_err = hc_clCreateProgramWithSource (opencl_ctx->ocl, device_param->context, 1, (const char **) kernel_sources, NULL, &device_param->program);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clCreateProgramWithSource(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }

          CL_err = hc_clBuildProgram (opencl_ctx->ocl, device_param->program, 1, &device_param->device, build_opts, NULL, NULL);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clBuildProgram(): %s\n", val2cstr_cl (CL_err));

            //return -1;
          }

          size_t build_log_size = 0;

          /*
          CL_err = hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program, device_param->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &build_log_size);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clGetProgramBuildInfo(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }
          */

          hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program, device_param->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &build_log_size);

          #if defined (DEBUG)
          if ((build_log_size != 0) || (CL_err != CL_SUCCESS))
          #else
          if (CL_err != CL_SUCCESS)
          #endif
          {
            char *build_log = (char *) mymalloc (build_log_size + 1);

            CL_err = hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program, device_param->device, CL_PROGRAM_BUILD_LOG, build_log_size, build_log, NULL);

            if (CL_err != CL_SUCCESS)
            {
              log_error ("ERROR: clGetProgramBuildInfo(): %s\n", val2cstr_cl (CL_err));

              return -1;
            }

            puts (build_log);

            myfree (build_log);
          }

          if (CL_err != CL_SUCCESS)
          {
            device_param->skipped = true;

            log_info ("- Device #%u: Kernel %s build failure. Proceeding without this device.", device_id + 1, source_file);

            continue;
          }

          size_t binary_size;

          CL_err = hc_clGetProgramInfo (opencl_ctx->ocl, device_param->program, CL_PROGRAM_BINARY_SIZES, sizeof (size_t), &binary_size, NULL);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clGetProgramInfo(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }

          u8 *binary = (u8 *) mymalloc (binary_size);

          CL_err = hc_clGetProgramInfo (opencl_ctx->ocl, device_param->program, CL_PROGRAM_BINARIES, sizeof (binary), &binary, NULL);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clGetProgramInfo(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }

          writeProgramBin (cached_file, binary, binary_size);

          local_free (binary);
        }
        else
        {
          #if defined (DEBUG)
          log_info ("- Device #%u: Kernel %s (%ld bytes)", device_id + 1, cached_file, cst.st_size);
          #endif

          load_kernel (cached_file, 1, kernel_lengths, kernel_sources);

          CL_err = hc_clCreateProgramWithBinary (opencl_ctx->ocl, device_param->context, 1, &device_param->device, kernel_lengths, (const u8 **) kernel_sources, NULL, &device_param->program);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clCreateProgramWithBinary(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }

          CL_err = hc_clBuildProgram (opencl_ctx->ocl, device_param->program, 1, &device_param->device, build_opts, NULL, NULL);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clBuildProgram(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }
        }
      }
      else
      {
        #if defined (DEBUG)
        log_info ("- Device #%u: Kernel %s (%ld bytes)", device_id + 1, source_file, sst.st_size);
        #endif

        load_kernel (source_file, 1, kernel_lengths, kernel_sources);

        CL_err = hc_clCreateProgramWithSource (opencl_ctx->ocl, device_param->context, 1, (const char **) kernel_sources, NULL, &device_param->program);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateProgramWithSource(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        char build_opts_update[1024] = { 0 };

        if (opencl_ctx->force_jit_compilation == 1500)
        {
          snprintf (build_opts_update, sizeof (build_opts_update) - 1, "%s -DDESCRYPT_SALT=%u", build_opts, hashes->salts_buf[0].salt_buf[0]);
        }
        else if (opencl_ctx->force_jit_compilation == 8900)
        {
          snprintf (build_opts_update, sizeof (build_opts_update) - 1, "%s -DSCRYPT_N=%u -DSCRYPT_R=%u -DSCRYPT_P=%u -DSCRYPT_TMTO=%u -DSCRYPT_TMP_ELEM=%u", build_opts, hashes->salts_buf[0].scrypt_N, hashes->salts_buf[0].scrypt_r, hashes->salts_buf[0].scrypt_p, 1 << scrypt_tmto_final, scrypt_tmp_size / 16);
        }
        else
        {
          snprintf (build_opts_update, sizeof (build_opts_update) - 1, "%s", build_opts);
        }

        CL_err = hc_clBuildProgram (opencl_ctx->ocl, device_param->program, 1, &device_param->device, build_opts_update, NULL, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clBuildProgram(): %s\n", val2cstr_cl (CL_err));

          //return -1;
        }

        size_t build_log_size = 0;

        /*
        CL_err = hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program, device_param->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &build_log_size);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clGetProgramBuildInfo(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
        */

        hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program, device_param->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &build_log_size);

        #if defined (DEBUG)
        if ((build_log_size != 0) || (CL_err != CL_SUCCESS))
        #else
        if (CL_err != CL_SUCCESS)
        #endif
        {
          char *build_log = (char *) mymalloc (build_log_size + 1);

          CL_err = hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program, device_param->device, CL_PROGRAM_BUILD_LOG, build_log_size, build_log, NULL);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clGetProgramBuildInfo(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }

          puts (build_log);

          myfree (build_log);
        }

        if (CL_err != CL_SUCCESS)
        {
          device_param->skipped = true;

          log_info ("- Device #%u: Kernel %s build failure. Proceeding without this device.", device_id + 1, source_file);
        }
      }

      local_free (kernel_lengths);
      local_free (kernel_sources[0]);
      local_free (kernel_sources);
    }

    /**
     * word generator kernel
     */

    if (session_ctx->attack_mode != ATTACK_MODE_STRAIGHT)
    {
      /**
       * kernel mp source filename
       */

      char source_file[256] = { 0 };

      generate_source_kernel_mp_filename (hashconfig->opti_type, hashconfig->opts_type, session_ctx->shared_dir, source_file);

      struct stat sst;

      if (stat (source_file, &sst) == -1)
      {
        log_error ("ERROR: %s: %s", source_file, strerror (errno));

        return -1;
      }

      /**
       * kernel mp cached filename
       */

      char cached_file[256] = { 0 };

      generate_cached_kernel_mp_filename (hashconfig->opti_type, hashconfig->opts_type, session_ctx->profile_dir, device_name_chksum, cached_file);

      int cached = 1;

      struct stat cst;

      if (stat (cached_file, &cst) == -1)
      {
        cached = 0;
      }

      /**
       * kernel compile or load
       */

      size_t *kernel_lengths = (size_t *) mymalloc (sizeof (size_t));

      const u8 **kernel_sources = (const u8 **) mymalloc (sizeof (u8 *));

      if (cached == 0)
      {
        if (session_ctx->quiet == 0) log_info ("- Device #%u: Kernel %s not found in cache! Building may take a while...", device_id + 1, filename_from_filepath (cached_file));
        if (session_ctx->quiet == 0) log_info ("");

        load_kernel (source_file, 1, kernel_lengths, kernel_sources);

        CL_err = hc_clCreateProgramWithSource (opencl_ctx->ocl, device_param->context, 1, (const char **) kernel_sources, NULL, &device_param->program_mp);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateProgramWithSource(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        CL_err = hc_clBuildProgram (opencl_ctx->ocl, device_param->program_mp, 1, &device_param->device, build_opts, NULL, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clBuildProgram(): %s\n", val2cstr_cl (CL_err));

          //return -1;
        }

        size_t build_log_size = 0;

        /*
        CL_err = hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program_mp, device_param->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &build_log_size);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clGetProgramBuildInfo(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
        */

        hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program_mp, device_param->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &build_log_size);

        #if defined (DEBUG)
        if ((build_log_size != 0) || (CL_err != CL_SUCCESS))
        #else
        if (CL_err != CL_SUCCESS)
        #endif
        {
          char *build_log = (char *) mymalloc (build_log_size + 1);

          CL_err = hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program_mp, device_param->device, CL_PROGRAM_BUILD_LOG, build_log_size, build_log, NULL);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clGetProgramBuildInfo(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }

          puts (build_log);

          myfree (build_log);
        }

        if (CL_err != CL_SUCCESS)
        {
          device_param->skipped = true;

          log_info ("- Device #%u: Kernel %s build failure. Proceeding without this device.", device_id + 1, source_file);

          continue;
        }

        size_t binary_size;

        CL_err = hc_clGetProgramInfo (opencl_ctx->ocl, device_param->program_mp, CL_PROGRAM_BINARY_SIZES, sizeof (size_t), &binary_size, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clGetProgramInfo(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        u8 *binary = (u8 *) mymalloc (binary_size);

        CL_err = hc_clGetProgramInfo (opencl_ctx->ocl, device_param->program_mp, CL_PROGRAM_BINARIES, sizeof (binary), &binary, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clGetProgramInfo(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        writeProgramBin (cached_file, binary, binary_size);

        local_free (binary);
      }
      else
      {
        #if defined (DEBUG)
        log_info ("- Device #%u: Kernel %s (%ld bytes)", device_id + 1, cached_file, cst.st_size);
        #endif

        load_kernel (cached_file, 1, kernel_lengths, kernel_sources);

        CL_err = hc_clCreateProgramWithBinary (opencl_ctx->ocl, device_param->context, 1, &device_param->device, kernel_lengths, (const u8 **) kernel_sources, NULL, &device_param->program_mp);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateProgramWithBinary(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        CL_err = hc_clBuildProgram (opencl_ctx->ocl, device_param->program_mp, 1, &device_param->device, build_opts, NULL, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clBuildProgram(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }

      local_free (kernel_lengths);
      local_free (kernel_sources[0]);
      local_free (kernel_sources);
    }

    /**
     * amplifier kernel
     */

    if (hashconfig->attack_exec == ATTACK_EXEC_INSIDE_KERNEL)
    {

    }
    else
    {
      /**
       * kernel amp source filename
       */

      char source_file[256] = { 0 };

      generate_source_kernel_amp_filename (session_ctx->attack_kern, session_ctx->shared_dir, source_file);

      struct stat sst;

      if (stat (source_file, &sst) == -1)
      {
        log_error ("ERROR: %s: %s", source_file, strerror (errno));

        return -1;
      }

      /**
       * kernel amp cached filename
       */

      char cached_file[256] = { 0 };

      generate_cached_kernel_amp_filename (session_ctx->attack_kern, session_ctx->profile_dir, device_name_chksum, cached_file);

      int cached = 1;

      struct stat cst;

      if (stat (cached_file, &cst) == -1)
      {
        cached = 0;
      }

      /**
       * kernel compile or load
       */

      size_t *kernel_lengths = (size_t *) mymalloc (sizeof (size_t));

      const u8 **kernel_sources = (const u8 **) mymalloc (sizeof (u8 *));

      if (cached == 0)
      {
        if (session_ctx->quiet == 0) log_info ("- Device #%u: Kernel %s not found in cache! Building may take a while...", device_id + 1, filename_from_filepath (cached_file));
        if (session_ctx->quiet == 0) log_info ("");

        load_kernel (source_file, 1, kernel_lengths, kernel_sources);

        CL_err = hc_clCreateProgramWithSource (opencl_ctx->ocl, device_param->context, 1, (const char **) kernel_sources, NULL, &device_param->program_amp);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateProgramWithSource(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        CL_err = hc_clBuildProgram (opencl_ctx->ocl, device_param->program_amp, 1, &device_param->device, build_opts, NULL, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clBuildProgram(): %s\n", val2cstr_cl (CL_err));

          //return -1;
        }

        size_t build_log_size = 0;

        /*
        CL_err = hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program_amp, device_param->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &build_log_size);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clGetProgramBuildInfo(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
        */

        hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program_amp, device_param->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &build_log_size);

        #if defined (DEBUG)
        if ((build_log_size != 0) || (CL_err != CL_SUCCESS))
        #else
        if (CL_err != CL_SUCCESS)
        #endif
        {
          char *build_log = (char *) mymalloc (build_log_size + 1);

          CL_err = hc_clGetProgramBuildInfo (opencl_ctx->ocl, device_param->program_amp, device_param->device, CL_PROGRAM_BUILD_LOG, build_log_size, build_log, NULL);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clGetProgramBuildInfo(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }

          puts (build_log);

          myfree (build_log);
        }

        if (CL_err != CL_SUCCESS)
        {
          device_param->skipped = true;

          log_info ("- Device #%u: Kernel %s build failure. Proceed without this device.", device_id + 1, source_file);

          continue;
        }

        size_t binary_size;

        CL_err = hc_clGetProgramInfo (opencl_ctx->ocl, device_param->program_amp, CL_PROGRAM_BINARY_SIZES, sizeof (size_t), &binary_size, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clGetProgramInfo(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        u8 *binary = (u8 *) mymalloc (binary_size);

        CL_err = hc_clGetProgramInfo (opencl_ctx->ocl, device_param->program_amp, CL_PROGRAM_BINARIES, sizeof (binary), &binary, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clGetProgramInfo(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        writeProgramBin (cached_file, binary, binary_size);

        local_free (binary);
      }
      else
      {
        #if defined (DEBUG)
        if (session_ctx->quiet == 0) log_info ("- Device #%u: Kernel %s (%ld bytes)", device_id + 1, cached_file, cst.st_size);
        #endif

        load_kernel (cached_file, 1, kernel_lengths, kernel_sources);

        CL_err = hc_clCreateProgramWithBinary (opencl_ctx->ocl, device_param->context, 1, &device_param->device, kernel_lengths, (const u8 **) kernel_sources, NULL, &device_param->program_amp);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateProgramWithBinary(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        CL_err = hc_clBuildProgram (opencl_ctx->ocl, device_param->program_amp, 1, &device_param->device, build_opts, NULL, NULL);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clBuildProgram(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }

      local_free (kernel_lengths);
      local_free (kernel_sources[0]);
      local_free (kernel_sources);
    }

    // return back to the folder we came from initially (workaround)

    if (chdir (session_ctx->cwd) == -1)
    {
      log_error ("ERROR: %s: %s", session_ctx->cwd, strerror (errno));

      return -1;
    }

    // some algorithm collide too fast, make that impossible

    if (session_ctx->benchmark == 1)
    {
      ((uint *) hashes->digests_buf)[0] = -1u;
      ((uint *) hashes->digests_buf)[1] = -1u;
      ((uint *) hashes->digests_buf)[2] = -1u;
      ((uint *) hashes->digests_buf)[3] = -1u;
    }

    /**
     * global buffers
     */

    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   size_pws,     NULL, &device_param->d_pws_buf);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   size_pws,     NULL, &device_param->d_pws_amp_buf);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_WRITE,  size_tmps,    NULL, &device_param->d_tmps);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_WRITE,  size_hooks,   NULL, &device_param->d_hooks);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   session_ctx->bitmap_size,  NULL, &device_param->d_bitmap_s1_a);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   session_ctx->bitmap_size,  NULL, &device_param->d_bitmap_s1_b);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   session_ctx->bitmap_size,  NULL, &device_param->d_bitmap_s1_c);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   session_ctx->bitmap_size,  NULL, &device_param->d_bitmap_s1_d);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   session_ctx->bitmap_size,  NULL, &device_param->d_bitmap_s2_a);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   session_ctx->bitmap_size,  NULL, &device_param->d_bitmap_s2_b);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   session_ctx->bitmap_size,  NULL, &device_param->d_bitmap_s2_c);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   session_ctx->bitmap_size,  NULL, &device_param->d_bitmap_s2_d);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_WRITE,  size_plains,  NULL, &device_param->d_plain_bufs);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   size_digests, NULL, &device_param->d_digests_buf);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_WRITE,  size_shown,   NULL, &device_param->d_digests_shown);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY,   size_salts,   NULL, &device_param->d_salt_bufs);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_WRITE,  size_results, NULL, &device_param->d_result);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_WRITE,  size_scrypt4, NULL, &device_param->d_scryptV0_buf);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_WRITE,  size_scrypt4, NULL, &device_param->d_scryptV1_buf);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_WRITE,  size_scrypt4, NULL, &device_param->d_scryptV2_buf);
    CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_WRITE,  size_scrypt4, NULL, &device_param->d_scryptV3_buf);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clCreateBuffer(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    CL_err |= hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_bitmap_s1_a,    CL_TRUE, 0, session_ctx->bitmap_size,  session_ctx->bitmap_s1_a,        0, NULL, NULL);
    CL_err |= hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_bitmap_s1_b,    CL_TRUE, 0, session_ctx->bitmap_size,  session_ctx->bitmap_s1_b,        0, NULL, NULL);
    CL_err |= hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_bitmap_s1_c,    CL_TRUE, 0, session_ctx->bitmap_size,  session_ctx->bitmap_s1_c,        0, NULL, NULL);
    CL_err |= hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_bitmap_s1_d,    CL_TRUE, 0, session_ctx->bitmap_size,  session_ctx->bitmap_s1_d,        0, NULL, NULL);
    CL_err |= hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_bitmap_s2_a,    CL_TRUE, 0, session_ctx->bitmap_size,  session_ctx->bitmap_s2_a,        0, NULL, NULL);
    CL_err |= hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_bitmap_s2_b,    CL_TRUE, 0, session_ctx->bitmap_size,  session_ctx->bitmap_s2_b,        0, NULL, NULL);
    CL_err |= hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_bitmap_s2_c,    CL_TRUE, 0, session_ctx->bitmap_size,  session_ctx->bitmap_s2_c,        0, NULL, NULL);
    CL_err |= hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_bitmap_s2_d,    CL_TRUE, 0, session_ctx->bitmap_size,  session_ctx->bitmap_s2_d,        0, NULL, NULL);
    CL_err |= hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_digests_buf,    CL_TRUE, 0, size_digests, hashes->digests_buf,   0, NULL, NULL);
    CL_err |= hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_digests_shown,  CL_TRUE, 0, size_shown,   hashes->digests_shown, 0, NULL, NULL);
    CL_err |= hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_salt_bufs,      CL_TRUE, 0, size_salts,   hashes->salts_buf,     0, NULL, NULL);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clEnqueueWriteBuffer(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    /**
     * special buffers
     */

    if (session_ctx->attack_kern == ATTACK_KERN_STRAIGHT)
    {
      CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_rules,   NULL, &device_param->d_rules);
      CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_rules_c, NULL, &device_param->d_rules_c);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clCreateBuffer(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      CL_err = hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_rules, CL_TRUE, 0, size_rules, session_ctx->kernel_rules_buf, 0, NULL, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clEnqueueWriteBuffer(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }
    else if (session_ctx->attack_kern == ATTACK_KERN_COMBI)
    {
      CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_combs,      NULL, &device_param->d_combs);
      CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_combs,      NULL, &device_param->d_combs_c);
      CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_root_css,   NULL, &device_param->d_root_css_buf);
      CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_markov_css, NULL, &device_param->d_markov_css_buf);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clCreateBuffer(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }
    else if (session_ctx->attack_kern == ATTACK_KERN_BF)
    {
      CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_bfs,        NULL, &device_param->d_bfs);
      CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_bfs,        NULL, &device_param->d_bfs_c);
      CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_tm,         NULL, &device_param->d_tm_c);
      CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_root_css,   NULL, &device_param->d_root_css_buf);
      CL_err |= hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_markov_css, NULL, &device_param->d_markov_css_buf);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clCreateBuffer(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }

    if (size_esalts)
    {
      CL_err = hc_clCreateBuffer (opencl_ctx->ocl, device_param->context, CL_MEM_READ_ONLY, size_esalts, NULL, &device_param->d_esalt_bufs);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clCreateBuffer(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      CL_err = hc_clEnqueueWriteBuffer (opencl_ctx->ocl, device_param->command_queue, device_param->d_esalt_bufs, CL_TRUE, 0, size_esalts, hashes->esalts_buf, 0, NULL, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clEnqueueWriteBuffer(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }

    /**
     * main host data
     */

    pw_t *pws_buf = (pw_t *) mymalloc (size_pws);

    device_param->pws_buf = pws_buf;

    comb_t *combs_buf = (comb_t *) mycalloc (KERNEL_COMBS, sizeof (comb_t));

    device_param->combs_buf = combs_buf;

    void *hooks_buf = mymalloc (size_hooks);

    device_param->hooks_buf = hooks_buf;

    /**
     * kernel args
     */

    device_param->kernel_params_buf32[24] = session_ctx->bitmap_mask;
    device_param->kernel_params_buf32[25] = session_ctx->bitmap_shift1;
    device_param->kernel_params_buf32[26] = session_ctx->bitmap_shift2;
    device_param->kernel_params_buf32[27] = 0; // salt_pos
    device_param->kernel_params_buf32[28] = 0; // loop_pos
    device_param->kernel_params_buf32[29] = 0; // loop_cnt
    device_param->kernel_params_buf32[30] = 0; // kernel_rules_cnt
    device_param->kernel_params_buf32[31] = 0; // digests_cnt
    device_param->kernel_params_buf32[32] = 0; // digests_offset
    device_param->kernel_params_buf32[33] = 0; // combs_mode
    device_param->kernel_params_buf32[34] = 0; // gid_max

    device_param->kernel_params[ 0] = (hashconfig->attack_exec == ATTACK_EXEC_INSIDE_KERNEL)
                                    ? &device_param->d_pws_buf
                                    : &device_param->d_pws_amp_buf;
    device_param->kernel_params[ 1] = &device_param->d_rules_c;
    device_param->kernel_params[ 2] = &device_param->d_combs_c;
    device_param->kernel_params[ 3] = &device_param->d_bfs_c;
    device_param->kernel_params[ 4] = &device_param->d_tmps;
    device_param->kernel_params[ 5] = &device_param->d_hooks;
    device_param->kernel_params[ 6] = &device_param->d_bitmap_s1_a;
    device_param->kernel_params[ 7] = &device_param->d_bitmap_s1_b;
    device_param->kernel_params[ 8] = &device_param->d_bitmap_s1_c;
    device_param->kernel_params[ 9] = &device_param->d_bitmap_s1_d;
    device_param->kernel_params[10] = &device_param->d_bitmap_s2_a;
    device_param->kernel_params[11] = &device_param->d_bitmap_s2_b;
    device_param->kernel_params[12] = &device_param->d_bitmap_s2_c;
    device_param->kernel_params[13] = &device_param->d_bitmap_s2_d;
    device_param->kernel_params[14] = &device_param->d_plain_bufs;
    device_param->kernel_params[15] = &device_param->d_digests_buf;
    device_param->kernel_params[16] = &device_param->d_digests_shown;
    device_param->kernel_params[17] = &device_param->d_salt_bufs;
    device_param->kernel_params[18] = &device_param->d_esalt_bufs;
    device_param->kernel_params[19] = &device_param->d_result;
    device_param->kernel_params[20] = &device_param->d_scryptV0_buf;
    device_param->kernel_params[21] = &device_param->d_scryptV1_buf;
    device_param->kernel_params[22] = &device_param->d_scryptV2_buf;
    device_param->kernel_params[23] = &device_param->d_scryptV3_buf;
    device_param->kernel_params[24] = &device_param->kernel_params_buf32[24];
    device_param->kernel_params[25] = &device_param->kernel_params_buf32[25];
    device_param->kernel_params[26] = &device_param->kernel_params_buf32[26];
    device_param->kernel_params[27] = &device_param->kernel_params_buf32[27];
    device_param->kernel_params[28] = &device_param->kernel_params_buf32[28];
    device_param->kernel_params[29] = &device_param->kernel_params_buf32[29];
    device_param->kernel_params[30] = &device_param->kernel_params_buf32[30];
    device_param->kernel_params[31] = &device_param->kernel_params_buf32[31];
    device_param->kernel_params[32] = &device_param->kernel_params_buf32[32];
    device_param->kernel_params[33] = &device_param->kernel_params_buf32[33];
    device_param->kernel_params[34] = &device_param->kernel_params_buf32[34];

    device_param->kernel_params_mp_buf64[3] = 0;
    device_param->kernel_params_mp_buf32[4] = 0;
    device_param->kernel_params_mp_buf32[5] = 0;
    device_param->kernel_params_mp_buf32[6] = 0;
    device_param->kernel_params_mp_buf32[7] = 0;
    device_param->kernel_params_mp_buf32[8] = 0;

    device_param->kernel_params_mp[0] = NULL;
    device_param->kernel_params_mp[1] = NULL;
    device_param->kernel_params_mp[2] = NULL;
    device_param->kernel_params_mp[3] = &device_param->kernel_params_mp_buf64[3];
    device_param->kernel_params_mp[4] = &device_param->kernel_params_mp_buf32[4];
    device_param->kernel_params_mp[5] = &device_param->kernel_params_mp_buf32[5];
    device_param->kernel_params_mp[6] = &device_param->kernel_params_mp_buf32[6];
    device_param->kernel_params_mp[7] = &device_param->kernel_params_mp_buf32[7];
    device_param->kernel_params_mp[8] = &device_param->kernel_params_mp_buf32[8];

    device_param->kernel_params_mp_l_buf64[3] = 0;
    device_param->kernel_params_mp_l_buf32[4] = 0;
    device_param->kernel_params_mp_l_buf32[5] = 0;
    device_param->kernel_params_mp_l_buf32[6] = 0;
    device_param->kernel_params_mp_l_buf32[7] = 0;
    device_param->kernel_params_mp_l_buf32[8] = 0;
    device_param->kernel_params_mp_l_buf32[9] = 0;

    device_param->kernel_params_mp_l[0] = NULL;
    device_param->kernel_params_mp_l[1] = NULL;
    device_param->kernel_params_mp_l[2] = NULL;
    device_param->kernel_params_mp_l[3] = &device_param->kernel_params_mp_l_buf64[3];
    device_param->kernel_params_mp_l[4] = &device_param->kernel_params_mp_l_buf32[4];
    device_param->kernel_params_mp_l[5] = &device_param->kernel_params_mp_l_buf32[5];
    device_param->kernel_params_mp_l[6] = &device_param->kernel_params_mp_l_buf32[6];
    device_param->kernel_params_mp_l[7] = &device_param->kernel_params_mp_l_buf32[7];
    device_param->kernel_params_mp_l[8] = &device_param->kernel_params_mp_l_buf32[8];
    device_param->kernel_params_mp_l[9] = &device_param->kernel_params_mp_l_buf32[9];

    device_param->kernel_params_mp_r_buf64[3] = 0;
    device_param->kernel_params_mp_r_buf32[4] = 0;
    device_param->kernel_params_mp_r_buf32[5] = 0;
    device_param->kernel_params_mp_r_buf32[6] = 0;
    device_param->kernel_params_mp_r_buf32[7] = 0;
    device_param->kernel_params_mp_r_buf32[8] = 0;

    device_param->kernel_params_mp_r[0] = NULL;
    device_param->kernel_params_mp_r[1] = NULL;
    device_param->kernel_params_mp_r[2] = NULL;
    device_param->kernel_params_mp_r[3] = &device_param->kernel_params_mp_r_buf64[3];
    device_param->kernel_params_mp_r[4] = &device_param->kernel_params_mp_r_buf32[4];
    device_param->kernel_params_mp_r[5] = &device_param->kernel_params_mp_r_buf32[5];
    device_param->kernel_params_mp_r[6] = &device_param->kernel_params_mp_r_buf32[6];
    device_param->kernel_params_mp_r[7] = &device_param->kernel_params_mp_r_buf32[7];
    device_param->kernel_params_mp_r[8] = &device_param->kernel_params_mp_r_buf32[8];

    device_param->kernel_params_amp_buf32[5] = 0; // combs_mode
    device_param->kernel_params_amp_buf32[6] = 0; // gid_max

    device_param->kernel_params_amp[0] = &device_param->d_pws_buf;
    device_param->kernel_params_amp[1] = &device_param->d_pws_amp_buf;
    device_param->kernel_params_amp[2] = &device_param->d_rules_c;
    device_param->kernel_params_amp[3] = &device_param->d_combs_c;
    device_param->kernel_params_amp[4] = &device_param->d_bfs_c;
    device_param->kernel_params_amp[5] = &device_param->kernel_params_amp_buf32[5];
    device_param->kernel_params_amp[6] = &device_param->kernel_params_amp_buf32[6];

    device_param->kernel_params_tm[0] = &device_param->d_bfs_c;
    device_param->kernel_params_tm[1] = &device_param->d_tm_c;

    device_param->kernel_params_memset_buf32[1] = 0; // value
    device_param->kernel_params_memset_buf32[2] = 0; // gid_max

    device_param->kernel_params_memset[0] = NULL;
    device_param->kernel_params_memset[1] = &device_param->kernel_params_memset_buf32[1];
    device_param->kernel_params_memset[2] = &device_param->kernel_params_memset_buf32[2];

    /**
     * kernel name
     */

    size_t kernel_wgs_tmp;

    char kernel_name[64] = { 0 };

    if (hashconfig->attack_exec == ATTACK_EXEC_INSIDE_KERNEL)
    {
      if (hashconfig->opti_type & OPTI_TYPE_SINGLE_HASH)
      {
        snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_s%02d", hashconfig->kern_type, 4);

        CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel1);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_s%02d", hashconfig->kern_type, 8);

        CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel2);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_s%02d", hashconfig->kern_type, 16);

        CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel3);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }
      else
      {
        snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_m%02d", hashconfig->kern_type, 4);

        CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel1);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_m%02d", hashconfig->kern_type, 8);

        CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel2);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_m%02d", hashconfig->kern_type, 16);

        CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel3);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }

      if (session_ctx->attack_mode == ATTACK_MODE_BF)
      {
        if (hashconfig->opts_type & OPTS_TYPE_PT_BITSLICE)
        {
          snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_tm", hashconfig->kern_type);

          CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel_tm);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }

          CL_err = hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel_tm, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);

          if (CL_err != CL_SUCCESS)
          {
            log_error ("ERROR: clGetKernelWorkGroupInfo(): %s\n", val2cstr_cl (CL_err));

            return -1;
          }
        }
      }
    }
    else
    {
      snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_init", hashconfig->kern_type);

      CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel1);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_loop", hashconfig->kern_type);

      CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel2);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_comp", hashconfig->kern_type);

      CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel3);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      if (hashconfig->opts_type & OPTS_TYPE_HOOK12)
      {
        snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_hook12", hashconfig->kern_type);

        CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel12);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        CL_err = hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel12, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clGetKernelWorkGroupInfo(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }

      if (hashconfig->opts_type & OPTS_TYPE_HOOK23)
      {
        snprintf (kernel_name, sizeof (kernel_name) - 1, "m%05d_hook23", hashconfig->kern_type);

        CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, kernel_name, &device_param->kernel23);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }

        CL_err = hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel23, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clGetKernelWorkGroupInfo(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }
    }

    CL_err |= hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel1, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);
    CL_err |= hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel2, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);
    CL_err |= hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel3, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clGetKernelWorkGroupInfo(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    for (uint i = 0; i <= 23; i++)
    {
      CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel1, i, sizeof (cl_mem), device_param->kernel_params[i]);
      CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel2, i, sizeof (cl_mem), device_param->kernel_params[i]);
      CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel3, i, sizeof (cl_mem), device_param->kernel_params[i]);

      if (hashconfig->opts_type & OPTS_TYPE_HOOK12) CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel12, i, sizeof (cl_mem), device_param->kernel_params[i]);
      if (hashconfig->opts_type & OPTS_TYPE_HOOK23) CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel23, i, sizeof (cl_mem), device_param->kernel_params[i]);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clSetKernelArg(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }

    for (uint i = 24; i <= 34; i++)
    {
      CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel1, i, sizeof (cl_uint), device_param->kernel_params[i]);
      CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel2, i, sizeof (cl_uint), device_param->kernel_params[i]);
      CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel3, i, sizeof (cl_uint), device_param->kernel_params[i]);

      if (hashconfig->opts_type & OPTS_TYPE_HOOK12) CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel12, i, sizeof (cl_uint), device_param->kernel_params[i]);
      if (hashconfig->opts_type & OPTS_TYPE_HOOK23) CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel23, i, sizeof (cl_uint), device_param->kernel_params[i]);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clSetKernelArg(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }

    // GPU memset

    CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program, "gpu_memset", &device_param->kernel_memset);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    CL_err = hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel_memset, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clGetKernelWorkGroupInfo(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel_memset, 0, sizeof (cl_mem),  device_param->kernel_params_memset[0]);
    CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel_memset, 1, sizeof (cl_uint), device_param->kernel_params_memset[1]);
    CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel_memset, 2, sizeof (cl_uint), device_param->kernel_params_memset[2]);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clSetKernelArg(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    // MP start

    if (session_ctx->attack_mode == ATTACK_MODE_BF)
    {
      CL_err |= hc_clCreateKernel (opencl_ctx->ocl, device_param->program_mp, "l_markov", &device_param->kernel_mp_l);
      CL_err |= hc_clCreateKernel (opencl_ctx->ocl, device_param->program_mp, "r_markov", &device_param->kernel_mp_r);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      CL_err |= hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel_mp_l, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);
      CL_err |= hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel_mp_r, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetKernelWorkGroupInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      if (hashconfig->opts_type & OPTS_TYPE_PT_BITSLICE)
      {
        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel_tm, 0, sizeof (cl_mem), device_param->kernel_params_tm[0]);
        CL_err |= hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel_tm, 1, sizeof (cl_mem), device_param->kernel_params_tm[1]);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clSetKernelArg(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }
    }
    else if (session_ctx->attack_mode == ATTACK_MODE_HYBRID1)
    {
      CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program_mp, "C_markov", &device_param->kernel_mp);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      CL_err = hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel_mp, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetKernelWorkGroupInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }
    else if (session_ctx->attack_mode == ATTACK_MODE_HYBRID2)
    {
      CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program_mp, "C_markov", &device_param->kernel_mp);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      CL_err = hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel_mp, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetKernelWorkGroupInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }

    if (hashconfig->attack_exec == ATTACK_EXEC_INSIDE_KERNEL)
    {
      // nothing to do
    }
    else
    {
      CL_err = hc_clCreateKernel (opencl_ctx->ocl, device_param->program_amp, "amp", &device_param->kernel_amp);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clCreateKernel(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }

      CL_err = hc_clGetKernelWorkGroupInfo (opencl_ctx->ocl, device_param->kernel_amp, device_param->device, CL_KERNEL_WORK_GROUP_SIZE, sizeof (size_t), &kernel_wgs_tmp, NULL); kernel_threads = MIN (kernel_threads, kernel_wgs_tmp);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clGetKernelWorkGroupInfo(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }

    if (hashconfig->attack_exec == ATTACK_EXEC_INSIDE_KERNEL)
    {
      // nothing to do
    }
    else
    {
      for (uint i = 0; i < 5; i++)
      {
        CL_err = hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel_amp, i, sizeof (cl_mem), device_param->kernel_params_amp[i]);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clSetKernelArg(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }

      for (uint i = 5; i < 7; i++)
      {
        CL_err = hc_clSetKernelArg (opencl_ctx->ocl, device_param->kernel_amp, i, sizeof (cl_uint), device_param->kernel_params_amp[i]);

        if (CL_err != CL_SUCCESS)
        {
          log_error ("ERROR: clSetKernelArg(): %s\n", val2cstr_cl (CL_err));

          return -1;
        }
      }
    }

    // maybe this has been updated by clGetKernelWorkGroupInfo()
    // value can only be decreased, so we don't need to reallocate buffers

    device_param->kernel_threads = kernel_threads;

    // zero some data buffers

    run_kernel_bzero (opencl_ctx, device_param, device_param->d_pws_buf,        size_pws);
    run_kernel_bzero (opencl_ctx, device_param, device_param->d_pws_amp_buf,    size_pws);
    run_kernel_bzero (opencl_ctx, device_param, device_param->d_tmps,           size_tmps);
    run_kernel_bzero (opencl_ctx, device_param, device_param->d_hooks,          size_hooks);
    run_kernel_bzero (opencl_ctx, device_param, device_param->d_plain_bufs,     size_plains);
    run_kernel_bzero (opencl_ctx, device_param, device_param->d_result,         size_results);

    /**
     * special buffers
     */

    if (session_ctx->attack_kern == ATTACK_KERN_STRAIGHT)
    {
      run_kernel_bzero (opencl_ctx, device_param, device_param->d_rules_c, size_rules_c);
    }
    else if (session_ctx->attack_kern == ATTACK_KERN_COMBI)
    {
      run_kernel_bzero (opencl_ctx, device_param, device_param->d_combs,          size_combs);
      run_kernel_bzero (opencl_ctx, device_param, device_param->d_combs_c,        size_combs);
      run_kernel_bzero (opencl_ctx, device_param, device_param->d_root_css_buf,   size_root_css);
      run_kernel_bzero (opencl_ctx, device_param, device_param->d_markov_css_buf, size_markov_css);
    }
    else if (session_ctx->attack_kern == ATTACK_KERN_BF)
    {
      run_kernel_bzero (opencl_ctx, device_param, device_param->d_bfs,            size_bfs);
      run_kernel_bzero (opencl_ctx, device_param, device_param->d_bfs_c,          size_bfs);
      run_kernel_bzero (opencl_ctx, device_param, device_param->d_tm_c,           size_tm);
      run_kernel_bzero (opencl_ctx, device_param, device_param->d_root_css_buf,   size_root_css);
      run_kernel_bzero (opencl_ctx, device_param, device_param->d_markov_css_buf, size_markov_css);
    }
  }

  return 0;
}

int opencl_session_destroy (opencl_ctx_t *opencl_ctx)
{
  for (uint device_id = 0; device_id < opencl_ctx->devices_cnt; device_id++)
  {
    hc_device_param_t *device_param = &opencl_ctx->devices_param[device_id];

    if (device_param->skipped) continue;

    cl_int CL_err = CL_SUCCESS;

    myfree (device_param->pws_buf);
    myfree (device_param->combs_buf);
    myfree (device_param->hooks_buf);

    if (device_param->d_pws_buf)          CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_pws_buf);
    if (device_param->d_pws_amp_buf)      CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_pws_amp_buf);
    if (device_param->d_rules)            CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_rules);
    if (device_param->d_rules_c)          CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_rules_c);
    if (device_param->d_combs)            CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_combs);
    if (device_param->d_combs_c)          CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_combs_c);
    if (device_param->d_bfs)              CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_bfs);
    if (device_param->d_bfs_c)            CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_bfs_c);
    if (device_param->d_bitmap_s1_a)      CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_bitmap_s1_a);
    if (device_param->d_bitmap_s1_b)      CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_bitmap_s1_b);
    if (device_param->d_bitmap_s1_c)      CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_bitmap_s1_c);
    if (device_param->d_bitmap_s1_d)      CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_bitmap_s1_d);
    if (device_param->d_bitmap_s2_a)      CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_bitmap_s2_a);
    if (device_param->d_bitmap_s2_b)      CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_bitmap_s2_b);
    if (device_param->d_bitmap_s2_c)      CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_bitmap_s2_c);
    if (device_param->d_bitmap_s2_d)      CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_bitmap_s2_d);
    if (device_param->d_plain_bufs)       CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_plain_bufs);
    if (device_param->d_digests_buf)      CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_digests_buf);
    if (device_param->d_digests_shown)    CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_digests_shown);
    if (device_param->d_salt_bufs)        CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_salt_bufs);
    if (device_param->d_esalt_bufs)       CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_esalt_bufs);
    if (device_param->d_tmps)             CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_tmps);
    if (device_param->d_hooks)            CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_hooks);
    if (device_param->d_result)           CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_result);
    if (device_param->d_scryptV0_buf)     CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_scryptV0_buf);
    if (device_param->d_scryptV1_buf)     CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_scryptV1_buf);
    if (device_param->d_scryptV2_buf)     CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_scryptV2_buf);
    if (device_param->d_scryptV3_buf)     CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_scryptV3_buf);
    if (device_param->d_root_css_buf)     CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_root_css_buf);
    if (device_param->d_markov_css_buf)   CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_markov_css_buf);
    if (device_param->d_tm_c)             CL_err |= hc_clReleaseMemObject (opencl_ctx->ocl, device_param->d_tm_c);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clReleaseMemObject(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    if (device_param->kernel1)        CL_err |= hc_clReleaseKernel (opencl_ctx->ocl, device_param->kernel1);
    if (device_param->kernel12)       CL_err |= hc_clReleaseKernel (opencl_ctx->ocl, device_param->kernel12);
    if (device_param->kernel2)        CL_err |= hc_clReleaseKernel (opencl_ctx->ocl, device_param->kernel2);
    if (device_param->kernel23)       CL_err |= hc_clReleaseKernel (opencl_ctx->ocl, device_param->kernel23);
    if (device_param->kernel3)        CL_err |= hc_clReleaseKernel (opencl_ctx->ocl, device_param->kernel3);
    if (device_param->kernel_mp)      CL_err |= hc_clReleaseKernel (opencl_ctx->ocl, device_param->kernel_mp);
    if (device_param->kernel_mp_l)    CL_err |= hc_clReleaseKernel (opencl_ctx->ocl, device_param->kernel_mp_l);
    if (device_param->kernel_mp_r)    CL_err |= hc_clReleaseKernel (opencl_ctx->ocl, device_param->kernel_mp_r);
    if (device_param->kernel_tm)      CL_err |= hc_clReleaseKernel (opencl_ctx->ocl, device_param->kernel_tm);
    if (device_param->kernel_amp)     CL_err |= hc_clReleaseKernel (opencl_ctx->ocl, device_param->kernel_amp);
    if (device_param->kernel_memset)  CL_err |= hc_clReleaseKernel (opencl_ctx->ocl, device_param->kernel_memset);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clReleaseKernel(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    if (device_param->program)     CL_err |= hc_clReleaseProgram (opencl_ctx->ocl, device_param->program);
    if (device_param->program_mp)  CL_err |= hc_clReleaseProgram (opencl_ctx->ocl, device_param->program_mp);
    if (device_param->program_amp) CL_err |= hc_clReleaseProgram (opencl_ctx->ocl, device_param->program_amp);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clReleaseProgram(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    if (device_param->command_queue) CL_err |= hc_clReleaseCommandQueue (opencl_ctx->ocl, device_param->command_queue);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clReleaseCommandQueue(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }

    if (device_param->context) CL_err |= hc_clReleaseContext (opencl_ctx->ocl, device_param->context);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: hc_clReleaseContext(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }
  }

  return 0;
}
