#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <signal.h>

#define SICM_RUNTIME 1
#include "sicm_profile.h"
#include "sicm_parsing.h"

#include "proc_object_map.h"

profiler prof;
static pthread_rwlock_t profile_lock = PTHREAD_RWLOCK_INITIALIZER;
static int global_signal;
static char should_stop = 0;

void print_proc_swaps(FILE * outfile)
{
    FILE * fptr;

    fptr = fopen(PROC_SWAPS, "r");
    char buff[100];
    char * token;
    int counter = 0;

    //Skip line
    fgets(buff, 100, fptr);

    while(fgets(buff, 100, fptr) != NULL)
    {
        token = strtok(buff, " \t");
        fprintf(outfile, " %s: ", token);
        while(token != NULL)
        {
            if(counter >= 3)
            {
                fprintf(outfile, "%12llu", atoi(token));
                break;
            }
            token = strtok(NULL, " \t");
            counter++;
        }
        counter = 0;
    }

    fclose(fptr);
}

unsigned long long get_stat(const char *fname, int pos)
{
  FILE *fptr;
  int cnt;
  unsigned long long tmp, stat;

  fptr = fopen(fname, "r");
  if (!fptr) {
    fprintf(stderr, "Error opening %s\n", fname);
    perror("");
    exit(errno);
  }

  while (cnt < pos) {
    if ( (fscanf(fptr, "%llu", &stat)) != 1 ) {
      fprintf(stderr, "Error reading %s\n", fname);
      exit(-EINVAL);
    }
    cnt++;
  }
  fclose(fptr);

  return stat;
}

void print_compress_stats(FILE *compressf)
{
  unsigned long long rss, cmp, d_rss, p_rss, same_filled, page_count;

  rss   = (get_stat(PROC_SELF_STATM, 2) * PAGE_SIZE);
  cmp   = (get_stat(ZSWAP_STORED_PAGES, 1) * PAGE_SIZE);
  d_rss = get_stat(CGROUP_MEM_CURRENT, 1);
  p_rss = get_stat(ZSWAP_POOL_TOTAL_SIZE, 1);
  same_filled = get_stat(ZSWAP_SAME_FILLED_PAGES, 1);
  page_count = get_stat(ZSWAP_STORED_PAGES, 1);

  
  fprintf(compressf, "rss: %12llu cmp: %12llu d_rss: %12llu p_rss: %12llu s_filled_p: %12llu page_count: %12llu",
          rss, cmp, d_rss, p_rss, same_filled, page_count);

  print_proc_swaps(compressf);

  fprintf(compressf, "\n");
  
}

void update_cmem_limit()
{
  unsigned long long cmp;
  cmp   = (get_stat(ZSWAP_STORED_PAGES, 1) * PAGE_SIZE);
  cmp = cmp * profopts.compress_limit_ratio;
  fprintf(stderr, "New limit is: %lld\n", cmp);
  cgroup_set_mem_high(cmp);
  cgroup_set_mem_max(cmp);
}


/* Runs when an arena has already been created, but the runtime library
   has added an allocation site to the arena. */
void add_site_profile(int index, int site_id) {
  arena_profile *aprof;
  int err;
  
  err = pthread_rwlock_wrlock(&profile_lock);
  if(err) {
    fprintf(stderr, "add_site_profile failed to grab the profile_lock. Aborting.\n");
    exit(1);
  }
  if((index < 0) || (index > tracker.max_arenas)) {
    fprintf(stderr, "Can't add a site to an index (%d) larger than the maximum index (%d).\n", index, tracker.max_arenas);
    exit(1);
  }
  aprof = get_arena_prof(index);
  if(!aprof) {
    fprintf(stderr, "Tried to add a site to index %d without having created an arena profile there. Aborting.\n", index);
    exit(1);
  }
  if(aprof->num_alloc_sites + 1 > tracker.max_sites_per_arena) {
    fprintf(stderr, "The maximum number of sites per arena has been reached: %d\n", tracker.max_sites_per_arena);
    exit(1);
  }
  aprof->num_alloc_sites++;
  aprof->alloc_sites[aprof->num_alloc_sites - 1] = site_id;
  pthread_rwlock_unlock(&profile_lock);
}

/* Runs when a new arena is created. Allocates room to store
   profiling information about this arena. */
void create_arena_profile(int index, int site_id, char invalid) {
  arena_profile *aprof;
  int err, i;

  err = pthread_rwlock_wrlock(&profile_lock);
  if(err) {
    fprintf(stderr, "create_arena_profile failed to grab the profile_lock. Aborting.\n");
    exit(1);
  }
  
  if((index < 0) || (index > tracker.max_arenas)) {
    fprintf(stderr, "Can't add a site to an index (%d) larger than the maximum index (%d).\n", index, tracker.max_arenas);
    exit(1);
  }
  
  aprof = internal_valloc(sizeof(arena_profile));
  memset(aprof, 0, sizeof(arena_profile));

  if(should_profile_pebs()) {
    profile_pebs_arena_init(&(aprof->profile_pebs));
  }
  if(should_profile_rss()) {
    profile_rss_arena_init(&(aprof->profile_rss));
  }
  if(should_profile_extent_size()) {
    profile_extent_size_arena_init(&(aprof->profile_extent_size));
  }
  if(should_profile_allocs()) {
    profile_allocs_arena_init(&(aprof->profile_allocs));
  }
  if(should_profile_objmap()) {
    profile_objmap_arena_init(&(aprof->profile_objmap));
  }
  if(should_profile_online()) {
    profile_online_arena_init(&(aprof->profile_online));
  }
  if(should_profile_bw()) {
    profile_bw_arena_init(&(aprof->profile_bw));
  }

  /* Creates a profile for this arena at the current interval */
  aprof->index = index;
  aprof->invalid = invalid;
  aprof->num_alloc_sites = 1;
  aprof->alloc_sites = internal_malloc(sizeof(int) * tracker.max_sites_per_arena);
  aprof->alloc_sites[0] = site_id;
  prof.cur_interval.num_arenas++;
  if(index > prof.cur_interval.max_index) {
    prof.cur_interval.max_index = index;
  }
  prof.cur_interval.arenas[index] = aprof;
  
  pthread_rwlock_unlock(&profile_lock);
}

/* Runs when a new extent is created. */
void create_extent_objmap_entry(void *start, void *end) {
  int err;
  
  err = objmap_add_range(&prof.profile_objmap.objmap, start, end);
  if (err < 0) {
    fprintf(stderr, "Couldn't add extent (start=%p, end=%p) to object_map (error = %d). Aborting.\n", start, end, err);
    exit(1);
  }
}

/* Runs when a new extent is deleted. */
void delete_extent_objmap_entry(void *start) {
  int err;
  
  err = objmap_del_range(&prof.profile_objmap.objmap, start);
  if ((err < 0) && (err != -22)) {
    fprintf(stderr, "WARNING: Couldn't delete extent (%p) to object_map (error = %d).\n", start, err);
  }
}

/* This is the signal handler for the Master thread, so
 * it does this on every interval.
 */
void profile_master_interval(int s) {
  struct timespec actual;
  size_t i, n, x;
  char copy;
  double elapsed_time;
  int err;
  sigset_t mask;

  /* Convenience pointers */
  arena_profile *aprof;
  arena_info *arena;
  profile_thread *profthread;

  /* this is really only used for online runs */
  if (should_profile_compress_stats()) {
    print_compress_stats(profopts.compress_stats_file);
    if(profopts.compress_limit_ratio != 0)
    {
        //fprintf(stderr, "Updating cmem\n");
        update_cmem_limit();
    }

    return;
  }
  
  /* Block the signal for a bit */
  sigemptyset(&mask);
  sigaddset(&mask, prof.master_signal);
  if(sigprocmask(SIG_SETMASK, &mask, NULL) == -1) {
    fprintf(stderr, "Error blocking signal. Aborting.\n");
    exit(1);
  }
  
  /* Here, we're checking to see if the time between this interval and
     the previous one is too short. If it is, this is likely a queued-up
     signal caused by an interval that took too long. In some cases,
     profiling threads can take up to 10 seconds to complete, and in that
     span of time, hundreds or thousands of timer signals could have been
     queued up. We want to prevent that from happening, so ignore a signal
     that occurs quicker than it should. */
  clock_gettime(CLOCK_MONOTONIC, &(prof.start));
  timespec_diff(&(prof.end), &(prof.start), &actual);
  elapsed_time = actual.tv_sec + (((double) actual.tv_nsec) / 1000000000);
  if(elapsed_time < (prof.target - (prof.target * 10 / 100))) {
    /* It's too soon since the last interval. */
    clock_gettime(CLOCK_MONOTONIC, &(prof.end));
    /* Unblock the signal */
    if(sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1) {
      fprintf(stderr, "Error unblocking signal. Aborting.\n");
      exit(1);
    }
    return;
  }

  err = pthread_rwlock_wrlock(&profile_lock);
  if(err) {
    fprintf(stderr, "WARNING: Failed to acquire the profiling lock in an interval: %d\n", err);
    /* Unblock the signal */
    if(sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1) {
      fprintf(stderr, "Error unblocking signal. Aborting.\n");
      exit(1);
    }
    pthread_rwlock_unlock(&profile_lock);
    return;
  }
  
  /* Call the interval functions for each of the profiling types */
  for(i = 0; i < prof.num_profile_threads; i++) {
    profthread = &prof.profile_threads[i];
    if(profthread->skipped_intervals == (profthread->skip_intervals - 1)) {
      /* This thread doesn't get skipped */
      (*profthread->interval_func)(0);
      profthread->skipped_intervals = 0;
    } else {
      /* This thread gets skipped */
      (*profthread->skip_interval_func)(0);
      profthread->skipped_intervals++;
    }
  }

  aprof_arr_for(i, aprof) {
    aprof_check_good(i, aprof);

    if(should_profile_pebs()) {
      profile_pebs_post_interval(aprof);
    }
    if(should_profile_rss()) {
      profile_rss_post_interval(aprof);
    }
    if(should_profile_extent_size()) {
      profile_extent_size_post_interval(aprof);
    }
    if(should_profile_allocs()) {
      profile_allocs_post_interval(aprof);
    }
    if(should_profile_objmap()) {
      profile_objmap_post_interval(aprof);
    }
    if(should_profile_online()) {
      profile_online_post_interval(aprof);
    }
  }
  if(should_profile_bw()) {
    profile_bw_post_interval();
  }
  if(should_profile_latency()) {
    profile_latency_post_interval();
  }
  
  /* Store the time that this interval took */
  clock_gettime(CLOCK_MONOTONIC, &(prof.end));
  timespec_diff(&(prof.start), &(prof.end), &actual);
  prof.cur_interval.time = actual.tv_sec + (((double) actual.tv_nsec) / 1000000000);
   
  if(profopts.profile_output_file) {
    sh_print_interval_profile(&prof.cur_interval, prof.profile, prof.profile->num_intervals, profopts.profile_output_file);
    fflush(profopts.profile_output_file);
  }
  prof.profile->num_intervals++;
  copy_interval_profile(&prof.prev_interval, &prof.cur_interval);
  
  /* We need this timer to actually end outside out of the lock */
  clock_gettime(CLOCK_MONOTONIC, &(prof.end));
  
  /* Unblock the signal */
  if(sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1) {
    fprintf(stderr, "Error unblocking signal. Aborting.\n");
    exit(1);
  }
  
  pthread_rwlock_unlock(&profile_lock);
}

/* Stops the master thread */
void profile_master_stop(int s) {
  timer_delete(prof.timerid);
  should_stop = 1;
  //pthread_exit(NULL);
}

void setup_profile_thread(void *(*main)(void *), /* Spinning loop function */
                          void (*interval)(int), /* Per-interval function */
                          void (*skip_interval)(int), /* Per-interval skip function */
                          unsigned long skip_intervals) {
  struct sigaction sa;
  profile_thread *profthread;

  /* Add a new profile_thread struct for it */
  prof.num_profile_threads++;
  prof.profile_threads = internal_realloc(prof.profile_threads, sizeof(profile_thread) * prof.num_profile_threads);
  profthread = &(prof.profile_threads[prof.num_profile_threads - 1]);

  profthread->interval_func      = interval;
  profthread->skip_interval_func = skip_interval;
  profthread->skipped_intervals  = 0;
  profthread->skip_intervals     = skip_intervals;
}

/* This is the Master thread, it keeps track of intervals
 * and starts/stops the profiling threads. It has a timer
 * which signals it at a certain interval. Each time this
 * happens, it notifies the profiling threads.
 */
void *profile_master(void *a) {
  struct sigevent sev;
  struct sigaction sa;
  struct itimerspec its;
  long long frequency;
  sigset_t mask;
  pid_t tid;
  
  /* NOTE: This order is important for profiling types that depend on others.
     For example, if a profiling type depends on the bandwidth values, 
     make sure that its `setup_profile_thread` is called *before* the bandwidth
     profiler. This also means that, if you use the SH_PROFILE_SEPARATE_THREADS feature,
     you must add mutices to ensure that one type has finished before another starts. */
  
  if(should_profile_latency()) {
    setup_profile_thread(&profile_latency,
                         &profile_latency_interval,
                         &profile_latency_skip_interval,
                         profopts.profile_latency_skip_intervals);
  }
  if(should_profile_pebs()) {
    setup_profile_thread(&profile_pebs,
                         &profile_pebs_interval,
                         &profile_pebs_skip_interval,
                         profopts.profile_pebs_skip_intervals);
  }
  if(should_profile_rss()) {
    setup_profile_thread(&profile_rss,
                         &profile_rss_interval,
                         &profile_rss_skip_interval,
                         profopts.profile_rss_skip_intervals);
  }
  if(should_profile_bw()) {
    setup_profile_thread(&profile_bw,
                         &profile_bw_interval,
                         &profile_bw_skip_interval,
                         profopts.profile_bw_skip_intervals);
  }
  if(should_profile_extent_size()) {
    setup_profile_thread(&profile_extent_size,
                         &profile_extent_size_interval,
                         &profile_extent_size_skip_interval,
                         profopts.profile_extent_size_skip_intervals);
  }
  if(should_profile_allocs()) {
    setup_profile_thread(&profile_allocs,
                         &profile_allocs_interval,
                         &profile_allocs_skip_interval,
                         profopts.profile_allocs_skip_intervals);
  }
  if(should_profile_objmap()) {
    setup_profile_thread(&profile_objmap,
                         &profile_objmap_interval,
                         &profile_objmap_skip_interval,
                         profopts.profile_objmap_skip_intervals);
  }
  if(should_profile_online()) {
    setup_profile_thread(&profile_online,
                         &profile_online_interval,
                         &profile_online_skip_interval,
                         profopts.profile_online_skip_intervals);
  }

  /* Initialize synchronization primitives */
  pthread_mutex_init(&prof.mtx, NULL);
  pthread_cond_init(&prof.cond, NULL);

  /* Set up a signal handler for the master */
  sa.sa_flags = 0;
  sa.sa_handler = profile_master_interval;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, prof.stop_signal); /* Stop signal should block until an interval is finished */
  if(sigaction(prof.master_signal, &sa, NULL) == -1) {
    fprintf(stderr, "Error creating signal handler. Aborting.\n");
    exit(1);
  }

  /* Block the signal for a bit */
  sigemptyset(&mask);
  sigaddset(&mask, prof.master_signal);
  if(sigprocmask(SIG_SETMASK, &mask, NULL) == -1) {
    fprintf(stderr, "Error blocking signal. Aborting.\n");
    exit(1);
  }

  /* Create the timer */
  tid = syscall(SYS_gettid);
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = prof.master_signal;
  sev.sigev_value.sival_ptr = &prof.timerid;
  sev._sigev_un._tid = tid;
  if(timer_create(CLOCK_REALTIME, &sev, &prof.timerid) == -1) {
    fprintf(stderr, "Error creating timer. Aborting.\n");
    exit(1);
  }
  
  /* Store how long the interval should take */
  prof.target = ((double) profopts.profile_rate_nseconds) / 1000000000;

  /* Set the timer */
  its.it_value.tv_sec     = profopts.profile_rate_nseconds / 1000000000;
  its.it_value.tv_nsec    = profopts.profile_rate_nseconds % 1000000000;
  its.it_interval.tv_sec  = its.it_value.tv_sec;
  its.it_interval.tv_nsec = its.it_value.tv_nsec;
  if(timer_settime(prof.timerid, 0, &its, NULL) == -1) {
    fprintf(stderr, "Error setting the timer. Aborting.\n");
    exit(1);
  }
  
  /* Initialize this time */
  clock_gettime(CLOCK_MONOTONIC, &(prof.end));
  
  /* Unblock the signal */
  if(sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1) {
    fprintf(stderr, "Error unblocking signal. Aborting.\n");
    exit(1);
  }

  /* Wait for either the timer to signal us to start a new interval,
   * or for the main thread to signal us to stop.
   */
  while(!should_stop) {}
}

void init_application_profile(application_profile *profile) {
  prof.profile->num_intervals = 0;
  prof.profile->intervals = NULL;
  
  /* Set flags for what type of profiling we'll store */
  if(should_profile_pebs()) {
    prof.profile->has_profile_pebs = 1;
  }
  if(should_profile_rss()) {
    prof.profile->has_profile_rss = 1;
  }
  if(should_profile_allocs()) {
    prof.profile->has_profile_allocs = 1;
  }
  if(should_profile_extent_size()) {
    prof.profile->has_profile_extent_size = 1;
  }
  if(should_profile_objmap()) {
    prof.profile->has_profile_objmap = 1;
  }
  if(should_profile_online()) {
    prof.profile->has_profile_online = 1;
  }
  if(should_profile_bw()) {
    prof.profile->has_profile_bw = 1;
  }
  if(should_profile_latency()) {
    prof.profile->has_profile_latency = 1;
  }
  if(should_profile_pebs() && should_profile_bw()) {
    prof.profile->has_profile_bw_relative = 1;
  }
}

void initialize_profiling() {
  size_t i;
  int err;
  
  err = pthread_rwlock_wrlock(&profile_lock);
  if(err) {
    fprintf(stderr, "initialize_profiling failed to grab the profile_lock. Aborting.\n");
    exit(1);
  }

  /* Initialize the structs that store the profiling information */
  prof.profile = internal_calloc(1, sizeof(application_profile));
  init_application_profile(prof.profile);

  /* Stores the current interval's profiling */
  prof.cur_interval.num_arenas = 0;
  prof.cur_interval.max_index = 0;
  prof.cur_interval.arenas = internal_calloc(tracker.max_arenas, sizeof(arena_profile *));

  /* Store the profile_pebs event strings */
  prof.profile->num_profile_pebs_events = profopts.num_profile_pebs_events;
  prof.profile->profile_pebs_events = internal_calloc(prof.profile->num_profile_pebs_events, sizeof(char *));
  for(i = 0; i < profopts.num_profile_pebs_events; i++) {
    prof.profile->profile_pebs_events[i] = internal_malloc((strlen(profopts.profile_pebs_events[i]) + 1) * sizeof(char));
    strcpy(prof.profile->profile_pebs_events[i], profopts.profile_pebs_events[i]);
  }
  
  #if 0
  /* Store which sockets we profiled */
  prof.profile->num_profile_skts = profopts.num_profile_skt_cpus;
  prof.profile->profile_skts = internal_calloc(prof.profile->num_profile_skts, sizeof(int));
  for(i = 0; i < prof.profile->num_profile_skts; i++) {
    prof.profile->profile_skts[i] = profopts.profile_skts[i];
  }
  #endif
  
  /* The signal that will stop the master thread */
  global_signal = SIGRTMIN;
  prof.stop_signal = global_signal;
  global_signal++;

  /* The signal that the master thread will use to tell itself
   * (via a timer) when the next interval should start */
  prof.master_signal = global_signal;
  global_signal++;
  
  /* All of this initialization HAS to happen in the main SICM thread.
   * If it's not, the `perf_event_open` system call won't profile
   * the current thread, but instead will only profile the thread that
   * it was run in.
   */
  if(should_profile_pebs()) {
    profile_pebs_init();
  }
  if(should_profile_rss()) {
    profile_rss_init();
  }
  if(should_profile_bw()) {
    profile_bw_init();
  }
  if(should_profile_latency()) {
    profile_latency_init();
  }
  if(should_profile_extent_size()) {
    profile_extent_size_init();
  }
  if(should_profile_allocs()) {
    profile_allocs_init();
  }
  if(should_profile_objmap()) {
    profile_objmap_init();
  }
  if(should_profile_online()) {
    profile_online_init();
  }
  
  pthread_rwlock_unlock(&profile_lock);
}

void sh_start_profile_master_thread() {
  struct sigaction sa;
  
  /* This initializes the values that the threads will need to do their profiling,
   * including perf events, file descriptors, etc.
   */
  initialize_profiling();
  
  /* Set up the signal that we'll use to stop the master thread */
  sa.sa_flags = 0;
  sa.sa_handler = profile_master_stop;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, prof.master_signal); /* Block the interval signal while the stop signal handler is running */
  if(sigaction(prof.stop_signal, &sa, NULL) == -1) {
    fprintf(stderr, "Error creating master stop signal handler. Aborting.\n");
    exit(1);
  }

  /* Start the master thread */
  pthread_create(&prof.master_id, NULL, &profile_master, NULL);
}

void deinitialize_profiling() {
  if(should_profile_pebs()) {
    profile_pebs_deinit();
  }
  if(should_profile_rss()) {
    profile_rss_deinit();
  }
  if(should_profile_bw()) {
    profile_bw_deinit();
  }
  if(should_profile_latency()) {
    profile_latency_deinit();
  }
  if(should_profile_extent_size()) {
    profile_extent_size_deinit();
  }
  if(should_profile_allocs()) {
    profile_allocs_deinit();
  }
  if(should_profile_objmap()) {
    profile_objmap_deinit();
  }
  if(should_profile_online()) {
    profile_online_deinit();
  }
}

void sh_stop_profile_master_thread() {
  /* Tell the master thread to stop */
  pthread_kill(prof.master_id, prof.stop_signal);
  pthread_join(prof.master_id, NULL);

  deinitialize_profiling();
}
