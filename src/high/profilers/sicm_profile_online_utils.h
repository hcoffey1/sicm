#include <time.h>
#include <sys/time.h>

/* Rebinds an arena to the device list.
   Times it and prints debugging information if necessary. */
void rebind_arena(int index, sicm_dev_ptr dl, tree_it(site_info_ptr, int) sit) {
  int retval;
  
  retval = sicm_arena_set_devices(tracker.arenas[index]->arena, dl);

  if(profopts.profile_online_debug_file) {
    if(retval == -EINVAL) {
      fprintf(profopts.profile_online_debug_file,
        "Rebinding arena %d failed in SICM.\n", index);
    } else if(retval != 0) {
      fprintf(profopts.profile_online_debug_file,
        "Rebinding arena %d failed internally.\n", index);
    }
  }
}

/* Rebinds all arenas according to the `dev` and `hot` parameters
   in the arena's `per_arena_profile_online_info` struct. */
void full_rebind(tree(site_info_ptr, int) sorted_sites) {
  sicm_dev_ptr dl;
  int index;
  char dev, hot;
  tree_it(site_info_ptr, int) sit;
  struct timespec start, end, diff;

  if(profopts.profile_online_debug_file) {
    clock_gettime(CLOCK_MONOTONIC, &start);
  }

  tree_traverse(sorted_sites, sit) {
    index = tree_it_key(sit)->index;
    dev = get_arena_online_prof(index)->dev;
    hot = get_arena_online_prof(index)->hot;

    dl = NULL;
    if((dev == 1) && (hot == 0)) {
      /* The site is in DRAM and isn't in the hotset */
      dl = prof.profile_online.lower_dl;
      get_arena_online_prof(index)->dev = 0;
      fprintf(profopts.profile_online_debug_file,
              "Binding down: %zu\n", index);
    }
    if(((dev == 0) || (dev == -1)) && !hot) {
      dl = prof.profile_online.lower_dl;
      get_arena_online_prof(index)->dev = 0;
      fprintf(profopts.profile_online_debug_file,
              "Binding down: %zu\n", index);
    }
    if(dl) {
      rebind_arena(index, dl, sit);
    }
  }
  
  tree_traverse(sorted_sites, sit) {
    index = tree_it_key(sit)->index;
    dev = get_arena_online_prof(index)->dev;
    hot = get_arena_online_prof(index)->hot;

    dl = NULL;
    if(((dev == -1) && hot) ||
        ((dev == 0) && hot)) {
      /* The site is in AEP, and is in the hotset. */
      dl = prof.profile_online.upper_dl;
      get_arena_online_prof(index)->dev = 1;
      fprintf(profopts.profile_online_debug_file,
              "Binding up: %zu\n", index);
    }
    if((dev == 1) && hot) {
      dl = prof.profile_online.upper_dl;
      get_arena_online_prof(index)->dev = 1;
      fprintf(profopts.profile_online_debug_file,
              "Binding up: %zu\n", index);
    }
    if(dl) {
      rebind_arena(index, dl, sit);
    }
  }
  
  if(profopts.profile_online_debug_file) {
    clock_gettime(CLOCK_MONOTONIC, &end);
    timespec_diff(&start, &end, &diff);
    if(profopts.profile_online_ski) {
      fprintf(profopts.profile_online_debug_file,
        "Full rebind estimate: %zu ms, real: %ld.%09ld s.\n",
        prof.profile_online.ski->penalty_move,
        diff.tv_sec, diff.tv_nsec);
      fflush(profopts.profile_online_debug_file);
    }
  }
}

void full_rebind_cold(tree(site_info_ptr, int) sorted_sites) {
  sicm_dev_ptr dl;
  int index;
  tree_it(site_info_ptr, int) sit;
  struct timespec start, end, diff;

  if(profopts.profile_online_debug_file) {
    clock_gettime(CLOCK_MONOTONIC, &start);
  }

  tree_traverse(sorted_sites, sit) {
    index = tree_it_key(sit)->index;

    dl = prof.profile_online.lower_dl;
    get_arena_online_prof(index)->dev = 0;
    rebind_arena(index, dl, sit);
  }
  
  if(profopts.profile_online_debug_file) {
    clock_gettime(CLOCK_MONOTONIC, &end);
    timespec_diff(&start, &end, &diff);
  }
}
