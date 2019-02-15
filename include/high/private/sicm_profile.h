#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <linux/perf_event.h>
#include <asm/perf_regs.h>
#include <asm/unistd.h>
#include <perfmon/pfmlib_perf_event.h>
#include <time.h>
#include <signal.h>
#include <stdbool.h>
#include <pthread.h>
#include <errno.h>
#include <poll.h>
#include "sicm_runtime.h"
#include "sicm_profilers.h"

/* Profiling information for one arena */
typedef struct profile_info {
  size_t first_interval, num_intervals;

  profile_all_info profile_all;
  profile_rss_info profile_rss;
  profile_extent_size_info profile_extent_size;
  profile_allocs_info profile_allocs;
  profile_online_info profile_online;
} profile_info;

typedef struct profile_thread {
  pthread_t id;
  int signal, skip_signal;
  unsigned long skip_intervals; /* Number of intervals we should skip */
  unsigned long skipped_intervals; /* Number of intervals we have skipped */
} profile_thread;

typedef struct profiler {
  /* For the master thread */
  pthread_t master_id;
  timer_t timerid;

  /* One for each profiling thread */
  profile_thread *profile_threads;
  size_t num_profile_threads;

  /* Sync the threads */
  size_t cur_interval;
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  char threads_finished;

  /* For the main application thread to
   * signal the master to stop
   */
  int stop_signal, master_signal;

  /* Per-arena profiling information */
  profile_info **info;
  pthread_rwlock_t info_lock;

  /* Data for each profile thread */
  profile_all_data profile_all;
  profile_rss_data profile_rss;
  profile_extent_size_data profile_extent_size;
  profile_allocs_data profile_allocs;
  profile_online_data profile_online;
} profiler;

extern profiler prof;

void sh_start_profile_master_thread();
void sh_stop_profile_master_thread();

void end_interval();

void *create_profile_arena(int);
