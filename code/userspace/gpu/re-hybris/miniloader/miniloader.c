/*
 * miniloader: load the ROM's ARM32 libGLES_mali.so without bionic and call
 * its EGL entry points.  Diagnostic only: no GPU, no /dev/mali0 traffic.
 *
 * The Android blob is ET_DYN/ARM with APS2 (SLEB128) packed relocations and
 * 284 undefined symbols.  We map it at a fixed base, apply R_ARM_RELATIVE,
 * R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT and R_ARM_ABS32, resolve undefined symbols
 * to local stubs, then call eglGetDisplay/eglQueryString/eglGetProcAddress.
 */
#include "common.h"

#define BASE 0x20000000u
#define MAX_IMAGE (64u*1024u*1024u)

/* ELF32 on-disk offsets */
#define E_PHOFF 0x1c
#define E_SHOFF 0x20
#define E_PHENTSIZE 0x2a
#define E_PHNUM 0x2c
#define E_SHENTSIZE 0x2e
#define E_SHNUM 0x30

typedef struct { u32 name, value, size; u8 info, other; u16 shndx; } Sym;
typedef struct { u32 name; u32 type; u32 flags; u32 addr; u32 offset; u32 size; u32 link; u32 info; u32 addralign; u32 entsize; } Shdr;

static u32 rd32(const u8* p){ return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24); }

/* ---------------- stubs ---------------- */
static u32 g_errno;
static u32 g_zero_obj[16];
static u32 g_bump_ptr;
static u32 g_bump_end;

static void* bump(usize n){
  n = (n + 7) & ~7u;
  if (!g_bump_ptr || g_bump_ptr + n > g_bump_end){
    u32 sz = 1024*1024;
    long p = sc6(SYS_mmap2, 0, (long)sz, 7 /*RWX*/, 0x22 /*PRIVATE|ANON*/, -1, 0);
    if (p < 0) { out("miniloader: mmap bump failed\n"); sys_exit(1); }
    g_bump_ptr = (u32)p; g_bump_end = (u32)p + sz;
  }
  u32 r = g_bump_ptr; g_bump_ptr += n;
  memset((void*)r, 0, n);
  return (void*)r;
}

static int ret0(void){ return 0; }
static int retm1_keep(void){ return -1; }
static int ret1(void){ return 1; }
/* ARM EABI struct stat64: st_dev@0(u64), __pad, st_ino@12, st_mode@16(u32),
   st_nlink@20, st_uid@24, st_gid@28, st_rdev@32(u64). */
static int fstat_stub(int fd, void* st){
  out("miniloader: fstat(fd="); out_dec(fd); out(")");
  if (fd != 42){ out(" -> -1\n"); return -1; }
  u8* p = (u8*)st; memset(p, 0, 128);
  *(u32*)(p+16) = 0x21B6u;  /* S_IFCHR|0666 */
  *(u32*)(p+20) = 1;
  out(" -> char device\n");
  return 0;
}
static void* mmap_stub(void* a, usize len, int prot, int flags, int fd, long off){
  out("miniloader: mmap(len="); out_dec((long)len); out(" prot="); out_dec(prot);
  out(" flags="); out_dec(flags); out(" fd="); out_dec(fd); out(") -> -1\n");
  (void)a;(void)off;
  if (fd == 42){ void* r = bump(len ? len : 4096); out("  -> fake device mapping "); out_hex((u32)r); out("\n"); return r; }
  return (void*)-1;
}
static int fcntl_stub(int fd, int cmd, long arg){ out("miniloader: fcntl(fd="); out_dec(fd); out(" cmd="); out_dec(cmd); out(")\n"); (void)arg; return 0; }
static int poll_stub(void* p, unsigned long n, int t){ out("miniloader: poll(\n"); (void)p;(void)n;(void)t; return 0; }
static int close_stub(int fd){ out("miniloader: close(fd="); out_dec(fd); out(")\n"); return 0; }
static int retm1(void){ return -1; }
static void noop(void){}
static u32* errno_ptr(void){ return &g_errno; }
static int log_stub(int prio, const char* tag, const char* fmt, ...){
  (void)prio;
  out("miniloader: log["); out(tag ? tag : "?"); out("] "); out(fmt ? fmt : "?"); out("\n"); return 0; }
static int log_write_stub(int prio, const char* tag, const char* text){
  (void)prio;
  out("miniloader: logw["); out(tag ? tag : "?"); out("] "); out(text ? text : "?"); out("\n"); return 0; }
static void abort_stub(void){ out("miniloader: blob called abort()\n"); sys_exit(134); }
static u32 guard_word;

static void* ml_malloc(usize n){ return bump(n ? n : 1); }
static void* ml_calloc(usize a, usize b){ return bump((a? a:1) * (b? b:1)); }
static void* ml_realloc(void* p, usize n){ void* q = bump(n?n:1); if (p) memcpy(q, p, n); (void)p; return q; }
static void ml_free(void* p){ (void)p; }

static u32 once_state[64];
static int ml_pthread_once(u32* flag, void (*fn)(void)){
  /* single-threaded: run the initialiser once */
  if (*flag == 0){ *flag = 1; fn(); }
  return 0;
}
static int ml_pthread_create(void* a, void* b, void* c, void* d){
  out("miniloader: pthread_create() -> -1 (no threads in miniloader)\n");
  (void)a;(void)b;(void)c;(void)d; return -1; }
static void* ml_dlopen(const char* name, int flags){
  out("miniloader: dlopen(\""); out(name ? name : "(null)"); out("\") -> 0\n"); (void)flags; return 0; }
static u32 ml_pthread_self(void){ return 1; }

static int g_uk_calls;
static int open_stub(const char* path, int flags, int mode){
  (void)flags; (void)mode;
  out("miniloader: open(\""); out(path ? path : "(null)"); out("\", flags="); out_dec(flags);
  out(", caller="); out_hex((u32)__builtin_return_address(0)); out(")\n");
  if (path && !strcmp(path, "/dev/mali0")){
    out("miniloader: blob opened /dev/mali0 -> fake fd 42\n");
    return 42;
  }
  return -1;
}
static const char* uk_name(u32 id){
  switch (id){
    case 0: return "UKP_FUNC_ID_CHECK_VERSION";
    case 528: return "KBASE_FUNC_GET_VERSION";
    case 530: return "KBASE_FUNC_SET_FLAGS";
    case 549: return "KBASE_FUNC_HWCNT_UTIL_SETUP(r15p0)/DESTROY_SURFACE(r7p0)";
    case 553: return "KBASE_FUNC_CREATE_SURFACE(r15p0 id)";
    default: return "?";
  }
}
static int ioctl_stub(int fd, unsigned long req, void* arg){
  u32* a = (u32*)arg;
  u32 size = (u32)((req >> 16) & 0x3fff);
  if (fd != 42) return -1;
  g_uk_calls++;
  out("miniloader: ioctl(42, cmd="); out_hex((u32)req); out(", size="); out_dec((long)size);
  out(", caller="); out_hex((u32)__builtin_return_address(0));
  if (a){
    out(") hdr{id="); out_dec((long)a[0]); out("("); out(uk_name(a[0])); out(") ret="); out_dec((long)a[1]); out("}");
    if (g_uk_calls < 12){
      out(" words:"); for (int i = 2; i < 8; i++){ out(" "); out_hex(a[i]); }
    }
  }
  out("\n");
  /* Minimal kbase emulation: answer the mandatory handshake so the blob
     proceeds further; everything else is refused. */
  switch (a ? a[0] : 0xffffffffu){
    case 0:   /* CHECK_VERSION: negotiate 10.0 as the kernel would for this client */
      a[1] = 0;
      if (size >= 12){ a[2] = 10 | ((a[2] >> 16) << 16); }
      return 0;
    case 530: /* SET_FLAGS */
      a[1] = 0; return 0;
    case 528: /* GET_VERSION */
      a[1] = 0;
      { char* v = (char*)&a[2]; const char* s = "K:r15p0-00rel0(GPL)"; int i = 0; for (; s[i]; i++) v[i] = s[i]; v[i] = 0; }
      return 0;
    default:
      a[1] = 0; /* pretend success; we only care which UK ids are used */
      return 0;
  }
}

typedef struct { const char* name; void* addr; int is_data; } Stub;

static Stub g_stubs[] = {
  {"__stack_chk_guard", &guard_word, 1},
  {"__stack_chk_fail", (void*)abort_stub, 0},
  {"__errno", (void*)errno_ptr, 0},
  {"malloc", (void*)ml_malloc, 0},
  {"calloc", (void*)ml_calloc, 0},
  {"realloc", (void*)ml_realloc, 0},
  {"free", (void*)ml_free, 0},
  {"__android_log_print", (void*)log_stub, 0},
  {"__android_log_write", (void*)log_write_stub, 0},
  {"abort", (void*)abort_stub, 0},
  {"__cxa_atexit", (void*)ret0, 0},
  {"__cxa_finalize", (void*)noop, 0},
  {"__register_atfork", (void*)ret0, 0},
  {"__aeabi_atexit", (void*)ret0, 0},
  {"__aeabi_memcpy", (void*)memcpy, 0},
  {"__aeabi_memmove", (void*)memmove, 0},
  {"__aeabi_memset", (void*)memset, 0},
  {"__cxa_guard_acquire", (void*)ret1, 0},
  {"__cxa_guard_release", (void*)ret0, 0},
  {"__cxa_guard_abort", (void*)ret0, 0},
  {"__cxa_pure_virtual", (void*)abort_stub, 0},
  {"pthread_mutex_lock", (void*)ret0, 0},
  {"pthread_mutex_unlock", (void*)ret0, 0},
  {"pthread_mutex_trylock", (void*)ret0, 0},
  {"pthread_mutex_init", (void*)ret0, 0},
  {"pthread_mutex_destroy", (void*)ret0, 0},
  {"pthread_cond_wait", (void*)ret0, 0},
  {"pthread_cond_signal", (void*)ret0, 0},
  {"pthread_cond_broadcast", (void*)ret0, 0},
  {"pthread_cond_init", (void*)ret0, 0},
  {"pthread_cond_destroy", (void*)ret0, 0},
  {"pthread_once", (void*)ml_pthread_once, 0},
  {"pthread_create", (void*)ml_pthread_create, 0},
  {"dlopen", (void*)ml_dlopen, 0},
  {"android_dlopen_ext", (void*)ml_dlopen, 0},
  {"dlclose", (void*)ret0, 0},
  {"dlsym", (void*)ret0, 0},
  {"pthread_join", (void*)ret0, 0},
  {"pthread_self", (void*)ml_pthread_self, 0},
  {"pthread_key_create", (void*)ret0, 0},
  {"pthread_setspecific", (void*)ret0, 0},
  {"pthread_getspecific", (void*)ret0, 0},
  {"pthread_rwlock_rdlock", (void*)ret0, 0},
  {"pthread_rwlock_wrlock", (void*)ret0, 0},
  {"pthread_rwlock_unlock", (void*)ret0, 0},
  {"sem_init", (void*)ret0, 0},
  {"sem_destroy", (void*)ret0, 0},
  {"sem_post", (void*)ret0, 0},
  {"sem_wait", (void*)ret0, 0},
  {"sem_timedwait", (void*)ret0, 0},
  {"hw_get_module", (void*)ret0, 0},
  {"ion_open", (void*)ret0, 0},
  {"ion_close", (void*)ret0, 0},
  {"ion_alloc", (void*)ret0, 0},
  {"ion_alloc_fd", (void*)ret0, 0},
  {"ion_free", (void*)ret0, 0},
  {"ion_map", (void*)ret0, 0},
  {"ion_share", (void*)ret0, 0},
  {"open", (void*)open_stub, 0},
  {"openat", (void*)open_stub, 0},
  {"ioctl", (void*)ioctl_stub, 0},
  {"mmap", (void*)mmap_stub, 0},
  {"mmap64", (void*)mmap_stub, 0},
  {"munmap", (void*)retm1_keep, 0},
  {"fstat", (void*)fstat_stub, 0},
  {"lseek", (void*)retm1_keep, 0},
  {"fcntl", (void*)fcntl_stub, 0},
  {"poll", (void*)poll_stub, 0},
  {"close", (void*)close_stub, 0},
  {"__progname", &g_zero_obj[0], 1},
  {"environ", &g_zero_obj[4], 1},
  {"stdout", &g_zero_obj[8], 1},
  {"stderr", &g_zero_obj[9], 1},
  {0, 0, 0}
};

static void* stub_for(const char* name, int type){
  for (Stub* s = g_stubs; s->name; s++) if (!strcmp(s->name, name)) return s->addr;
  if (type == 1 /*STT_OBJECT*/) return bump(64);
  return (void*)ret0;
}

/* ---------------- image ---------------- */
static u32 g_base = BASE;
static u32 g_dynsym, g_dynstr, g_symcount;
static u32 g_rel_plt, g_rel_plt_sz;
static u32 g_android_rel, g_android_rel_sz;
static u32 g_dyn;
static u32 g_init_array, g_init_array_sz, g_init_func;
static const u8* g_file;
static usize g_file_size;

static Sym* sym_at(u32 i){ return (Sym*)(g_base + g_dynsym + i*16); }
static const char* sym_name(Sym* s){ return (const char*)(g_base + g_dynstr + s->name); }

static u32 resolve(u32 idx){
  if (idx == 0) return 0;
  Sym* s = sym_at(idx);
  if (s->shndx != 0){
    if (s->shndx == 0xfff1) return s->value;
    return g_base + s->value;
  }
  return (u32)stub_for(sym_name(s), s->info & 0xf);
}

static u32 apply_one(u32 off, u32 info, u32* counts){
  u32 type = info & 0xff, sym = info >> 8;
  u32* loc = (u32*)(g_base + off);
  switch (type){
    case 0:  break;                                   /* R_ARM_NONE */
    case 23: *loc += g_base; counts[0]++; break;       /* RELATIVE */
    case 2:  *loc = *loc + resolve(sym); counts[1]++; break; /* ABS32 */
    case 21: *loc = resolve(sym); counts[2]++; break;  /* GLOB_DAT */
    case 22: *loc = resolve(sym); counts[3]++; break;  /* JUMP_SLOT */
    default: counts[4]++; break;
  }
  return type;
}

static i32 g_sleb_pos;
static u32 sleb(const u8* p, const u8* end, long* out){
  u32 r = 0; int shift = 0; u8 b;
  do { if (p + g_sleb_pos >= end) return 0; b = p[g_sleb_pos++];
       r |= (u32)(b & 0x7f) << shift; shift += 7; } while (b & 0x80);
  if (shift < 32 && (b & 0x40)) r |= ~0u << shift;
  *out = (long)(i32)r;
  return 1;
}

static void relocate_aps2(const u8* p, const u8* end, u32* counts){
  g_sleb_pos = 0;
  if (end - p < 8 || p[0] != 'A' || p[1] != 'P' || p[2] != 'S' || p[3] != '2'){
    out("miniloader: bad APS2 magic\n"); return;
  }
  g_sleb_pos = 4;
  long count, roff; u32 info = 0;
  if (!sleb(p,end,&count) || !sleb(p,end,&roff)) return;
  long produced = 0;
  while (produced < count){
    long gsize, flags, gd = 0;
    if (!sleb(p,end,&gsize) || !sleb(p,end,&flags)) break;
    if (flags & 2) if (!sleb(p,end,&gd)) break;
    if (flags & 1) { long v; if (!sleb(p,end,&v)) break; info = (u32)v; }
    for (long i = 0; i < gsize; i++){
      long d;
      if (flags & 2) roff += gd; else { if (!sleb(p,end,&d)) return; roff += d; }
      if (!(flags & 1)) { long v; if (!sleb(p,end,&v)) return; info = (u32)v; }
      apply_one((u32)roff, info, counts);
      produced++;
    }
  }
  out("miniloader: APS2 relocs produced="); out_dec(produced);
  out(" expected="); out_dec(count); out("\n");
}

static void relocate_plain(u32 off, u32 size, u32* counts){
  for (u32 i = 0; i < size; i += 8){
    u32 r_off = rd32((const u8*)(g_base + off + i));
    u32 r_info = rd32((const u8*)(g_base + off + i + 4));
    apply_one(r_off, r_info, counts);
  }
}

/* ---------------- ELF load ---------------- */
static int load(const char* path, int run_ctors){
  long fd = sc4(SYS_openat, AT_FDCWD, (long)path, 0 /*O_RDONLY*/, 0);
  if (fd < 0){ out("miniloader: cannot open "); out(path); out("\n"); return 0; }
  u32 fbuf = (u32)sc6(SYS_mmap2, 0, MAX_IMAGE, 3 /*RW*/, 0x22, -1, 0);
  if ((long)fbuf < 0){ out("miniloader: file mmap failed\n"); return 0; }
  usize total = 0; long n;
  while ((n = sc3(SYS_read, fd, (long)(fbuf+total), (long)(MAX_IMAGE-total))) > 0){
    total += (usize)n; if (total >= MAX_IMAGE) break;
  }
  sc1(SYS_close, fd);
  g_file = (const u8*)fbuf; g_file_size = total;
  if (total < 0x40 || g_file[0] != 0x7f || g_file[1] != 'E'){ out("miniloader: not an ELF\n"); return 0; }
  if (g_file[4] != 1 || g_file[5] != 1){ out("miniloader: not ELF32 LSB\n"); return 0; }

  u32 phoff = rd32(g_file+E_PHOFF), phentsize = g_file[E_PHENTSIZE] | (g_file[E_PHENTSIZE+1]<<8);
  u32 phnum = g_file[E_PHNUM] | (g_file[E_PHNUM+1]<<8);
  u32 shoff = rd32(g_file+E_SHOFF), shentsize = g_file[E_SHENTSIZE] | (g_file[E_SHENTSIZE+1]<<8);
  u32 shnum = g_file[E_SHNUM] | (g_file[E_SHNUM+1]<<8);
  u32 maxend = 0; u32 dynoff = 0, dynsz = 0;

  for (u32 i = 0; i < phnum; i++){
    const u8* ph = g_file + phoff + i*phentsize;
    u32 type = rd32(ph), off = rd32(ph+4), va = rd32(ph+8), fsz = rd32(ph+16), msz = rd32(ph+20);
    if (type == 1 && va + msz > maxend) maxend = va + msz;
    if (type == 2){ dynoff = off; dynsz = fsz; }
  }
  if (!maxend){ out("miniloader: no PT_LOAD\n"); return 0; }

  u32 map = (u32)sc6(SYS_mmap2, BASE, (maxend + 0xfff) & ~0xfffu, 7 /*RWX*/, 0x32 /*PRIVATE|ANON|FIXED*/, -1, 0);
  if ((long)map < 0 || map != BASE){ out("miniloader: image mmap failed\n"); return 0; }
  memset((void*)BASE, 0, (maxend + 0xfff) & ~0xfffu);

  for (u32 i = 0; i < phnum; i++){
    const u8* ph = g_file + phoff + i*phentsize;
    u32 type = rd32(ph), off = rd32(ph+4), va = rd32(ph+8), fsz = rd32(ph+16);
    if (type != 1) continue;
    memcpy((void*)(BASE + va), g_file + off, fsz);
  }

  /* dynamic tags */
  for (u32 o = 0; o < dynsz; o += 8){
    u32 tag = rd32(g_file + dynoff + o), val = rd32(g_file + dynoff + o + 4);
    if (tag == 0) break;
    switch (tag){
      case 6:  g_dynsym = val; break;
      case 5:  g_dynstr = val; break;
      case 12: g_init_func = val; break;
      case 25: g_init_array = val; break;
      case 27: g_init_array_sz = val; break;
      case 23: g_rel_plt = val; break;
      case 2:  g_rel_plt_sz = val; break;
      case 0x6000000fu: g_android_rel = val; break;
      case 0x60000010u: g_android_rel_sz = val; break;
      default: break;
    }
  }
  (void)g_dyn;

  /* dynsym count from the section headers (SHT_DYNSYM = 11) */
  for (u32 i = 0; i < shnum; i++){
    const u8* sh = g_file + shoff + i*shentsize;
    if (rd32(sh+4) == 11 && rd32(sh+20)) { g_symcount = rd32(sh+20) / 16; break; }
  }
  out("miniloader: image @"); out_hex(BASE);
  out(" maxend "); out_hex(maxend);
  out(" dynsym "); out_dec(g_symcount);
  out(" aps2 "); out_hex(g_android_rel); out("+"); out_hex(g_android_rel_sz);
  out(" plt "); out_hex(g_rel_plt); out("+"); out_hex(g_rel_plt_sz); out("\n");

  u32 counts[5] = {0,0,0,0,0};
  if (g_android_rel && g_android_rel_sz)
    relocate_aps2((const u8*)(g_base + g_android_rel), (const u8*)(g_base + g_android_rel + g_android_rel_sz), counts);
  if (g_rel_plt && g_rel_plt_sz) relocate_plain(g_rel_plt, g_rel_plt_sz, counts);
  out("miniloader: relocs RELATIVE="); out_dec(counts[0]);
  out(" ABS32="); out_dec(counts[1]);
  out(" GLOB_DAT="); out_dec(counts[2]);
  out(" JUMP_SLOT="); out_dec(counts[3]);
  out(" other="); out_dec(counts[4]); out("\n");
  if (!run_ctors) { out("miniloader: constructors skipped (pass 'ctors' as 2nd arg to run)\n"); return 1; }
  if (g_init_func){ out("miniloader: DT_INIT @"); out_hex(g_base + g_init_func); out("\n");
    ((void(*)(void))(g_base + g_init_func))(); }
  if (g_init_array && g_init_array_sz){
    u32 n = g_init_array_sz / 4;
    out("miniloader: constructors="); out_dec(n); out("\n");
    for (u32 i = 0; i < n; i++){
      u32 fn = *(u32*)(g_base + g_init_array + i*4);
      if (!fn) continue;
      out("  ctor["); out_dec(i); out("]="); out_hex(fn); out("\n");
      ((void(*)(void))fn)();
    }
    out("miniloader: constructors done\n");
  }
  return 1;
}

static u32 find_export(const char* name){
  for (u32 i = 1; i < g_symcount; i++){
    Sym* s = sym_at(i);
    if (s->shndx && s->name && !strcmp(sym_name(s), name)) return g_base + s->value;
  }
  return 0;
}

static char* as_str(u32 ptr){ return (char*)ptr; }

int main(int argc, char** argv){
  const char* path = (argc > 1) ? argv[1] : "libGLES_mali.so";
  int run_ctors = (argc > 2 && !strcmp(argv[2], "ctors"));
  out("miniloader: loading "); out(path); out("\n");
  if (!load(path, run_ctors)) return 1;

  u32 p_getdisp = find_export("eglGetDisplay");
  u32 p_query   = find_export("eglQueryString");
  u32 p_getproc = find_export("eglGetProcAddress");
  u32 p_init    = find_export("eglInitialize");
  out("exports eglGetDisplay="); out_hex(p_getdisp);
  out(" eglQueryString="); out_hex(p_query);
  out(" eglGetProcAddress="); out_hex(p_getproc);
  out(" eglInitialize="); out_hex(p_init); out("\n");
  if (!p_getdisp || !p_query) { out("miniloader: exports missing\n"); return 1; }

  void* (*get_display)(void*) = (void* (*)(void*))p_getdisp;
  const char* (*query)(void*, int) = (const char* (*)(void*, int))p_query;

  void* dpy = get_display((void*)0);
  out("eglGetDisplay(EGL_DEFAULT_DISPLAY) = "); out_hex((u32)dpy); out("\n");

  const char* ca = query(0, 0x308D); /* EGL_CLIENT_APIS */
  out("eglQueryString(NO_DISPLAY, EGL_CLIENT_APIS) = ");
  out(ca ? as_str((u32)ca) : "(null)"); out("\n");

  const char* ext = query(0, 0x3055); /* EGL_EXTENSIONS */
  out("eglQueryString(NO_DISPLAY, EGL_EXTENSIONS)[0..63] = ");
  if (ext){ char buf[65]; memcpy(buf, ext, 64); buf[64]=0; out(as_str((u32)buf)); } else out("(null)");
  out("\n");

  if (dpy){
    const char* (*qd)(void*, int) = query;
    const char* v = qd(dpy, 0x3053); /* EGL_VENDOR */
    out("eglQueryString(dpy, EGL_VENDOR) = "); out(v ? as_str((u32)v) : "(null)"); out("\n");
    const char* ver = qd(dpy, 0x3054); /* EGL_VERSION */
    out("eglQueryString(dpy, EGL_VERSION) = "); out(ver ? as_str((u32)ver) : "(null)"); out("\n");
  }

  if (p_getproc){
    void* (*getproc)(const char*) = (void* (*)(const char*))p_getproc;
    void* gp = getproc("glGetString");
    void* gd = getproc("eglGetDisplay");
    out("eglGetProcAddress(glGetString)="); out_hex((u32)gp);
    out(" eglGetDisplay="); out_hex((u32)gd); out("\n");
  }
  if (p_init){
    int (*initialize)(void*, int*, int*) = (int (*)(void*, int*, int*))p_init;
    int maj = -1, min = -1;
    int ok = initialize(dpy, &maj, &min);
    out("eglInitialize(dpy) = "); out_dec(ok);
    out(" version "); out_dec(maj); out("."); out_dec(min); out("\n");
    u32 pe = find_export("eglGetError");
    if (pe){ int (*gerr)(void) = (int (*)(void))pe; out("eglGetError() = 0x"); out_hex((u32)gerr()); out("\n"); }
  }
  out("miniloader: done (no kernel driver touched, no acceleration claimed)\n");
  return 0;
}
