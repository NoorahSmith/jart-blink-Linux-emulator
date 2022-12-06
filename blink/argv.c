/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2022 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include <stdlib.h>
#include <string.h>

#include "blink/endian.h"
#include "blink/machine.h"

#define STACKALIGN      16
#define LINUX_AT_EXECFN 31

static size_t GetArgListLen(char **p) {
  size_t n;
  for (n = 0; *p; ++p) ++n;
  return n;
}

static i64 PushString(struct Machine *m, char *s) {
  size_t n;
  i64 sp;
  n = strlen(s) + 1;
  sp = Read64(m->sp);
  sp -= n;
  Write64(m->sp, sp);
  CopyToUser(m, sp, s, n);
  return sp;
}

void LoadArgv(struct Machine *m, char *prog, char **args, char **vars) {
  u8 *bytes;
  i64 sp, *p, *bloc;
  size_t i, narg, nenv, naux, nall;
  naux = 1;
  nenv = GetArgListLen(vars);
  narg = GetArgListLen(args);
  nall = 1 + narg + 1 + nenv + 1 + (naux + 1) * 2;
  bloc = (i64 *)malloc(sizeof(i64) * nall);
  p = bloc + nall;
  *--p = 0;
  *--p = 0;
  *--p = PushString(m, prog);
  *--p = LINUX_AT_EXECFN;
  for (*--p = 0, i = nenv; i--;) *--p = PushString(m, vars[i]);
  for (*--p = 0, i = narg; i--;) *--p = PushString(m, args[i]);
  *--p = narg;
  sp = Read64(m->sp);
  while ((sp - nall * sizeof(i64)) & (STACKALIGN - 1)) --sp;
  sp -= nall * sizeof(i64);
  Write64(m->sp, sp);
  Write64(m->di, 0); /* or ape detects freebsd */
  bytes = (u8 *)malloc(nall * 8);
  for (i = 0; i < nall; ++i) {
    Write64(bytes + i * 8, bloc[i]);
  }
  CopyToUser(m, sp, bytes, nall * 8);
  free(bytes);
  free(bloc);
}
