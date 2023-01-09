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
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "blink/assert.h"
#include "blink/builtin.h"
#include "blink/debug.h"
#include "blink/dis.h"
#include "blink/high.h"
#include "blink/log.h"
#include "blink/machine.h"
#include "blink/macros.h"
#include "blink/modrm.h"
#include "blink/stats.h"

#define APPEND(...) o += snprintf(b + o, n - o, __VA_ARGS__)

void (*AddPath_StartOp_Hook)(P);

#ifdef CLOG
static int g_clog;
static struct Dis g_dis;
#endif

static void StartPath(struct Machine *m) {
  JIX_LOGF("%" PRIx64 " <path>", m->ip);
}

static void DebugOp(struct Machine *m, i64 expected_ip) {
  if (m->ip != expected_ip) {
    LOGF("IP was %" PRIx64 " but it should have been %" PRIx64, m->ip,
         expected_ip);
  }
  unassert(m->ip == expected_ip);
}

static void StartOp(struct Machine *m) {
  JIX_LOGF("%" PRIx64 "   <op>", GetPc(m));
  JIX_LOGF("%" PRIx64 "     %s", GetPc(m), DescribeOp(m, GetPc(m)));
  unassert(!IsMakingPath(m));
}

static void EndOp(struct Machine *m) {
  JIX_LOGF("%" PRIx64 "   </op>", GetPc(m));
  m->oplen = 0;
  if (m->stashaddr) {
    CommitStash(m);
  }
}

static void EndPath(struct Machine *m) {
  JIX_LOGF("%" PRIx64 "   %s", GetPc(m), DescribeOp(m, GetPc(m)));
  JIX_LOGF("%" PRIx64 " </path>", GetPc(m));
}

#ifdef HAVE_JIT
#if defined(__x86_64__)
static const u8 kEnter[] = {
    0x55,                    // push %rbp
    0x48, 0x89, 0345,        // mov  %rsp,%rbp
    0x48, 0x83, 0354, 0x30,  // sub  $0x30,%rsp
    0x48, 0x89, 0135, 0xd8,  // mov  %rbx,-0x28(%rbp)
    0x4c, 0x89, 0145, 0xe0,  // mov  %r12,-0x20(%rbp)
    0x4c, 0x89, 0155, 0xe8,  // mov  %r13,-0x18(%rbp)
    0x4c, 0x89, 0165, 0xf0,  // mov  %r14,-0x10(%rbp)
    0x4c, 0x89, 0175, 0xf8,  // mov  %r15,-0x08(%rbp)
    0x48, 0x89, 0xfb,        // mov  %rdi,%rbx
};
static const u8 kLeave[] = {
    0x4c, 0x8b, 0175, 0xf8,  // mov -0x08(%rbp),%r15
    0x4c, 0x8b, 0165, 0xf0,  // mov -0x10(%rbp),%r14
    0x4c, 0x8b, 0155, 0xe8,  // mov -0x18(%rbp),%r13
    0x4c, 0x8b, 0145, 0xe0,  // mov -0x20(%rbp),%r12
    0x48, 0x8b, 0135, 0xd8,  // mov -0x28(%rbp),%rbx
    0x48, 0x83, 0304, 0x30,  // add $0x30,%rsp
    0x5d,                    // pop %rbp
};
#elif defined(__aarch64__)
static const u32 kEnter[] = {
    0xa9bc7bfd,  // stp x29, x30, [sp, #-64]!
    0x910003fd,  // mov x29, sp
    0xa90153f3,  // stp x19, x20, [sp, #16]
    0xa9025bf5,  // stp x21, x22, [sp, #32]
    0xa90363f7,  // stp x23, x24, [sp, #48]
    0xaa0003f3,  // mov x19, x0
};
static const u32 kLeave[] = {
    0xa94153f3,  // ldp x19, x20, [sp, #16]
    0xa9425bf5,  // ldp x21, x22, [sp, #32]
    0xa94363f7,  // ldp x23, x24, [sp, #48]
    0xa8c47bfd,  // ldp x29, x30, [sp], #64
};
#endif
#endif

long GetPrologueSize(void) {
#ifdef HAVE_JIT
  return sizeof(kEnter);
#else
  return 0;
#endif
}

void SetupClog(struct Machine *m) {
#ifdef CLOG
  LoadDebugSymbols(&m->system->elf);
  DisLoadElf(&g_dis, &m->system->elf);
  g_clog = open("/tmp/blink.s", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  g_clog = fcntl(g_clog, F_DUPFD_CLOEXEC, kMinBlinkFd);
#endif
}

static void WriteClog(const char *fmt, ...) {
#ifdef CLOG
  int n;
  va_list va;
  char buf[256];
  if (!g_clog) return;
  va_start(va, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, va);
  va_end(va);
  write(g_clog, buf, MIN(n, sizeof(buf)));
#endif
}

static void BeginClog(struct Machine *m, i64 pc) {
#ifdef CLOG
  char b[256];
  char spec[64];
  int i, o = 0, n = sizeof(b);
  if (!g_clog) return;
  DISABLE_HIGHLIGHT_BEGIN;
  APPEND("/\t");
  unassert(!GetInstruction(m, pc, g_dis.xedd));
  DisInst(&g_dis, b + o, DisSpec(g_dis.xedd, spec));
  o = strlen(b);
  APPEND(" #");
  for (i = 0; i < g_dis.xedd->length; ++i) {
    APPEND(" %02x", g_dis.xedd->bytes[i]);
  }
  APPEND("\n");
  write(g_clog, b, MIN(o, n));
  DISABLE_HIGHLIGHT_END;
#endif
}

static void FlushClog(struct JitBlock *jb) {
#ifdef CLOG
  char b[256];
  char spec[64];
  if (!g_clog) return;
  if (jb->index == jb->blocksize + 1) {
    WriteClog("/\tOOM!\n");
    jb->clog = jb->index;
    return;
  }
  DISABLE_HIGHLIGHT_BEGIN;
  for (; jb->clog < jb->index; jb->clog += g_dis.xedd->length) {
    unassert(!DecodeInstruction(g_dis.xedd, jb->addr + jb->clog,
                                jb->index - jb->clog, XED_MODE_LONG));
    g_dis.addr = (intptr_t)jb->addr + jb->clog;
    DisInst(&g_dis, b, DisSpec(g_dis.xedd, spec));
    WriteClog("\t%s\n", b);
  }
  DISABLE_HIGHLIGHT_END;
#endif
}

static bool IsPure(u64 rde) {
  switch (Mopcode(rde)) {
    case 0x004:  // OpAluAlIbAdd
    case 0x005:  // OpAluRaxIvds
    case 0x00C:  // OpAluAlIbOr
    case 0x00D:  // OpAluRaxIvds
    case 0x014:  // OpAluAlIbAdc
    case 0x015:  // OpAluRaxIvds
    case 0x01C:  // OpAluAlIbSbb
    case 0x01D:  // OpAluRaxIvds
    case 0x024:  // OpAluAlIbAnd
    case 0x025:  // OpAluRaxIvds
    case 0x02C:  // OpAluAlIbSub
    case 0x02D:  // OpAluRaxIvds
    case 0x034:  // OpAluAlIbXor
    case 0x035:  // OpAluRaxIvds
    case 0x03C:  // OpCmpAlIb
    case 0x03D:  // OpCmpRaxIvds
    case 0x090:  // OpNop
    case 0x091:  // OpXchgZvqp
    case 0x092:  // OpXchgZvqp
    case 0x093:  // OpXchgZvqp
    case 0x094:  // OpXchgZvqp
    case 0x095:  // OpXchgZvqp
    case 0x096:  // OpXchgZvqp
    case 0x097:  // OpXchgZvqp
    case 0x098:  // OpSax
    case 0x099:  // OpConvert
    case 0x09E:  // OpSahf
    case 0x09F:  // OpLahf
    case 0x0A1:  // OpMovRaxOvqp
    case 0x0A3:  // OpMovOvqpRax
    case 0x0A8:  // OpTestAlIb
    case 0x0A9:  // OpTestRaxIvds
    case 0x0B0:  // OpMovZbIb
    case 0x0B1:  // OpMovZbIb
    case 0x0B2:  // OpMovZbIb
    case 0x0B3:  // OpMovZbIb
    case 0x0B4:  // OpMovZbIb
    case 0x0B5:  // OpMovZbIb
    case 0x0B6:  // OpMovZbIb
    case 0x0B7:  // OpMovZbIb
    case 0x0B8:  // OpMovZvqpIvqp
    case 0x0B9:  // OpMovZvqpIvqp
    case 0x0BA:  // OpMovZvqpIvqp
    case 0x0BB:  // OpMovZvqpIvqp
    case 0x0BC:  // OpMovZvqpIvqp
    case 0x0BD:  // OpMovZvqpIvqp
    case 0x0BE:  // OpMovZvqpIvqp
    case 0x0BF:  // OpMovZvqpIvqp
    case 0x0D6:  // OpSalc
    case 0x0F5:  // OpCmc
    case 0x0F8:  // OpClc
    case 0x0F9:  // OpStc
    case 0x11F:  // OpNopEv
    case 0x150:  // OpMovmskpsd
    case 0x1C8:  // OpBswapZvqp
    case 0x1C9:  // OpBswapZvqp
    case 0x1CA:  // OpBswapZvqp
    case 0x1CB:  // OpBswapZvqp
    case 0x1CC:  // OpBswapZvqp
    case 0x1CD:  // OpBswapZvqp
    case 0x1CE:  // OpBswapZvqp
    case 0x1CF:  // OpBswapZvqp
      return true;
    case 0x000:  // OpAlub
    case 0x001:  // OpAluw
    case 0x002:  // OpAlubFlip
    case 0x003:  // OpAluwFlip
    case 0x008:  // OpAlub
    case 0x009:  // OpAluw
    case 0x00A:  // OpAlubFlip
    case 0x00B:  // OpAluwFlip
    case 0x010:  // OpAlub
    case 0x011:  // OpAluw
    case 0x012:  // OpAlubFlip
    case 0x013:  // OpAluwFlip
    case 0x018:  // OpAlub
    case 0x019:  // OpAluw
    case 0x01A:  // OpAlubFlip
    case 0x01B:  // OpAluwFlip
    case 0x020:  // OpAlub
    case 0x021:  // OpAluw
    case 0x022:  // OpAlubFlip
    case 0x023:  // OpAluwFlip
    case 0x028:  // OpAlub
    case 0x029:  // OpAluw
    case 0x02A:  // OpAlubFlip
    case 0x02B:  // OpAluwFlip
    case 0x030:  // OpAlub
    case 0x031:  // OpAluw
    case 0x032:  // OpAlubFlip
    case 0x033:  // OpAluwFlip
    case 0x038:  // OpAlubCmp
    case 0x039:  // OpAluwCmp
    case 0x03A:  // OpAlubFlipCmp
    case 0x03B:  // OpAluwFlipCmp
    case 0x063:  // OpMovsxdGdqpEd
    case 0x069:  // OpImulGvqpEvqpImm
    case 0x06B:  // OpImulGvqpEvqpImm
    case 0x080:  // OpAlubiReg
    case 0x081:  // OpAluwiReg
    case 0x082:  // OpAlubiReg
    case 0x083:  // OpAluwiReg
    case 0x084:  // OpAlubTest
    case 0x085:  // OpAluwTest
    case 0x086:  // OpXchgGbEb
    case 0x087:  // OpXchgGvqpEvqp
    case 0x088:  // OpMovEbGb
    case 0x089:  // OpMovEvqpGvqp
    case 0x08A:  // OpMovGbEb
    case 0x08B:  // OpMovGvqpEvqp
    case 0x0C0:  // OpBsubiImm
    case 0x0C1:  // OpBsuwiImm
    case 0x0C6:  // OpMovEbIb
    case 0x0C7:  // OpMovEvqpIvds
    case 0x0D0:  // OpBsubi1
    case 0x0D1:  // OpBsuwi1
    case 0x0D2:  // OpBsubiCl
    case 0x0D3:  // OpBsuwiCl
    case 0x0F6:  // Op0f6
    case 0x0F7:  // Op0f7
    case 0x140:  // OpCmovo
    case 0x141:  // OpCmovno
    case 0x142:  // OpCmovb
    case 0x143:  // OpCmovae
    case 0x144:  // OpCmove
    case 0x145:  // OpCmovne
    case 0x146:  // OpCmovbe
    case 0x147:  // OpCmova
    case 0x148:  // OpCmovs
    case 0x149:  // OpCmovns
    case 0x14A:  // OpCmovp
    case 0x14B:  // OpCmovnp
    case 0x14C:  // OpCmovl
    case 0x14D:  // OpCmovge
    case 0x14E:  // OpCmovle
    case 0x14F:  // OpCmovg
    case 0x190:  // OpSeto
    case 0x191:  // OpSetno
    case 0x192:  // OpSetb
    case 0x193:  // OpSetae
    case 0x194:  // OpSete
    case 0x195:  // OpSetne
    case 0x196:  // OpSetbe
    case 0x197:  // OpSeta
    case 0x198:  // OpSets
    case 0x199:  // OpSetns
    case 0x19A:  // OpSetp
    case 0x19B:  // OpSetnp
    case 0x19C:  // OpSetl
    case 0x19D:  // OpSetge
    case 0x19E:  // OpSetle
    case 0x19F:  // OpSetg
    case 0x1A3:  // OpBit
    case 0x1A4:  // OpDoubleShift
    case 0x1A5:  // OpDoubleShift
    case 0x1AB:  // OpBit
    case 0x1AC:  // OpDoubleShift
    case 0x1AD:  // OpDoubleShift
    case 0x1AF:  // OpImulGvqpEvqp
    case 0x1B3:  // OpBit
    case 0x1B6:  // OpMovzbGvqpEb
    case 0x1B7:  // OpMovzwGvqpEw
    case 0x1BA:  // OpBit
    case 0x1BB:  // OpBit
    case 0x1BC:  // OpBsf
    case 0x1BD:  // OpBsr
    case 0x1BE:  // OpMovsbGvqpEb
    case 0x1BF:  // OpMovswGvqpEw
      return IsModrmRegister(rde);
    case 0x08D:  // OpLeaGvqpM
      return !IsRipRelative(rde);
    default:
      return false;
  }
}

static bool MustUpdateIp(P) {
  u64 next;
  if (!IsPure(rde)) return true;
  next = m->ip + Oplength(rde);
  if (!HasHook(m, next)) return true;
  if (GetHook(m, next) != GeneralDispatch) return true;
  return false;
}

static void InitPaths(struct System *s) {
#ifdef HAVE_JIT
  struct JitBlock *jb;
  if (!s->ender) {
    unassert((jb = StartJit(&s->jit)));
    WriteClog("\nJit_%" PRIx64 ":\n", jb->addr + jb->index);
    s->ender = GetJitPc(jb);
#if LOG_JIX
    AppendJitMovReg(jb, kJitArg0, kJitSav0);
    AppendJitCall(jb, (void *)EndPath);
#endif
    AppendJit(jb, kLeave, sizeof(kLeave));
    AppendJitRet(jb);
    FlushClog(jb);
    unassert(FinishJit(&s->jit, jb, 0));
  }
#endif
}

bool CreatePath(P) {
#ifdef HAVE_JIT
  bool res;
  i64 pc, jpc;
  unassert(!IsMakingPath(m));
  InitPaths(m->system);
  if ((pc = GetPc(m))) {
    if ((m->path.jb = StartJit(&m->system->jit))) {
      JIT_LOGF("starting new path jit_pc:%" PRIxPTR " at pc:%" PRIx64,
               GetJitPc(m->path.jb), pc);
      FlushClog(m->path.jb);
      jpc = (intptr_t)m->path.jb->addr + m->path.jb->index;
      AppendJit(m->path.jb, kEnter, sizeof(kEnter));
#if LOG_JIX
      Jitter(A,
             "q"   // arg0 = machine
             "c"   // call function (StartPath)
             "q",  // arg0 = machine
             StartPath);
#endif
      WriteClog("\nJit_%" PRIx64 "_%" PRIx64 ":\n", pc, jpc);
      FlushClog(m->path.jb);
      m->path.start = pc;
      m->path.elements = 0;
      SetHook(m, pc, JitlessDispatch);
      res = true;
    } else {
      LOGF("jit failed: %s", strerror(errno));
      res = false;
    }
  } else {
    res = false;
  }
  return res;
#else
  return false;
#endif
}

void CompletePath(P) {
  unassert(IsMakingPath(m));
  FlushSkew(A);
  AppendJitJump(m->path.jb, (void *)m->system->ender);
  FinishPath(m);
}

void FinishPath(struct Machine *m) {
  unassert(IsMakingPath(m));
  FlushClog(m->path.jb);
  STATISTIC(path_longest_bytes =
                MAX(path_longest_bytes, m->path.jb->index - m->path.jb->start));
  STATISTIC(path_longest = MAX(path_longest, m->path.elements));
  STATISTIC(AVERAGE(path_average_elements, m->path.elements));
  STATISTIC(AVERAGE(path_average_bytes, m->path.jb->index - m->path.jb->start));
  if (FinishJit(&m->system->jit, m->path.jb, m->fun + m->path.start)) {
    STATISTIC(++path_count);
    JIT_LOGF("staged path to %" PRIx64, m->path.start);
  } else {
    STATISTIC(++path_ooms);
    JIT_LOGF("path starting at %" PRIx64 " ran out of space", m->path.start);
    SetHook(m, m->path.start, 0);
  }
  m->path.jb = 0;
}

void AbandonPath(struct Machine *m) {
  WriteClog("/\tABANDONED\n");
  unassert(IsMakingPath(m));
  STATISTIC(++path_abandoned);
  JIT_LOGF("abandoning path jit_pc:%" PRIxPTR " which started at pc:%" PRIx64,
           GetJitPc(m->path.jb), m->path.start);
  AbandonJit(&m->system->jit, m->path.jb);
  SetHook(m, m->path.start, 0);
  m->path.skew = 0;
  m->path.jb = 0;
}

void FlushSkew(P) {
  unassert(IsMakingPath(m));
  if (m->path.skew) {
    JIT_LOGF("adding %d to ip", m->path.skew);
    WriteClog("/\tflush skew\n");
    Jitter(A,
           "a1i"  // arg1 = skew
           "q"    // arg0 = machine
           "m",   // arg0 = machine
           m->path.skew, AddIp);
    m->path.skew = 0;
  }
}

void AddPath_StartOp(P) {
  BeginClog(m, GetPc(m));
#ifndef NDEBUG
  if (FLAG_statistics) {
    Jitter(A,
           "a0i"  // arg0 = &instructions_jitted
           "m",   // call micro-op (CountOp)
           &instructions_jitted, CountOp);
  }
#endif
  if (AddPath_StartOp_Hook) {
    AddPath_StartOp_Hook(A);
  }
#if LOG_JIX || defined(DEBUG)
  Jitter(A,
         "a1i"  // arg1 = m->ip
         "q"    // arg0 = machine
         "c",   // call function (DebugOp)
         m->ip, DebugOp);
#endif
#if LOG_JIX
  Jitter(A,
         "a1i"  // arg1 = Oplength(rde)
         "q"    // arg0 = machine
         "c",   // call function (StartOp)
         Oplength(rde), StartOp);
#endif
  if (MustUpdateIp(A)) {
    if (!m->path.skew) {
      JIT_LOGF("adding %d to ip", Oplength(rde));
      Jitter(A,
             "a1i"  // arg1 = Oplength(rde)
             "q"    // arg0 = machine
             "m",   // call micro-op (AddIp)
             Oplength(rde), AddIp);
    } else {
      JIT_LOGF("adding %d+%d to ip", m->path.skew, Oplength(rde));
      Jitter(A,
             "a2i"  // arg1 = program counter delta
             "a1i"  // arg1 = instruction length
             "q"    // arg0 = machine
             "m",   // call micro-op (SkewIp)
             m->path.skew + Oplength(rde), Oplength(rde), SkewIp);
      m->path.skew = 0;
    }
  } else {
    m->path.skew += Oplength(rde);
  }
  AppendJitMovReg(m->path.jb, kJitArg0, kJitSav0);
  m->reserving = false;
}

void AddPath_EndOp(P) {
  _Static_assert(offsetof(struct Machine, stashaddr) < 128, "");
  if (m->reserving) {
    WriteClog("/\tflush reserve\n");
  }
#if !LOG_JIX && defined(__x86_64__)
  if (m->reserving) {
    AppendJitMovReg(m->path.jb, kJitArg0, kJitSav0);
    u8 sa = offsetof(struct Machine, stashaddr);
    u8 code[] = {
        0x48, 0x83, 0177, sa, 0x00,  // cmpq $0x0,0x18(%rdi)
        0x74, 0x05,                  // jz +5
    };
    AppendJit(m->path.jb, code, sizeof(code));
    AppendJitCall(m->path.jb, (void *)CommitStash);
  }
#elif !LOG_JIX && defined(__aarch64__)
  if (m->reserving) {
    AppendJitMovReg(m->path.jb, kJitArg0, kJitSav0);
    u32 sa = offsetof(struct Machine, stashaddr);
    u32 code[] = {
        0xf9400001 | (sa / 8) << 10,  // ldr x1, [x0, #stashaddr]
        0xb4000001 | 2 << 5,          // cbz x1, +2
    };
    AppendJit(m->path.jb, code, sizeof(code));
    AppendJitCall(m->path.jb, (void *)CommitStash);
  }
#else
  Jitter(A,
         "q"   // arg0 = machine
         "c",  // call function (EndOp)
         EndOp);
#endif
  FlushClog(m->path.jb);
}

bool AddPath(P) {
  unassert(IsMakingPath(m));
  AppendJitSetReg(m->path.jb, kJitArg[kArgRde], rde);
  AppendJitSetReg(m->path.jb, kJitArg[kArgDisp], disp);
  AppendJitSetReg(m->path.jb, kJitArg[kArgUimm0], uimm0);
  AppendJitCall(m->path.jb, (void *)GetOp(Mopcode(rde)));
  return true;
}
