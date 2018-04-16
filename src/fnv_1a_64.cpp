// 64 bit Fowler/Noll/Vo-0 FNV-1a hash

#include "pch.h"
#include "fnv_1a_64.h"

using namespace std;
using namespace boost;
namespace fs = boost::filesystem;

// 64 bit magic FNV-1a prime
const u64 FNV_64_PRIME(0x100000001b3ULL);
const u64 FNV1A_64_INIT(0xcbf29ce484222325ULL);

const streamsize buf_size(1024 * 1024);

u64 fnv_1a_64_buf(void* buf, size_t len, u64 hash)
{
  // start of buffer
  u8* bp = (u8*)buf;
  // beyond end of buffer
  u8* be = bp + len;

  while (bp < be) {
    // xor the bottom with the current octet
    hash ^= (u64)*bp++;
    // multiply by the 64 bit FNV magic prime mod 2^64
    hash *= FNV_64_PRIME;
  }

  return hash;
}

u64 fnv_1a_64(const fs::wpath& path)
{
  u64 hash(FNV1A_64_INIT);

  boost::filesystem::ifstream ifs;
  ifs.exceptions(ifstream::badbit);
  ifs.open(path, ifstream::binary);

  if (!ifs.is_open()) {
    throw wstring(L"Couldn't open file");
  }

  scoped_array<u8> buf(new u8[buf_size]);

  do {
    ifs.read(reinterpret_cast<char*>(buf.get()), buf_size);
    hash = fnv_1a_64_buf(buf.get(), ifs.gcount(), hash);
  } while (ifs);

  return hash;
}
