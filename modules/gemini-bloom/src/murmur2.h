#pragma once

#include <cstddef>
#include <cstdint>

uint32_t MurmurHash2(const void* key, size_t len, uint32_t seed);
uint64_t MurmurHash64A(const void* key, size_t len, uint64_t seed);
