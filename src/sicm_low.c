#include "sicm_low.h"

#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define X86_CPUID_MODEL_MASK        (0xf<<4)
#define X86_CPUID_EXT_MODEL_MASK    (0xf<<16)

struct sicm_device_list sicm_init() {
  int node_count = numa_max_node() + 1;
  int device_count = node_count;
  struct sicm_device* devices = malloc(device_count * sizeof(struct sicm_device));
  
  int i, j;
  int idx = 0;
  
  struct bitmask* cpumask = numa_allocate_cpumask();
  int cpu_count = numa_num_possible_cpus();
  struct bitmask* compute_nodes = numa_bitmask_alloc(node_count);
  for(i = 0; i < node_count; i++) {
    numa_node_to_cpus(i, cpumask);
    for(j = 0; j < cpu_count; j++) {
      if(numa_bitmask_isbitset(cpumask, j)) {
        numa_bitmask_setbit(compute_nodes, i);
        break;
      }
    }
  }
  numa_free_cpumask(cpumask);
  
  struct bitmask* non_dram_nodes = numa_bitmask_alloc(node_count);
  
  // Knights Landing
  uint32_t xeon_phi_model = (0x7<<4);
  uint32_t xeon_phi_ext_model = (0x5<<16);
  uint32_t registers[4];
  uint32_t expected = xeon_phi_model | xeon_phi_ext_model;
  asm volatile("cpuid":"=a"(registers[0]),
                         "=b"(registers[1]),
                         "=c"(registers[2]),
                         "=d"(registers[2]):"0"(1), "2"(0));
  uint32_t actual = registers[0] & (X86_CPUID_MODEL_MASK | X86_CPUID_EXT_MODEL_MASK);

  if (actual == expected) {
    for(i = 0; i <= numa_max_node(); i++) {
      if(!numa_bitmask_isbitset(compute_nodes, i)) {
        devices[idx].tag = SICM_KNL_HBM;
        devices[idx].data.knl_hbm = i;
        numa_bitmask_setbit(non_dram_nodes, i);
        idx++;
      }
    }
  }
  
  // DRAM
  for(i = 0; i <= numa_max_node(); i++) {
    if(!numa_bitmask_isbitset(non_dram_nodes, i)) {
      devices[idx].tag = SICM_DRAM;
      devices[idx].data.dram = i;
      idx++;
    }
  }
  
  numa_bitmask_free(compute_nodes);
  numa_bitmask_free(non_dram_nodes);
  
  return (struct sicm_device_list){ .count = device_count, .devices = devices };
}

void* sicm_alloc(struct sicm_device* device, size_t size) {
  switch(device->tag) {
    case SICM_DRAM:
      return numa_alloc_onnode(size, device->data.dram);
    case SICM_KNL_HBM:
      return numa_alloc_onnode(size, device->data.knl_hbm);
  }
  printf("error in sicm_alloc: unknown tag\n");
  exit(-1);
}

void sicm_free(struct sicm_device* device, void* ptr, size_t size) {
  switch(device->tag) {
    case SICM_DRAM:
    case SICM_KNL_HBM:
      numa_free(ptr, size);
      break;
    default:
      printf("error in sicm_free: unknown tag\n");
      exit(-1);
  }
}

int sicm_numa_id(struct sicm_device* device) {
  switch(device->tag) {
    case SICM_DRAM:
      return device->data.dram;
    case SICM_KNL_HBM:
      return device->data.knl_hbm;
    default:
      return -1;
  }
}

int sicm_move(struct sicm_device* src, struct sicm_device* dst, void* ptr, size_t size) {
  if(sicm_numa_id(src) >= 0) {
    int dst_node = sicm_numa_id(dst);
    if(dst_node >= 0) {
      nodemask_t nodemask;
      nodemask_zero(&nodemask);
      nodemask_set_compat(&nodemask, dst_node);
      return mbind(ptr, size, MPOL_BIND, nodemask.n, numa_max_node() + 2, MPOL_MF_MOVE);
    }
  }
  return -1;
}

size_t sicm_capacity(struct sicm_device* device) {
  switch(device->tag) {
    case SICM_DRAM:
    case SICM_KNL_HBM:; // labels can't be followed by declarations
      int node = sicm_numa_id(device);
      char path[50];
      sprintf(path, "/sys/devices/system/node/node%d/meminfo", node);
      int fd = open(path, O_RDONLY);
      char data[31];
      read(fd, data, 31);
      close(fd);
      size_t res = 0;
      size_t factor = 1;
      int i;
      for(i = 30; data[i] != ' '; i--) {
        res += factor * (data[i] - '0');
        factor *= 10;
      }
      return res;
    default:
      return -1;
  }
}

size_t sicm_used(struct sicm_device* device) {
  switch(device->tag) {
    case SICM_DRAM:
    case SICM_KNL_HBM:; // labels can't be followed by declarations
      int node = sicm_numa_id(device);
      char path[50];
      sprintf(path, "/sys/devices/system/node/node%d/meminfo", node);
      int fd = open(path, O_RDONLY);
      char data[101];
      read(fd, data, 101);
      close(fd);
      size_t res = 0;
      size_t factor = 1;
      int i;
      for(i = 100; data[i] != ' '; i--) {
        res += factor * (data[i] - '0');
        factor *= 10;
      }
      return res;
    default:
      return -1;
  }
}

#ifdef _GNU_SOURCE
int sicm_model_distance(struct sicm_device* device) {
  switch(device->tag) {
    case SICM_DRAM:
    case SICM_KNL_HBM:; // labels can't be followed by declarations
      int node = sicm_numa_id(device);
      return numa_distance(node, numa_node_of_cpu(sched_getcpu()));
    default:
      return -1;
  }
}
#endif

void sicm_latency(struct sicm_device* device, size_t size, int iter, struct sicm_timing* res) {
  struct timespec start, end;
  int i;
  char b = 0;
  unsigned int n = time(NULL);
  clock_gettime(CLOCK_MONOTONIC_RAW, &start);
  char* blob = sicm_alloc(device, size);
  clock_gettime(CLOCK_MONOTONIC_RAW, &end);
  res->alloc = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
  clock_gettime(CLOCK_MONOTONIC_RAW, &start);
  for(i = 0; i < iter; i++) {
    sicm_rand(n);
    blob[n % size] = 0;
  }
  clock_gettime(CLOCK_MONOTONIC_RAW, &end);
  res->write = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
  clock_gettime(CLOCK_MONOTONIC_RAW, &start);
  for(i = 0; i < iter; i++) {
    sicm_rand(n);
    b = blob[n % size];
  }
  clock_gettime(CLOCK_MONOTONIC_RAW, &end);
  // Write it back so hopefully it won't compile away the read
  blob[0] = b;
  res->read = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
  clock_gettime(CLOCK_MONOTONIC_RAW, &start);
  sicm_free(device, blob, size);
  clock_gettime(CLOCK_MONOTONIC_RAW, &end);
  res->free = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
}

union size_bytes {
  size_t i;
  char b[sizeof(size_t)];
};

size_t sicm_bandwidth_linear2(struct sicm_device* device, size_t size,
    size_t (*kernel)(double*, double*, size_t)) {
  struct timespec start, end;
  double* a = sicm_alloc(device, size * sizeof(double));
  double* b = sicm_alloc(device, size * sizeof(double));
  unsigned int i;
  #pragma omp parallel for
  for(i = 0; i < size; i++) {
    a[i] = 1;
    b[i] = 2;
  }
  clock_gettime(CLOCK_MONOTONIC_RAW, &start);
  size_t accesses = kernel(a, b, size);
  clock_gettime(CLOCK_MONOTONIC_RAW, &end);
  size_t delta = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
  sicm_free(device, a, size * sizeof(double));
  sicm_free(device, b, size * sizeof(double));
  return accesses / delta;
}

size_t sicm_bandwidth_random2(struct sicm_device* device, size_t size,
    size_t (*kernel)(double*, double*, size_t*, size_t)) {
  struct timespec start, end;
  double* a = sicm_alloc(device, size * sizeof(double));
  double* b = sicm_alloc(device, size * sizeof(double));
  size_t* indexes = sicm_alloc(device, size * sizeof(size_t));
  unsigned int i, j;
  union size_bytes bytes;
  #pragma omp parallel for
  for(i = 0; i < size; i++) {
    a[i] = 1;
    b[i] = 2;
    bytes.i = i;
    indexes[i] = 0xcbf29ce484222325;
    for(j = 0; j < sizeof(size_t); j++) {
      indexes[i] ^= bytes.b[j];
      indexes[i] *= 0x100000001b3;
    }
    indexes[i] = indexes[i] % size;
  }
  clock_gettime(CLOCK_MONOTONIC_RAW, &start);
  size_t accesses = kernel(a, b, indexes, size);
  clock_gettime(CLOCK_MONOTONIC_RAW, &end);
  size_t delta = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
  sicm_free(device, a, size * sizeof(double));
  sicm_free(device, b, size * sizeof(double));
  sicm_free(device, indexes, size * sizeof(size_t));
  return accesses / delta;
}

size_t sicm_bandwidth_linear3(struct sicm_device* device, size_t size,
    size_t (*kernel)(double*, double*, double*, size_t)) {
  struct timespec start, end;
  double* a = sicm_alloc(device, size * sizeof(double));
  double* b = sicm_alloc(device, size * sizeof(double));
  double* c = sicm_alloc(device, size * sizeof(double));
  unsigned int i;
  #pragma omp parallel for
  for(i = 0; i < size; i++) {
    a[i] = 1;
    b[i] = 2;
    c[i] = 3;
  }
  clock_gettime(CLOCK_MONOTONIC_RAW, &start);
  size_t accesses = kernel(a, b, c, size);
  clock_gettime(CLOCK_MONOTONIC_RAW, &end);
  size_t delta = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
  sicm_free(device, a, size * sizeof(double));
  sicm_free(device, b, size * sizeof(double));
  sicm_free(device, c, size * sizeof(double));
  return accesses / delta;
}

size_t sicm_bandwidth_random3(struct sicm_device* device, size_t size,
    size_t (*kernel)(double*, double*, double*, size_t*, size_t)) {
  struct timespec start, end;
  double* a = sicm_alloc(device, size * sizeof(double));
  double* b = sicm_alloc(device, size * sizeof(double));
  double* c = sicm_alloc(device, size * sizeof(double));
  size_t* indexes = sicm_alloc(device, size * sizeof(size_t));
  unsigned int i, j;
  union size_bytes bytes;
  #pragma omp parallel for
  for(i = 0; i < size; i++) {
    a[i] = 1;
    b[i] = 2;
    c[i] = 3;
    bytes.i = i;
    indexes[i] = 0xcbf29ce484222325;
    for(j = 0; j < sizeof(size_t); j++) {
      indexes[i] ^= bytes.b[j];
      indexes[i] *= 0x100000001b3;
    }
    indexes[i] = indexes[i] % size;
  }
  clock_gettime(CLOCK_MONOTONIC_RAW, &start);
  size_t accesses = kernel(a, b, c, indexes, size);
  clock_gettime(CLOCK_MONOTONIC_RAW, &end);
  size_t delta = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
  sicm_free(device, a, size * sizeof(double));
  sicm_free(device, b, size * sizeof(double));
  sicm_free(device, c, size * sizeof(double));
  sicm_free(device, indexes, size * sizeof(size_t));
  return accesses / delta;
}

size_t sicm_triad_kernel_linear(double* a, double* b, double* c, size_t size) {
  int i;
  double scalar = 3.0;
  #pragma omp parallel for
  for(i = 0; i < size; i++) {
    a[i] = b[i] + scalar * c[i];
  }
  return size * 3 * sizeof(double);
}

size_t sicm_triad_kernel_random(double* a, double* b, double* c, size_t* indexes, size_t size) {
  int i, idx;
  double scalar = 3.0;
  #pragma omp parallel for
  for(i = 0; i < size; i++) {
    idx = indexes[i];
    a[idx] = b[idx] + scalar * c[idx];
  }
  return size * (sizeof(size_t) + 3 * sizeof(double));
}

int main() {
  struct sicm_device_list devices = sicm_init();
  
  int* blob = sicm_alloc(&devices.devices[0], 100 * sizeof(int));
  int i;
  for(i = 0; i < 100; i++) blob[i] = i;
  for(i = 0; i < 100; i++) printf("%d ", blob[i]);
  printf("\n");
  sicm_free(&devices.devices[0], blob, 100 * sizeof(int));
  
  return 1;
}
