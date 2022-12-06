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
#include <errno.h>
#include <string.h>

#include "blink/log.h"
#include "blink/map.h"
#include "blink/types.h"
#include "blink/util.h"

void *Mmap(void *addr,     //
           size_t length,  //
           int prot,       //
           int flags,      //
           int fd,         //
           off_t offset,   //
           const char *owner) {
  void *res = mmap(addr, length, prot, flags, fd, offset);
#if LOG_MEM
  char szbuf[16];
  FormatSize(szbuf, length, 1024);
  if (res != MAP_FAILED) {
    MEM_LOGF("%s created %s map [%p,%p)", owner, szbuf, res, res + length);
  } else {
    MEM_LOGF("%s failed to create %s map [%p,%p): %s", owner, szbuf, (u8 *)addr,
             (u8 *)addr + length, strerror(errno));
  }
#endif
  return res;
}

int Mprotect(void *addr,     //
             size_t length,  //
             int prot,       //
             const char *owner) {
  int res = mprotect(addr, length, prot);
#if LOG_MEM
  char szbuf[16];
  FormatSize(szbuf, length, 1024);
  if (res != -1) {
    MEM_LOGF("%s protected %s map [%p,%p)", owner, szbuf, res, res + length);
  } else {
    MEM_LOGF("%s failed to protect %s map [%p,%p): %s", owner, szbuf,
             (u8 *)addr, (u8 *)addr + length, strerror(errno));
  }
#endif
  return res;
}
