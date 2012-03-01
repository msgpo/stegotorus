/*
cdecode.h - c header for a base64 decoding algorithm

This is part of the libb64 project, and has been placed in the public domain.
For details, see http://sourceforge.net/projects/libb64
*/

#ifndef BASE64_CDECODE_H
#define BASE64_CDECODE_H

#include "b64encode.h"

enum base64_decodestep
{
  step_a, step_b, step_c, step_d
};

struct base64_decodestate
{
  base64_decodestep step;
  char plainchar;
};

void base64_init_decodestate(base64_decodestate* state_in);
int base64_decode_value(char value_in);
int base64_decode_block(const char* code_in, const int length_in,
                        char* plaintext_out, base64_decodestate* state_in);

namespace base64
{
  struct decoder
  {
    base64_decodestate _state;
    int _buffersize;

    decoder(int buffersize_in = BUFFERSIZE)
    : _buffersize(buffersize_in)
    {
    }

    int decode(char value_in)
    {
      return base64_decode_value(value_in);
    }

    int decode(const char* code_in, const int length_in, char* plaintext_out)
    {
      base64_init_decodestate(&_state);
      return base64_decode_block(code_in, length_in, plaintext_out, &_state);
    }
  };
} // namespace base64

#endif /* BASE64_CDECODE_H */
