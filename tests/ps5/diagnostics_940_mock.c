// SPDX-License-Identifier: GPL-3.0-or-later
/* Host-only fault injection. No PS5 APIs or console connection are used. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "common/process/inject.h"

static FILE *test_log;
static int scenario, counts[32], attached, detached, remote_uncertain;
static uint64_t tick;
static unsigned char scratch[4096], remote[0x200000];
static FILE *mock_fopen(const char *name, const char *mode) {
  (void)mode;
  FILE *f=tmpfile();
  if (strstr(name,".log")) test_log=f;
  return f;
}
static int mock_fclose(FILE *f) { return f==test_log ? fflush(f) : fclose(f); }
static int mock_mkdir(const char *p, mode_t mode) { (void)p; (void)mode; return 0; }
static int mock_clock(clockid_t id, struct timespec *t) {
  (void)id; t->tv_sec=tick/1000000; t->tv_nsec=(tick%1000000)*1000; return 0;
}
#define main diagnostic_main
#define fopen mock_fopen
#define fclose mock_fclose
#define mkdir mock_mkdir
#define clock_gettime mock_clock
#include "../../tools/diagnostics/ps5_940_main.c"
#undef main
#undef fopen
#undef fclose
#undef mkdir
#undef clock_gettime

enum { INIT=1, CLOSE, ENABLE, SET, CHANGE, CLEAR, CB, START, STOP, READ,
       HEADERS, ALLOC, RELEASE, OPEN, GET };
enum { GOOD, WRONG_FW, OPEN_REJECT, OPEN_TIMEOUT, START_REJECT, START_TIMEOUT,
       READ_ERROR, READ_TIMEOUT, STOP_ERROR, CLOSE_ERROR, CLEAR_ERROR, NO_VIDEO,
       INIT_ERROR, BAD_FINGERPRINT, HANDLE_READ_ERROR, CALLBACK_REJECT,
       OPEN_ASSERT, ENABLE_TIMEOUT, GETTER_TIMEOUT, READ_ASSERT };

uint32_t kernel_get_fw_version(void) { return scenario==WRONG_FW ? 0x09200005 : 0x09400008; }
int kernel_dynlib_handle(pid_t p, const char *n, uint32_t *h) { (void)p;(void)n; *h=1;return 0; }
intptr_t kernel_dynlib_mapbase_addr(pid_t p,uint32_t h) { (void)p;(void)h;return 0x100000; }
intptr_t pbs_resolve_in(pid_t p,const char *m,const char *n) {
  (void)p;(void)m;
  if (!strcmp(n,"sceAvcap2OpenVideo")) return 0x100340;
  if (!strcmp(n,"sceAvcap2GetVideoOutMode")) return 0x101630;
  const char *names[]={"", "sceAvcap2Initialize", "sceAvcap2Close", "sceAvcap2EnableScanInEncode",
    "sceResourceArbitratorSetCallback", "sceResourceArbitratorChangeState", "sceResourceArbitratorClearCallback",
    "getpid", "sceAvcap2Start", "sceAvcap2Stop", "sceAvcap2ReadVideo", "sceAvcap2GetNonVclNalUnitStream",
    "malloc", "free"};
  for (unsigned i=1;i<sizeof names/sizeof *names;++i) if (!strcmp(n,names[i])) return i;
  assert(!"unexpected export (including global Terminate)"); return 0;
}
int pbs_inject_begin(const char *n,pbs_inject_t *ctx) {
  assert(!strcmp(n,"SceRemotePlay")); memset(ctx,0,sizeof *ctx);
  ctx->pid=106;ctx->scratch=0x10000;ctx->scratch_len=sizeof scratch;ctx->attached=1;++attached;return 0;
}
void pbs_inject_end(pbs_inject_t *ctx) { if (ctx->attached) ++detached;ctx->attached=0; }
int pbs_inject_write_at(pbs_inject_t *ctx,intptr_t a,const void *p,size_t n) {
  assert(!remote_uncertain);
  (void)ctx; assert(a>=0x10000 && a+n<=0x11000);memcpy(scratch+a-0x10000,p,n);return 0;
}
int pbs_inject_write(pbs_inject_t *ctx,const void *p,size_t n) { return pbs_inject_write_at(ctx,ctx->scratch,p,n); }
int pbs_inject_read_at(pbs_inject_t *ctx,intptr_t a,void *p,size_t n) {
  assert(!remote_uncertain);
  (void)ctx;
  if (a==0x103f45) { memcpy(p,(unsigned char[]){0xbe,1,0,0xa7,0},n); if(scenario==BAD_FINGERPRINT) ((char*)p)[0]=0;return 0; }
  if (a==0x103f29) { memcpy(p,(unsigned char[]){0x48,0xc7,0x45,0xe8,0xb0,2,0,0},n);return 0; }
  if (a==0x200000) { assert(n<=sizeof remote);memcpy(p,remote,n);return 0; }
  assert(a>=0x10000 && a+n<=0x11000);memcpy(p,scratch+a-0x10000,n);return 0;
}
int pbs_inject_read(pbs_inject_t *ctx,void *p,size_t n) {
  if (scenario==HANDLE_READ_ERROR) return -1;
  return pbs_inject_read_at(ctx,ctx->scratch,p,n);
}
int kernel_proc_copyout(pid_t pid,intptr_t addr,void *buf,size_t len) {
  (void)pid;return pbs_inject_read_at(NULL,addr,buf,len);
}
long pbs_inject_call_fast(pbs_inject_t *ctx,intptr_t f,const uint64_t a[6]) {
  assert(!remote_uncertain);
  int id=f==0x100340 ? OPEN : f==0x101630 ? GET : (int)f;
  assert(id>0 && id<32);++counts[id];ctx->call_stage="mock-complete";
  if ((id==OPEN && scenario==OPEN_TIMEOUT) || (id==START && scenario==START_TIMEOUT) ||
      (id==READ && scenario==READ_TIMEOUT) || (id==ENABLE && scenario==ENABLE_TIMEOUT) ||
      (id==GET && scenario==GETTER_TIMEOUT)) { remote_uncertain=1; return LONG_MIN; }
  if ((id==OPEN && scenario==OPEN_ASSERT) || (id==READ && scenario==READ_ASSERT)) {
    remote_uncertain=1;ctx->asserted=1;return 0;
  }
  if ((id==OPEN && scenario==OPEN_REJECT) || (id==START && scenario==START_REJECT) ||
      (id==READ && scenario==READ_ERROR) || (id==STOP && scenario==STOP_ERROR) ||
      (id==CLOSE && scenario==CLOSE_ERROR) || (id==CLEAR && scenario==CLEAR_ERROR) ||
      (id==INIT && scenario==INIT_ERROR) || (id==SET && scenario==CALLBACK_REJECT)) return (long)0x81950008u;
  if (id==GET) { uint32_t m=0x40;memcpy(scratch+a[0]-0x10000,&m,4); }
  if (id==OPEN) {
    assert(counts[OPEN]==1);
    uint32_t v; memcpy(&v,scratch+8+0x1c,4);assert(v==PBS_940_FPS_MODE);
    memcpy(&v,scratch+8+0x1d0,4);assert(v==PBS_940_WIDTH);
    memcpy(&v,scratch+8+0x1d4,4);assert(v==PBS_940_HEIGHT);
    memcpy(&v,scratch+8+0x28,4);assert(v==PBS_940_FEATURE);
    memcpy(&v,scratch+8+0x280,4);assert(v==0x40);
    uint64_t h=0x1234;memcpy(scratch,&h,8);
  }
  if (id==ALLOC) return 0x200000;
  if (id==HEADERS) {
    memset(remote,0,sizeof remote);uint64_t len=2;
    memcpy(remote+0x10,&len,8);memcpy(remote+0x98,&len,8);
    remote[0x18]=0x67;remote[0xa0]=0x68;
  }
  if (id==READ) {
    tick+=16667;
    if (scenario==NO_VIDEO) return 0;
    /* AUD + first IDR slice + another slice in the same picture. */
    unsigned char frame[]={0,0,0,1,9,0xf0,0,0,1,0x65,0x80,0,0,0,1,0x65,0x40};
    memcpy(remote,frame,sizeof frame);return sizeof frame;
  }
  if (id==RELEASE) return -1; /* free is void: arbitrary RAX is not a status. */
  return 0;
}

int main(void) {
  for (scenario=GOOD;scenario<=READ_ASSERT;++scenario) {
#if !PBS_940_CAPTURE
    if (scenario>=START_REJECT && scenario<=STOP_ERROR) continue;
    if (scenario==NO_VIDEO) continue;
    if (scenario==READ_ASSERT) continue;
#endif
    memset(counts,0,sizeof counts);memset(scratch,0,sizeof scratch);
    tick=1000000;attached=detached=remote_uncertain=0;test_log=NULL;
    char *args[]={"mock-diagnostic",NULL};
    int rc=diagnostic_main(1,args);
    assert((rc==0)==(scenario==GOOD));
    assert(attached==detached);
    assert(counts[OPEN]<=1 && counts[START]<=1 && counts[STOP]<=1 && counts[CLOSE]<=1 && counts[CLEAR]<=1);
    if (scenario==WRONG_FW || scenario==BAD_FINGERPRINT) assert(!counts[INIT]);
    if (scenario==INIT_ERROR || scenario==CALLBACK_REJECT) assert(!counts[OPEN] && !counts[CLEAR]);
    if (scenario==OPEN_TIMEOUT || scenario==START_TIMEOUT || scenario==READ_TIMEOUT)
      assert(!counts[STOP] && !counts[CLOSE] && !counts[CLEAR] && !counts[RELEASE]);
    if (scenario==STOP_ERROR) assert(!counts[CLOSE] && !counts[CLEAR] && !counts[RELEASE]);
    if (scenario==CLOSE_ERROR || scenario==HANDLE_READ_ERROR) assert(!counts[CLEAR]);
    if (scenario==OPEN_REJECT) assert(counts[CLEAR]==1 && !counts[CLOSE] && !counts[START]);
    if (scenario==START_REJECT) assert(counts[CLOSE]==1 && counts[CLEAR]==1 && !counts[STOP]);
    if (scenario==READ_ERROR || scenario==NO_VIDEO) assert(counts[STOP]==1 && counts[CLOSE]==1 && counts[CLEAR]==1);
    if (scenario==GOOD) {
      assert(counts[CLOSE]==1 && counts[CLEAR]==1);
#if PBS_940_CAPTURE
      assert(counts[READ]==600 && counts[STOP]==1 && counts[START]==1);
#endif
    }
#if !PBS_940_CAPTURE
    assert(!counts[START] && !counts[READ] && !counts[STOP] && !counts[ALLOC]);
#endif
    rewind(test_log);char log[16384];size_t n=fread(log,1,sizeof log-1,test_log);log[n]=0;
    assert(strstr(log,"attempts=1") && strstr(log,"END result="));
    if (scenario==GOOD) assert(strstr(log,"cleanup=complete"));
#if PBS_940_CAPTURE
    if (scenario==GOOD) assert(strstr(log,"picture_starts=600") && strstr(log,"candidate_picture_fps=59.999"));
#endif
    fclose(test_log);
    printf("PASS offline mock capture=%d scenario=%d\n",PBS_940_CAPTURE,scenario);
  }
  return 0;
}
