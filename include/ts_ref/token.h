#ifndef TS_REF_TOKEN_H_
#define TS_REF_TOKEN_H_

// Token holds transient input to the parser

#include "ts_ref/types.h"

namespace ts_ref {

struct Token {
  Symbol token_kind;
  ByteRange token_span;
};

}  // namespace ts_ref

#endif
