#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <inttypes.h>
#include <chrono>
#include <sstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "blake2s_ref.h"
#include "common.h"
#include "opencl_backend.hpp"

void usage() {
  fprintf(
    stderr,
    "  bigolchungus.sh [ -d <device id>         ]\n"
    "                  [ -p <platform id>       ]\n"
    "                  [ -l <local work size>   ]\n"
    "                  [ -w <work set size      ]\n"
    "                  [ -g <global work size>  ]\n"
    "                  [ -k <kernel location>   ]\n"
    "                  [ -n <hexadecimal nonce> ]\n"
    "                  [ -v                     ]\n"
    "                  <block>\n\n"
    "  1. Device Selection\n\n"
    "    -d <device id>\n"
    "      Default `0`\n\n"
    "    -p <platform id>\n"
    "      Default `0`\n\n"
    "    Run `clinfo -l` to get info about your device and platform ids.\n\n"
    "  2. Open CL work configuration \n\n"
    "    -l <local work size> \n"
    "      Default `256`.\n\n"
    "      If you are on AMD, `256` is probably the best value for you.\n"
    "      If you are on nVidia, you probably want `1024`.\n\n"
    "    -w <work set size> \n"
    "      Default `64`\n\n"
    "    -g <global work size>\n"
    "      Default `16777216` (1024 * 1024 * 16)\n\n"
    "    -k <kernel location>\n"
    "      If you are getting opencl error -46 or -30, try setting this to the absolute path of the `kernel.cl` file.\n"
    "      Defaults to ./kernels/kernel.cl\n"
    "      If -f is provided the default kernel is ./kernel/kernel2.cl.\n\n"
    "  3. Debugging\n\n"
    "    -v\n"
    "      enable verbose mode.\n\n"
    "  4. Advanced\n\n"
    "    -n <hexadecimal nonce>\n"
    "      Manually sets a nonce for hashing.\n"
    "      In the unlikely case that your mining host provides a nonce, use this.\n"
    "      If you are trying to get reproducible tests, use this.\n\n"
  );

}

void read_target_bytes(const char* str, uint8_t* target) {
    assert(strlen(str) == 64);
    for (size_t i = 0; i < 32; i++) {
        uint8_t c1 = hexchar2int(str[2 * i]);
        uint8_t c2 = hexchar2int(str[2 * i + 1]);
        target[i] = (c1 << 4) | c2;
    }
}

int main(int argc, char* const* argv) {
    // test_opencl <hash>

    if (argc == 1) {
      usage();
      exit(1);
    }


    //std::chrono::steady_clock::time_point t_start = std::chrono::high_resolution_clock::now();
    std::chrono::steady_clock::time_point t_start = std::chrono::steady_clock::now();

    bool quiet = true;
    int deviceOverride = 0;
    int platformOverride = -1;
    int localWorkSize = 256;
    int workSetSize = 64;
    int globalSize = 1024 * 1024 * 16;
    uint64_t nonceOverride;
    bool nonceOverridden = false;
    char* kernelPath = nullptr;

    int opt;
    while ((opt = getopt(argc, argv, "d:p:l:w:g:k:n:avh")) != -1) {
      switch(opt) {
        case 'd':
          deviceOverride = std::stoi(optarg);
          break;
        case 'p':
          platformOverride = std::stoi(optarg);
          break;
        case 'l':
          localWorkSize = std::stoi(optarg);
          break;
        case 'w':
          workSetSize = std::stoi(optarg);
          break;
        case 'g':
          globalSize = std::stoi(optarg);
          break;
        case 'k':
          kernelPath = optarg;
          break;
        case 'v':
          quiet = false;
          break;
        case 'n':
          nonceOverridden = true;
          nonceOverride = std::stoi(optarg, 0, 16);
          break;
        case 'h':
        case '?':
          usage();
          exit(1);
          break;
      }
    }


    uint8_t target_hash[32];
    read_target_bytes(argv[optind], target_hash);

    if (!quiet) fprintf(stderr, "Started\n");

    if (!quiet) {
        fprintf(stderr, "hash = ");
        for (int i = 0; i < 32; i++) {
            fprintf(stderr, "%#x,", target_hash[i]);
        }
        fprintf(stderr, "\n");
    }

    if (!quiet) fprintf(stderr, "Reading buf\n");
    const size_t BUF_SIZE = 4 * 1024;
    uint8_t buf[BUF_SIZE];
    size_t bufsize = fread(buf, 1, BUF_SIZE, stdin);
    assert(bufsize >= 8);
    assert(bufsize < BUF_SIZE);

    if (!quiet) {
        fprintf(stderr, "hash = ");
        for (int i = 0; i < bufsize; i++) {
            fprintf(stderr, "%#x,", buf[i]);
        }
        fprintf(stderr, "\n");
    }

    if (!quiet) fprintf(stderr, "bufsize = %lu\n", bufsize);

    assert(320 - 64 + 1 <= bufsize && bufsize <= 320);
    uint32_t last_block_size = bufsize - (320-64);
    memset(buf + 256 + last_block_size, 0, 64 - last_block_size);

    if (!quiet) fprintf(stderr, "last_block_size = %d\n", last_block_size);

    size_t global_size = globalSize;
    size_t local_size = localWorkSize;
    size_t workset_size = workSetSize;

    uint64_t nonce_step_size = global_size * workset_size;
    // uint8_t* result = new uint8_t[nonce_step_size * 64];

    uint64_t start_nonce = 0;
    if (nonceOverridden) {
      start_nonce = nonceOverride;
      if (!quiet) fprintf(stderr, "Using '0x%llX' as nonce.\n", start_nonce);
    } else {
      if (!quiet) fprintf(stderr, "Using /dev/urandom as nonce source\n");
      FILE* urandom = fopen("/dev/urandom","rb");
      fread(&start_nonce, 1, 8, urandom);
      fclose(urandom);
    }

    opencl_backend backend(nonce_step_size, quiet, deviceOverride, platformOverride, kernelPath);

    backend.start_search(
        global_size, local_size, workset_size,
        buf, target_hash);

    int steps = 0;
    while (true) {
        if (!quiet) fprintf(stderr,
            "Trying %#llx - %#llx\n", start_nonce, start_nonce + nonce_step_size - 1);
        steps += 1;
        uint64_t found = backend.continue_search(start_nonce);

        if (found != 0) {
            if (!quiet) fprintf(stderr, "Done %#llx!\n", found);

            uint8_t hash[32];
            blake2s_state state;
            blake2s_init(&state, BLAKE2S_OUTBYTES);
            blake2s_update(&state, buf, bufsize - 8);
            blake2s_update(&state, &found, 8);
            blake2s_final(&state, hash, BLAKE2S_OUTBYTES);

            if (compare_uint256(target_hash, hash) == -1) {
                fprintf(stderr, "Bad nonce!!!\n");
                exit(-1);
            }

            std::chrono::steady_clock::time_point t_end = std::chrono::steady_clock::now();

            float milliseconds = std::chrono::duration<float, std::milli>(t_end - t_start).count();
            uint64_t numHashes = steps * nonce_step_size;
            double rate = numHashes / (milliseconds / 1000.0);
            printf("%016" PRIx64 " %llu %llu", found, numHashes, (uint64_t) rate);
            break;
        } else {
            start_nonce += nonce_step_size;
        }
    }

    return 0;
}

