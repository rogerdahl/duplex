#include "pch.h"

/*
See http://www.boost.org for updates and documentation.

Copyright (C) 2002-2003 Stanislav Baranov. Permission to copy, use,
modify, sell and distribute this software and its documentation is
granted provided this copyright notice appears in all copies. This
software is provided "as is" without express or implied warranty,
and with no claim as to its suitability for any purpose. Derived
from the RSA Data Security, Inc. MD5 Message-Digest Algorithm.

Copyright (C) 1991-2, RSA Data Security, Inc. Created 1991. All rights
reserved. License to copy and use this software is granted provided that
it is identified as the "RSA Data Security, Inc. MD5 Message-Digest
Algorithm" in all material mentioning or referencing this software or
this function. License is also granted to make and use derivative works
provided that such works are identified as "derived from the RSA Data
Security, Inc. MD5 Message-Digest Algorithm" in all material mentioning
or referencing the derived work. RSA Data Security, Inc. makes no
representations concerning either the merchantability of this software
or the suitability of this software for any particular purpose. It is
provided "as is" without express or implied warranty of any kind. These
notices must be retained in any copies of any part of this documentation
and/or software.
*/

#include "md5.hpp"
#include <cstdio> // sprintf, sscanf
#include <cassert>

namespace boost
{

namespace
{
  /*
  Pack/unpack between arrays of u8 and u32 in
  a platform-independent way. Size is a multiple of 4.
  */

  void pack(u8* dst, const u32* src, u32 size)
  {
    u32 i(0);
    u32 j(0);

    while (j < size) {
      dst[j + 0] = static_cast<u8>((src[i] >> 0) & 0xff);
      dst[j + 1] = static_cast<u8>((src[i] >> 8) & 0xff);
      dst[j + 2] = static_cast<u8>((src[i] >> 16) & 0xff);
      dst[j + 3] = static_cast<u8>((src[i] >> 24) & 0xff);

      ++i;

      j += 4;
    }
  }

  void unpack(u32* dst, const u8* src, u32 size)
  {
    u32 i(0);
    u32 j(0);

    while (j < size) {
      dst[i] = (static_cast<u32>(src[j + 0]) << 0)
               | (static_cast<u32>(src[j + 1]) << 8)
               | (static_cast<u32>(src[j + 2]) << 16)
               | (static_cast<u32>(src[j + 3]) << 24);

      ++i;

      j += 4;
    }
  }

  // void* secure_memset(void* dst, int c, u32 size)
  //{
  //    return memset(dst, c, size);
  //}
} // namespace

md5::md5()
{
  init();
}

md5::~md5()
{
  // Zeroize sensitive information.
  // secure_memset(the_buffer, 0, sizeof(the_buffer));
}

md5::md5(const char* a_str)
{
  init();

  update(a_str);
}

md5::md5(const void* a_data, u32 a_data_size)
{
  init();

  update(a_data, a_data_size);
}

md5::md5(std::istream& a_istream)
{
  init();

  update(a_istream);
}

md5::md5(std::istream& a_istream, u32 a_size)
{
  init();

  update(a_istream, a_size);
}

void md5::init()
{
  the_is_dirty = true;

  the_count[0] = 0;
  the_count[1] = 0;

  the_state[0] = 0x67452301;
  the_state[1] = 0xefcdab89;
  the_state[2] = 0x98badcfe;
  the_state[3] = 0x10325476;
}

void md5::update(const char* a_str)
{
  update(
    a_str,
    (unsigned int)strlen(a_str)); // Optimization possible but not worth it.
}

void md5::update(const void* a_data, u32 a_data_size)
{
  // Continuation after finalization is not implemented.
  // It is easy to implement - see comments in digest().
  assert(the_is_dirty);

  if (a_data_size != 0)
    the_is_dirty = true;

  // Compute number of bytes mod 64.
  u32 buffer_index = static_cast<u32>((the_count[0] >> 3) & 0x3f);

  // Update number of bits.
  the_count[0] += (static_cast<u32>(a_data_size) << 3);
  if (the_count[0] < (static_cast<u32>(a_data_size) << 3))
    ++the_count[1];

  the_count[1] += (static_cast<u32>(a_data_size) >> 29);

  u32 buffer_space = 64 - buffer_index; // Remaining buffer space.

  u32 input_index;

  // Transform as many times as possible.
  if (a_data_size >= buffer_space) {
    memcpy(the_buffer + buffer_index, a_data, buffer_space);
    process_block(&the_buffer);

    for (input_index = buffer_space; input_index + 63 < a_data_size;
         input_index += 64) {
      process_block(reinterpret_cast<const u8(*)[64]>(
        reinterpret_cast<const u8*>(a_data) + input_index));
    }

    buffer_index = 0;
  }
  else {
    input_index = 0;
  }

  // Buffer remaining input.
  memcpy(
    the_buffer + buffer_index,
    reinterpret_cast<const u8*>(a_data) + input_index,
    a_data_size - input_index);
}

void md5::update(std::istream& a_istream)
{
  u8* buffer = new u8[1024 * 1024];
  while (a_istream) {
    a_istream.read(reinterpret_cast<char*>(&buffer[0]), sizeof(buffer));

    update(buffer, static_cast<u32>(a_istream.gcount()));
  }
  delete[] buffer;
}

void md5::update(std::istream& a_istream, u32 a_size)
{
  // TODO
}

const md5::digest_type& md5::digest()
{
  if (the_is_dirty) {
    static const u8 padding[64] = { 0x80, 0, 0, 0, 0,    0,    0, 0, 0, 0, 0,
                                    0,    0, 0, 0, 0,    0x00, 0, 0, 0, 0, 0,
                                    0,    0, 0, 0, 0,    0,    0, 0, 0, 0, 0x00,
                                    0,    0, 0, 0, 0,    0,    0, 0, 0, 0, 0,
                                    0,    0, 0, 0, 0x00, 0,    0, 0, 0, 0, 0,
                                    0,    0, 0, 0, 0,    0,    0, 0, 0 };

    // Save number of bits.
    u8 saved_count[8];
    pack(saved_count, the_count, 8);

    // TODO: State and buffer must be saved here to make continuation possible.

    // Pad out to 56 mod 64.
    u32 index = static_cast<u32>((the_count[0] >> 3) & 0x3f);
    u32 padding_size = (index < 56) ? (56 - index) : (120 - index);
    update(padding, padding_size);

    // Append size before padding.
    update(saved_count, 8);

    // Store state in digest.
    digest_type::value_type digest_value;
    pack(digest_value, the_state, sizeof(digest_type::value_type));
    the_digest.reset(digest_value);

    // TODO: State and buffer must be restored here to make continuation
    // possible.

    the_is_dirty = false;
  }

  return the_digest;
}

void md5::digest_type::reset(const hex_str_value_type& a_hex_str_value)
{
  delete[] the_hex_str_value;

  the_hex_str_value = 0;

  assert(a_hex_str_value[sizeof(hex_str_value_type) - 1] == '\0');

  for (unsigned int i(0); i < sizeof(value_type); ++i) {
    unsigned int value;

    int n = sscanf(&a_hex_str_value[i * 2], "%02x", &value);

    assert(n == 1 && value <= 0xff);

    the_value[i] = static_cast<u8>(value);
  }
}

const md5::digest_type::hex_str_value_type& md5::digest_type::hex_str_value()
  const
{
  if (the_hex_str_value == 0) {
    // Note: Have to cast because 'new char[33]' returns 'char*', not
    // 'char(*)[33]'.
    the_hex_str_value =
      reinterpret_cast<hex_str_value_type*>(new hex_str_value_type);

    for (unsigned int i(0); i < sizeof(value_type); ++i) {
      sprintf(&(*the_hex_str_value)[i * 2], "%02x", the_value[i]);
    }

    (*the_hex_str_value)[sizeof(hex_str_value_type) - 1] =
      '\0'; // Not necessary after sprintf.
  }

  return (*the_hex_str_value);
}

namespace
{
  const u32 S11(7);
  const u32 S12(12);
  const u32 S13(17);
  const u32 S14(22);
  const u32 S21(5);
  const u32 S22(9);
  const u32 S23(14);
  const u32 S24(20);
  const u32 S31(4);
  const u32 S32(11);
  const u32 S33(16);
  const u32 S34(23);
  const u32 S41(6);
  const u32 S42(10);
  const u32 S43(15);
  const u32 S44(21);

  inline u32 rotate_left(u32 x, u32 n_bits)
  {
    return (x << n_bits) | (x >> (32 - n_bits));
  }

  /*
  Basic MD5 functions.
  */

  inline u32 F(u32 x, u32 y, u32 z)
  {
    return (x & y) | (~x & z);
  }
  inline u32 G(u32 x, u32 y, u32 z)
  {
    return (x & z) | (y & ~z);
  }
  inline u32 H(u32 x, u32 y, u32 z)
  {
    return x ^ y ^ z;
  }
  inline u32 I(u32 x, u32 y, u32 z)
  {
    return y ^ (x | ~z);
  }

  /*
  Transformations for rounds 1, 2, 3, and 4.
  */

  inline void FF(u32& a, u32 b, u32 c, u32 d, u32 x, u32 s, u32 ac)
  {
    a += F(b, c, d) + x + ac;
    a = rotate_left(a, s) + b;
  }

  inline void GG(u32& a, u32 b, u32 c, u32 d, u32 x, u32 s, u32 ac)
  {
    a += G(b, c, d) + x + ac;
    a = rotate_left(a, s) + b;
  }

  inline void HH(u32& a, u32 b, u32 c, u32 d, u32 x, u32 s, u32 ac)
  {
    a += H(b, c, d) + x + ac;
    a = rotate_left(a, s) + b;
  }

  inline void II(u32& a, u32 b, u32 c, u32 d, u32 x, u32 s, u32 ac)
  {
    a += I(b, c, d) + x + ac;
    a = rotate_left(a, s) + b;
  }
} // namespace

void md5::process_block(const u8 (*a_block)[64])
{
  u32 a(the_state[0]);
  u32 b(the_state[1]);
  u32 c(the_state[2]);
  u32 d(the_state[3]);

  /*volatile*/ u32 x[16];

  unpack(x, reinterpret_cast<const u8*>(a_block), 64);

  // Round 1.
  FF(a, b, c, d, x[0], S11, 0xd76aa478); /*  1 */
  FF(d, a, b, c, x[1], S12, 0xe8c7b756); /*  2 */
  FF(c, d, a, b, x[2], S13, 0x242070db); /*  3 */
  FF(b, c, d, a, x[3], S14, 0xc1bdceee); /*  4 */
  FF(a, b, c, d, x[4], S11, 0xf57c0faf); /*  5 */
  FF(d, a, b, c, x[5], S12, 0x4787c62a); /*  6 */
  FF(c, d, a, b, x[6], S13, 0xa8304613); /*  7 */
  FF(b, c, d, a, x[7], S14, 0xfd469501); /*  8 */
  FF(a, b, c, d, x[8], S11, 0x698098d8); /*  9 */
  FF(d, a, b, c, x[9], S12, 0x8b44f7af); /* 10 */
  FF(c, d, a, b, x[10], S13, 0xffff5bb1); /* 11 */
  FF(b, c, d, a, x[11], S14, 0x895cd7be); /* 12 */
  FF(a, b, c, d, x[12], S11, 0x6b901122); /* 13 */
  FF(d, a, b, c, x[13], S12, 0xfd987193); /* 14 */
  FF(c, d, a, b, x[14], S13, 0xa679438e); /* 15 */
  FF(b, c, d, a, x[15], S14, 0x49b40821); /* 16 */

  // Round 2.
  GG(a, b, c, d, x[1], S21, 0xf61e2562); /* 17 */
  GG(d, a, b, c, x[6], S22, 0xc040b340); /* 18 */
  GG(c, d, a, b, x[11], S23, 0x265e5a51); /* 19 */
  GG(b, c, d, a, x[0], S24, 0xe9b6c7aa); /* 20 */
  GG(a, b, c, d, x[5], S21, 0xd62f105d); /* 21 */
  GG(d, a, b, c, x[10], S22, 0x2441453); /* 22 */
  GG(c, d, a, b, x[15], S23, 0xd8a1e681); /* 23 */
  GG(b, c, d, a, x[4], S24, 0xe7d3fbc8); /* 24 */
  GG(a, b, c, d, x[9], S21, 0x21e1cde6); /* 25 */
  GG(d, a, b, c, x[14], S22, 0xc33707d6); /* 26 */
  GG(c, d, a, b, x[3], S23, 0xf4d50d87); /* 27 */
  GG(b, c, d, a, x[8], S24, 0x455a14ed); /* 28 */
  GG(a, b, c, d, x[13], S21, 0xa9e3e905); /* 29 */
  GG(d, a, b, c, x[2], S22, 0xfcefa3f8); /* 30 */
  GG(c, d, a, b, x[7], S23, 0x676f02d9); /* 31 */
  GG(b, c, d, a, x[12], S24, 0x8d2a4c8a); /* 32 */

  // Round 3.
  HH(a, b, c, d, x[5], S31, 0xfffa3942); /* 33 */
  HH(d, a, b, c, x[8], S32, 0x8771f681); /* 34 */
  HH(c, d, a, b, x[11], S33, 0x6d9d6122); /* 35 */
  HH(b, c, d, a, x[14], S34, 0xfde5380c); /* 36 */
  HH(a, b, c, d, x[1], S31, 0xa4beea44); /* 37 */
  HH(d, a, b, c, x[4], S32, 0x4bdecfa9); /* 38 */
  HH(c, d, a, b, x[7], S33, 0xf6bb4b60); /* 39 */
  HH(b, c, d, a, x[10], S34, 0xbebfbc70); /* 40 */
  HH(a, b, c, d, x[13], S31, 0x289b7ec6); /* 41 */
  HH(d, a, b, c, x[0], S32, 0xeaa127fa); /* 42 */
  HH(c, d, a, b, x[3], S33, 0xd4ef3085); /* 43 */
  HH(b, c, d, a, x[6], S34, 0x4881d05); /* 44 */
  HH(a, b, c, d, x[9], S31, 0xd9d4d039); /* 45 */
  HH(d, a, b, c, x[12], S32, 0xe6db99e5); /* 46 */
  HH(c, d, a, b, x[15], S33, 0x1fa27cf8); /* 47 */
  HH(b, c, d, a, x[2], S34, 0xc4ac5665); /* 48 */

  // Round 4.
  II(a, b, c, d, x[0], S41, 0xf4292244); /* 49 */
  II(d, a, b, c, x[7], S42, 0x432aff97); /* 50 */
  II(c, d, a, b, x[14], S43, 0xab9423a7); /* 51 */
  II(b, c, d, a, x[5], S44, 0xfc93a039); /* 52 */
  II(a, b, c, d, x[12], S41, 0x655b59c3); /* 53 */
  II(d, a, b, c, x[3], S42, 0x8f0ccc92); /* 54 */
  II(c, d, a, b, x[10], S43, 0xffeff47d); /* 55 */
  II(b, c, d, a, x[1], S44, 0x85845dd1); /* 56 */
  II(a, b, c, d, x[8], S41, 0x6fa87e4f); /* 57 */
  II(d, a, b, c, x[15], S42, 0xfe2ce6e0); /* 58 */
  II(c, d, a, b, x[6], S43, 0xa3014314); /* 59 */
  II(b, c, d, a, x[13], S44, 0x4e0811a1); /* 60 */
  II(a, b, c, d, x[4], S41, 0xf7537e82); /* 61 */
  II(d, a, b, c, x[11], S42, 0xbd3af235); /* 62 */
  II(c, d, a, b, x[2], S43, 0x2ad7d2bb); /* 63 */
  II(b, c, d, a, x[9], S44, 0xeb86d391); /* 64 */

  the_state[0] += a;
  the_state[1] += b;
  the_state[2] += c;
  the_state[3] += d;

  // Zeroize sensitive information.
  // secure_memset(reinterpret_cast<u8*>(x), 0, sizeof(x));
}
} // namespace boost
