// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#include <revobase/HashUtils.h>

#include <revobase/MurMurHash3.h>

namespace revobase {

uint32_t HashUtils::hash32(const unsigned char *key, int32_t length,
                           uint32_t seed) {
  uint32_t h;
  MurmurHash3_x86_32(key, length, seed, &h);
  return h;
}

} // namespace revobase
