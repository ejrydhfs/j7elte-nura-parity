/* Freestanding ARM32 shim: enough libc for a diagnostic ELF loader. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long usize;

#define SYS_exit 1
#define SYS_write 4
#define SYS_read 3
#define SYS_openat 322
#define SYS_close 6
#define SYS_mmap2 192
#define SYS_ioctl 54
#define AT_FDCWD (-100)

static inline long sc0(long n){ register long r7 asm("r7")=n; register long r0 asm("r0")=0;
  asm volatile("svc #0":"+r"(r0):"r"(r7):"memory"); return r0; }
static inline long sc1(long n,long a){ register long r7 asm("r7")=n; register long r0 asm("r0")=a;
  asm volatile("svc #0":"+r"(r0):"r"(r7):"memory"); return r0; }
static inline long sc3(long n,long a,long b,long c){ register long r7 asm("r7")=n;
  register long r0 asm("r0")=a; register long r1 asm("r1")=b; register long r2 asm("r2")=c;
  asm volatile("svc #0":"+r"(r0):"r"(r1),"r"(r2),"r"(r7):"memory"); return r0; }
static inline long sc4(long n,long a,long b,long c,long d){ register long r7 asm("r7")=n;
  register long r0 asm("r0")=a; register long r1 asm("r1")=b; register long r2 asm("r2")=c; register long r3 asm("r3")=d;
  asm volatile("svc #0":"+r"(r0):"r"(r1),"r"(r2),"r"(r3),"r"(r7):"memory"); return r0; }
static inline long sc6(long n,long a,long b,long c,long d,long e,long f){ register long r7 asm("r7")=n;
  register long r0 asm("r0")=a; register long r1 asm("r1")=b; register long r2 asm("r2")=c; register long r3 asm("r3")=d;
  register long r4 asm("r4")=e; register long r5 asm("r5")=f;
  asm volatile("svc #0":"+r"(r0):"r"(r1),"r"(r2),"r"(r3),"r"(r4),"r"(r5),"r"(r7):"memory"); return r0; }

static long sys_write(int fd, const void* p, usize n){ return sc3(SYS_write, fd, (long)p, (long)n); }
static void sys_exit(int c){ sc1(SYS_exit, c); for(;;){} }

/* ---- tiny string/mem ---- */
void* memcpy(void* d, const void* s, usize n){ u8* a=d; const u8* b=s; while(n--) *a++=*b++; return d; }
void* memmove(void* d, const void* s, usize n){ u8* a=d; const u8* b=s;
  if(a<b){ while(n--) *a++=*b++; } else { a+=n; b+=n; while(n--) *--a=*--b; } return d; }
void* memset(void* d, int c, usize n){ u8* a=d; while(n--) *a++=(u8)c; return d; }
int memcmp(const void* a, const void* b, usize n){ const u8* x=a; const u8* y=b;
  while(n--){ if(*x!=*y) return (int)*x-(int)*y; x++; y++; } return 0; }
usize strlen(const char* s){ const char* p=s; while(*p) p++; return (usize)(p-s); }
int strcmp(const char* a, const char* b){ while(*a && *a==*b){a++;b++;} return (int)(u8)*a-(int)(u8)*b; }
int strncmp(const char* a, const char* b, usize n){ while(n && *a && *a==*b){a++;b++;n--;} if(!n) return 0; return (int)(u8)*a-(int)(u8)*b; }

/* ---- output ---- */
static void out(const char* s){ sys_write(2, s, strlen(s)); }
static void out_hex(u32 v){ char b[11]; b[0]='0'; b[1]='x';
  for(int i=0;i<8;i++){ u32 d=(v>>((7-i)*4))&0xf; b[2+i]= d<10 ? (char)('0'+d) : (char)('a'+d-10); }
  b[10]='\0'; out(b); }
static void out_dec(long v){ char b[24]; int i=23; b[i--]='\0'; int neg=v<0; unsigned long u= neg? (unsigned long)(-v):(unsigned long)v;
  if(!u) b[i--]='0'; while(u){ b[i--]=(char)('0'+(u%10)); u/=10; } if(neg) b[i--]='-'; out(&b[i+1]); }
