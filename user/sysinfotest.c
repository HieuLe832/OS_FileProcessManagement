#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "kernel/fcntl.h"
#include "kernel/sysinfo.h"
#include "user/user.h"

// Same order as the type enum of struct file (kernel/file.h).
enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE };
// Same order as enum procstate (kernel/proc.h).
enum { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

#define NSPIN 4      // spinning children, more than the 3 CPUs of make qemu
#define NCHILD 5     // children forked by the "many children" test
#define NFILLER 10   // children used to fill the system-wide file table

// struct sysinfo is ~38KB and the user stack is one page,
// so snapshots must be global.
struct sysinfo before, after;

char *testfile = "sitest.tmp";   // contains "abcdef"
int mypid;
int npass, nfail;

//
// lookup helpers
//

struct sysinfo_proc*
findproc(struct sysinfo *s, int pid)
{
  for(int i = 0; i < s->nproc; i++)
    if(s->procs[i].pid == pid)
      return &s->procs[i];
  return 0;
}

struct sysinfo_file*
findfile(struct sysinfo *s, int idx)
{
  for(int i = 0; i < s->nfile; i++)
    if(s->files[i].idx == idx)
      return &s->files[i];
  return 0;
}

// Index in ftable of the file behind fd of process pid, -1 if not open.
int
fdidx(struct sysinfo *s, int pid, int fd)
{
  struct sysinfo_proc *p = findproc(s, pid);

  if(p == 0)
    return -1;
  for(int i = 0; i < p->nfd; i++)
    if(p->fds[i] == fd)
      return p->fidx[i];
  return -1;
}

struct sysinfo_file*
fdfile(struct sysinfo *s, int pid, int fd)
{
  return findfile(s, fdidx(s, pid, fd));
}

// Number of fds of p that point to file idx.
int
holds(struct sysinfo_proc *p, int idx)
{
  int n = 0;

  for(int i = 0; i < p->nfd; i++)
    if(p->fidx[i] == idx)
      n++;
  return n;
}

int
haspid(struct sysinfo_file *f, int pid)
{
  for(int i = 0; i < f->npids; i++)
    if(f->pids[i] == pid)
      return 1;
  return 0;
}

// State of pid, UNUSED if it is not listed.
int
stateof(struct sysinfo *s, int pid)
{
  struct sysinfo_proc *p = findproc(s, pid);
  return p ? p->state : UNUSED;
}

int
nschedof(struct sysinfo *s, int pid)
{
  struct sysinfo_proc *p = findproc(s, pid);
  return p ? p->nsched : -1;
}

int
nfdof(struct sysinfo *s, int pid)
{
  struct sysinfo_proc *p = findproc(s, pid);
  return p ? p->nfd : -1;
}

//
// printing helpers (xv6 printf has no field width)
//

char*
typestr(int t)
{
  static char *names[] = { "NONE", "PIPE", "INODE", "DEVICE" };
  return (t >= FD_NONE && t <= FD_DEVICE) ? names[t] : "?";
}

char*
statestr(int st)
{
  static char *names[] = { "UNUSED", "USED", "SLEEPING", "RUNNABLE", "RUNNING", "ZOMBIE" };
  return (st >= UNUSED && st <= ZOMBIE) ? names[st] : "?";
}

char*
modestr(struct sysinfo_file *f)
{
  if(f->readable && f->writable)
    return "rw";
  if(f->readable)
    return "r-";
  if(f->writable)
    return "-w";
  return "--";
}

void
col(char *s, int w)
{
  printf("%s", s);
  for(int n = strlen(s); n < w; n++)
    printf(" ");
}

void
coli(int v, int w)
{
  char buf[16];
  int i = sizeof(buf) - 1;
  int neg = v < 0;

  buf[i] = 0;
  if(neg)
    v = -v;
  do {
    buf[--i] = '0' + v % 10;
    v /= 10;
  } while(v);
  if(neg)
    buf[--i] = '-';
  col(buf + i, w);
}

// Print the open file table; if pid != 0 only the files used by pid.
void
print_files(struct sysinfo *s, int pid)
{
  printf("  open files: %d\n", s->nfile);
  printf("    idx  type    mode  ref  pids\n");
  for(int i = 0; i < s->nfile; i++){
    struct sysinfo_file *f = &s->files[i];
    if(pid && !haspid(f, pid))
      continue;
    printf("    ");
    coli(f->idx, 5);
    col(typestr(f->type), 8);
    col(modestr(f), 6);
    coli(f->ref, 5);
    for(int j = 0; j < f->npids; j++)
      printf("%d ", f->pids[j]);
    printf("\n");
  }
}

void
print_procs(struct sysinfo *s)
{
  printf("  processes: %d\n", s->nproc);
  printf("    pid  ppid  state     nsched  name          fd->idx\n");
  for(int i = 0; i < s->nproc; i++){
    struct sysinfo_proc *p = &s->procs[i];
    printf("    ");
    coli(p->pid, 5);
    coli(p->ppid, 6);
    col(statestr(p->state), 10);
    coli(p->nsched, 8);
    col(p->name, 14);
    for(int j = 0; j < p->nfd; j++)
      printf("%d->%d ", p->fds[j], p->fidx[j]);
    printf("\n");
  }
}

//
// checking helpers: every check prints one PASS/FAIL line
//

void
result(int ok)
{
  if(ok)
    npass++;
  else
    nfail++;
  printf(ok ? "  [PASS] " : "  [FAIL] ");
}

void
check(int ok, char *msg)
{
  result(ok);
  printf("%s\n", msg);
}

void
checki(char *msg, int got, int want)
{
  result(got == want);
  printf("%s = %d", msg, got);
  if(got != want)
    printf(" (expected %d)", want);
  printf("\n");
}

void
checkst(char *msg, int got, int want)
{
  result(got == want);
  printf("%s = %s", msg, statestr(got));
  if(got != want)
    printf(" (expected %s)", statestr(want));
  printf("\n");
}

// Check type, mode and ref count of the file behind fd of this process.
void
checkfd(struct sysinfo *s, int fd, int type, char *mode, int ref)
{
  struct sysinfo_file *f = fdfile(s, mypid, fd);
  int ok = f && f->type == type && strcmp(modestr(f), mode) == 0 && f->ref == ref;

  result(ok);
  printf("fd %d -> ", fd);
  if(f)
    printf("idx %d %s %s ref %d", f->idx, typestr(f->type), modestr(f), f->ref);
  else
    printf("not open");
  if(!ok)
    printf(" (expected %s %s ref %d)", typestr(type), mode, ref);
  printf("\n");
}

// Invariants that every snapshot must satisfy: values in range and
// the file list and the process list point to each other.
int
consistent(struct sysinfo *s)
{
  int ok = 1;

  if(s->nfile < 0 || s->nfile > NFILE || s->nproc < 0 || s->nproc > NPROC){
    printf("    nfile %d or nproc %d out of range\n", s->nfile, s->nproc);
    return 0;
  }
  for(int i = 0; i < s->nfile; i++){
    struct sysinfo_file *f = &s->files[i];
    if(f->idx < 0 || f->idx >= NFILE || (i > 0 && f->idx <= s->files[i-1].idx)){
      printf("    file %d: bad idx %d\n", i, f->idx);
      ok = 0;
    }
    if(f->type < FD_PIPE || f->type > FD_DEVICE || (!f->readable && !f->writable)){
      printf("    file idx %d: bad type %d or mode\n", f->idx, f->type);
      ok = 0;
    }
    if(f->ref < 1 || f->npids < 0 || f->npids > f->ref){
      printf("    file idx %d: ref %d npids %d\n", f->idx, f->ref, f->npids);
      ok = 0;
    }
    for(int j = 0; j < f->npids; j++){
      struct sysinfo_proc *p = findproc(s, f->pids[j]);
      if(p == 0 || holds(p, f->idx) == 0){
        printf("    file idx %d lists pid %d, which has no fd to it\n", f->idx, f->pids[j]);
        ok = 0;
      }
    }
  }
  for(int i = 0; i < s->nproc; i++){
    struct sysinfo_proc *p = &s->procs[i];
    if(p->state < USED || p->state > ZOMBIE || p->nfd < 0 || p->nfd > NOFILE){
      printf("    pid %d: bad state %d or nfd %d\n", p->pid, p->state, p->nfd);
      ok = 0;
      continue;
    }
    for(int j = 0; j < p->nfd; j++){
      struct sysinfo_file *f = findfile(s, p->fidx[j]);
      if(p->fds[j] < 0 || p->fds[j] >= NOFILE || (j > 0 && p->fds[j] <= p->fds[j-1])){
        printf("    pid %d: bad fd %d\n", p->pid, p->fds[j]);
        ok = 0;
      }
      if(f == 0 || !haspid(f, p->pid)){
        printf("    pid %d fd %d -> idx %d, not listed or pid missing\n",
               p->pid, p->fds[j], p->fidx[j]);
        ok = 0;
      }
    }
  }
  return ok;
}

// Every reference to a struct file comes from some ofile[] slot,
// so when nothing is opening or closing files, ref must equal
// the number of fds pointing to the file.
int
refs_match(struct sysinfo *s)
{
  int ok = 1;

  for(int i = 0; i < s->nfile; i++){
    struct sysinfo_file *f = &s->files[i];
    int n = 0;
    for(int j = 0; j < s->nproc; j++)
      n += holds(&s->procs[j], f->idx);
    if(n != f->ref){
      printf("    file idx %d: ref %d but %d fds point to it\n", f->idx, f->ref, n);
      ok = 0;
    }
  }
  return ok;
}

//
// process helpers
//

void
snap(struct sysinfo *s)
{
  if(sysinfo(s) < 0){
    printf("sysinfotest: sysinfo failed\n");
    exit(1);
  }
}

int
xfork(void)
{
  int pid = fork();

  if(pid < 0){
    printf("sysinfotest: fork failed\n");
    exit(1);
  }
  return pid;
}

// Fork a child that blocks in read(fd) and exits once a byte arrives.
// It keeps every inherited fd, so its fd table equals the parent's.
int
fork_waiter(int fd)
{
  int pid = xfork();
  char c;

  if(pid == 0){
    read(fd, &c, 1);
    exit(0);
  }
  return pid;
}

// Take snapshots into s until pid is in state st (UNUSED = gone).
// Returns 0 if that does not happen within ~100 ticks.
// Polls without sleep(): a child in sleep() is woken at the same tick
// as we would be, so after our own sleep() it is always RUNNABLE.
int
waitstate(struct sysinfo *s, int pid, int st)
{
  int start = uptime();

  while(uptime() - start < 100){
    snap(s);
    if(stateof(s, pid) == st)
      return 1;
  }
  return 0;
}

//
// tests
//

void
test_baseline(void)
{
  struct sysinfo_proc *me, *init, *sh;
  struct sysinfo_file *con;

  snap(&before);
  print_files(&before, 0);
  print_procs(&before);

  check(consistent(&before), "file list and process list agree with each other");
  check(refs_match(&before), "ref of every file = number of fds pointing to it");

  me = findproc(&before, mypid);
  check(me != 0, "sysinfotest is listed");
  if(me == 0)
    return;
  checkst("state of sysinfotest", me->state, RUNNING);
  check(strcmp(me->name, "sysinfotest") == 0, "name of sysinfotest is \"sysinfotest\"");

  init = findproc(&before, 1);
  check(init && strcmp(init->name, "init") == 0, "pid 1 is init");
  if(init)
    checki("ppid of init", init->ppid, 0);

  sh = findproc(&before, me->ppid);
  check(sh && strcmp(sh->name, "sh") == 0, "parent of sysinfotest is sh");
  if(sh)
    checkst("state of sh (blocked in wait)", sh->state, SLEEPING);

  check(fdidx(&before, mypid, 0) == fdidx(&before, mypid, 1) &&
        fdidx(&before, mypid, 1) == fdidx(&before, mypid, 2),
        "fd 0, 1, 2 share one struct file (init used dup)");
  con = fdfile(&before, mypid, 0);
  check(con && con->type == FD_DEVICE && con->readable && con->writable,
        "fd 0 is the console: DEVICE, rw");
  check(con && haspid(con, 1) && haspid(con, mypid), "console is used by init and sysinfotest");
}

void
test_open(void)
{
  int fr, fw, frw, fdir, fdev;

  snap(&before);
  fr = open(testfile, O_RDONLY);
  fw = open(testfile, O_WRONLY);
  frw = open(testfile, O_RDWR);
  fdir = open(".", O_RDONLY);
  fdev = open("console", O_RDWR);
  snap(&after);
  print_files(&after, mypid);

  checki("nfile increase", after.nfile - before.nfile, 5);
  checki("nfd increase of sysinfotest", nfdof(&after, mypid) - nfdof(&before, mypid), 5);
  checkfd(&after, fr, FD_INODE, "r-", 1);
  checkfd(&after, fw, FD_INODE, "-w", 1);
  checkfd(&after, frw, FD_INODE, "rw", 1);
  checkfd(&after, fdir, FD_INODE, "r-", 1);
  checkfd(&after, fdev, FD_DEVICE, "rw", 1);
  check(fdidx(&after, mypid, fdev) != fdidx(&after, mypid, 0),
        "opening console again gives a new struct file, not the one of fd 0");
  check(fdfile(&after, mypid, fr) && fdfile(&after, mypid, fr)->npids == 1 &&
        fdfile(&after, mypid, fr)->pids[0] == mypid,
        "pids of a newly opened file = { sysinfotest }");

  close(fr);
  close(fw);
  close(frw);
  close(fdir);
  close(fdev);
}

void
test_open_twice(void)
{
  int a, b;
  char ca = 0, cb = 0;

  snap(&before);
  a = open(testfile, O_RDONLY);
  b = open(testfile, O_RDONLY);
  snap(&after);

  checki("nfile increase", after.nfile - before.nfile, 2);
  check(fdidx(&after, mypid, a) != fdidx(&after, mypid, b),
        "two open() of the same file give two struct file");
  checkfd(&after, a, FD_INODE, "r-", 1);
  checkfd(&after, b, FD_INODE, "r-", 1);

  read(a, &ca, 1);
  read(b, &cb, 1);
  check(ca == 'a' && cb == 'a', "offsets are independent (both fds read 'a')");

  close(a);
  close(b);
}

void
test_dup(void)
{
  int fd, fd2;
  char c1 = 0, c2 = 0;

  fd = open(testfile, O_RDONLY);
  snap(&before);
  fd2 = dup(fd);
  snap(&after);
  print_files(&after, mypid);

  checki("nfile increase", after.nfile - before.nfile, 0);
  check(fdidx(&after, mypid, fd) == fdidx(&after, mypid, fd2),
        "new fd points to the same struct file");
  checkfd(&after, fd, FD_INODE, "r-", 2);
  check(fdfile(&after, mypid, fd) && fdfile(&after, mypid, fd)->npids == 1,
        "sysinfotest is listed once in pids although it has 2 fds");

  read(fd, &c1, 1);
  read(fd2, &c2, 1);
  check(c1 == 'a' && c2 == 'b', "offset is shared (reads 'a' then 'b')");

  close(fd);
  close(fd2);
}

void
test_fork(void)
{
  int fd, p[2], pid, status, ok;
  char c = 0;
  struct sysinfo_proc *pp, *cp;
  struct sysinfo_file *fb, *fa;

  fd = open(testfile, O_RDONLY);
  pipe(p);
  snap(&before);
  pid = fork_waiter(p[0]);
  snap(&after);
  print_procs(&after);

  checki("nproc increase", after.nproc - before.nproc, 1);
  checki("nfile increase (fork creates no struct file)", after.nfile - before.nfile, 0);

  pp = findproc(&before, mypid);
  cp = findproc(&after, pid);
  check(cp != 0, "child is listed");
  if(cp && pp){
    checki("ppid of child", cp->ppid, mypid);
    check(strcmp(cp->name, "sysinfotest") == 0, "child inherits the name of the parent");
    checki("nfd of child", cp->nfd, pp->nfd);

    ok = cp->nfd == pp->nfd;
    for(int i = 0; ok && i < cp->nfd; i++)
      ok = cp->fds[i] == pp->fds[i] && cp->fidx[i] == pp->fidx[i];
    check(ok, "child has the same fds, pointing to the same struct file");

    // A file reached through k fds of the parent gains k refs.
    ok = 1;
    for(int i = 0; i < pp->nfd; i++){
      fb = findfile(&before, pp->fidx[i]);
      fa = findfile(&after, pp->fidx[i]);
      if(fb == 0 || fa == 0 || fa->ref != fb->ref + holds(pp, pp->fidx[i]) ||
         !haspid(fa, mypid) || !haspid(fa, pid))
        ok = 0;
    }
    check(ok, "each file gains one ref per inherited fd, pids has parent and child");
  }
  fb = fdfile(&before, mypid, 0);
  fa = fdfile(&after, mypid, 0);
  if(fb && fa)
    checki("console ref increase (fd 0, 1, 2 inherited)", fa->ref - fb->ref, 3);

  write(p[1], "x", 1);
  checki("wait() returns the child", wait(&status), pid);
  snap(&after);
  checki("nproc after wait", after.nproc, before.nproc);
  fa = fdfile(&after, mypid, 0);
  if(fb && fa)
    checki("console ref after child exit", fa->ref, fb->ref);

  // The child reads one byte through the inherited fd; the parent continues from there.
  pid = xfork();
  if(pid == 0){
    read(fd, &c, 1);
    exit(0);
  }
  wait(0);
  read(fd, &c, 1);
  check(c == 'b', "offset is shared with the child (child read 'a', parent reads 'b')");

  close(fd);
  close(p[0]);
  close(p[1]);
}

void
test_pipe(void)
{
  int p[2];

  snap(&before);
  pipe(p);
  snap(&after);
  print_files(&after, mypid);

  checki("nfile increase", after.nfile - before.nfile, 2);
  checkfd(&after, p[0], FD_PIPE, "r-", 1);
  checkfd(&after, p[1], FD_PIPE, "-w", 1);

  close(p[0]);
  close(p[1]);
}

void
test_close(void)
{
  int fd, fd2, idx;
  struct sysinfo_file *f;

  fd = open(testfile, O_RDONLY);
  fd2 = dup(fd);
  snap(&before);
  idx = fdidx(&before, mypid, fd);

  close(fd2);
  snap(&after);
  f = findfile(&after, idx);
  check(f != 0, "after closing one of two fds the struct file is still open");
  if(f)
    checki("ref", f->ref, 1);
  checki("fd count of sysinfotest decrease", nfdof(&before, mypid) - nfdof(&after, mypid), 1);

  close(fd);
  snap(&after);
  check(findfile(&after, idx) == 0, "after closing the last fd the struct file is freed");
  checki("nfile decrease", before.nfile - after.nfile, 1);
  checki("fd lookup of closed fd", fdidx(&after, mypid, fd), -1);
}

void
test_zombie(void)
{
  int pid, status = 0, ok;
  struct sysinfo_proc *cp;

  snap(&before);
  pid = xfork();
  if(pid == 0)
    exit(7);

  ok = waitstate(&after, pid, ZOMBIE);
  print_procs(&after);
  checkst("state of exited child before wait()", stateof(&after, pid), ZOMBIE);
  cp = findproc(&after, pid);
  if(ok && cp){
    checki("nfd of zombie (exit closed all fds)", cp->nfd, 0);
    checki("ppid of zombie", cp->ppid, mypid);
    ok = 1;
    for(int i = 0; i < after.nfile; i++)
      if(haspid(&after.files[i], pid))
        ok = 0;
    check(ok, "zombie is not in pids of any file");
  }

  checki("wait() returns the child", wait(&status), pid);
  checki("exit status", status, 7);
  snap(&after);
  check(findproc(&after, pid) == 0, "after wait() the child is no longer listed");
  checki("nproc", after.nproc, before.nproc);
}

void
test_sleeping(void)
{
  int p[2], pid, status = 0;

  pipe(p);
  pid = fork_waiter(p[0]);
  waitstate(&after, pid, SLEEPING);
  print_procs(&after);
  checkst("child blocked in read() of an empty pipe", stateof(&after, pid), SLEEPING);
  write(p[1], "x", 1);
  wait(0);
  close(p[0]);
  close(p[1]);

  pid = xfork();
  if(pid == 0){
    sleep(1000);
    exit(0);
  }
  waitstate(&after, pid, SLEEPING);
  checkst("child blocked in sleep(1000)", stateof(&after, pid), SLEEPING);
  kill(pid);
  checki("wait() returns the killed child", wait(&status), pid);
  checki("exit status of killed child", status, -1);
}

void
test_running(void)
{
  int pids[NSPIN], status, ok, allok = 1, running = 0, runnable = 0, grew = 1;

  for(int i = 0; i < NSPIN; i++){
    pids[i] = xfork();
    if(pids[i] == 0)
      for(;;)
        ;
  }

  // States change only at timer ticks, so poll a few times
  // until a snapshot shows both states.
  for(int k = 0; k < 50 && !(running && runnable); k++){
    if(k > 0)
      sleep(1);
    snap(&after);
    running = runnable = 0;
    for(int i = 0; i < NSPIN; i++){
      int st = stateof(&after, pids[i]);
      if(st == RUNNING)
        running++;
      else if(st == RUNNABLE)
        runnable++;
      else
        allok = 0;
    }
  }
  print_procs(&after);
  check(allok, "spinning children are always RUNNING or RUNNABLE");
  check(runnable > 0, "with more spinners than CPUs some of them wait as RUNNABLE");
  printf("         (in that snapshot: %d RUNNING, %d RUNNABLE)\n", running, runnable);

  snap(&before);
  sleep(5);
  snap(&after);
  for(int i = 0; i < NSPIN; i++){
    printf("         pid %d: nsched %d -> %d\n", pids[i],
           nschedof(&before, pids[i]), nschedof(&after, pids[i]));
    if(nschedof(&after, pids[i]) <= nschedof(&before, pids[i]))
      grew = 0;
  }
  check(grew, "nsched of every spinner grows (preempted by timer, scheduled again)");

  ok = 1;
  for(int i = 0; i < NSPIN; i++)
    kill(pids[i]);
  for(int i = 0; i < NSPIN; i++)
    if(wait(&status) < 0 || status != -1)
      ok = 0;
  check(ok, "killed spinners exit with status -1");
}

void
test_nsched(void)
{
  int p[2], pid, n1, n2, ok;

  snap(&before);
  sleep(2);
  snap(&after);
  n1 = nschedof(&before, mypid);
  n2 = nschedof(&after, mypid);
  result(n2 > n1);
  printf("nsched of sysinfotest grows after sleep(2): %d -> %d\n", n1, n2);

  // Nobody writes to the pipe, so nobody wakes the child up.
  pipe(p);
  pid = fork_waiter(p[0]);
  ok = waitstate(&before, pid, SLEEPING);
  sleep(5);
  snap(&after);
  n1 = nschedof(&before, pid);
  n2 = nschedof(&after, pid);
  result(ok && n1 == n2);
  printf("nsched of child blocked on a pipe does not change: %d -> %d\n", n1, n2);
  write(p[1], "x", 1);
  wait(0);
  close(p[0]);
  close(p[1]);

  // sleep() waits on &ticks, and every tick wakes all of those sleepers.
  pid = xfork();
  if(pid == 0){
    sleep(1000);
    exit(0);
  }
  ok = waitstate(&before, pid, SLEEPING);
  sleep(5);
  snap(&after);
  n1 = nschedof(&before, pid);
  n2 = nschedof(&after, pid);
  result(ok && n2 > n1);
  printf("nsched of child in sleep() grows (woken at every tick): %d -> %d\n", n1, n2);
  kill(pid);
  wait(0);
}

void
test_children(void)
{
  int p[2], pids[NCHILD], ok = 1;

  pipe(p);
  snap(&before);
  for(int i = 0; i < NCHILD; i++)
    pids[i] = fork_waiter(p[0]);
  snap(&after);
  print_procs(&after);

  checki("nproc increase", after.nproc - before.nproc, NCHILD);
  for(int i = 0; i < NCHILD; i++)
    if(findproc(&after, pids[i]) == 0 || findproc(&after, pids[i])->ppid != mypid)
      ok = 0;
  check(ok, "every child is listed with ppid = sysinfotest");
  checkfd(&after, p[0], FD_PIPE, "r-", 1 + NCHILD);
  check(consistent(&after), "file list and process list agree with each other");

  for(int i = 0; i < NCHILD; i++)
    write(p[1], "x", 1);
  for(int i = 0; i < NCHILD; i++)
    wait(0);
  snap(&after);
  checki("nproc after waiting all children", after.nproc, before.nproc);
  close(p[0]);
  close(p[1]);
}

void
test_orphan(void)
{
  int p[2], q[2], pid, gc = -1;
  char c;
  struct sysinfo_proc *g;

  pipe(p);
  pipe(q);
  pid = xfork();
  if(pid == 0){
    int gpid = fork();
    if(gpid == 0){
      close(p[1]);
      close(q[0]);
      close(q[1]);
      read(p[0], &c, 1);   // returns 0 once the test closes p[1]
      exit(0);
    }
    write(q[1], &gpid, sizeof(gpid));
    exit(0);
  }
  read(q[0], &gc, sizeof(gc));
  wait(0);

  snap(&after);
  print_procs(&after);
  check(findproc(&after, pid) == 0, "middle child is gone");
  g = findproc(&after, gc);
  check(g != 0, "grandchild is still alive");
  if(g)
    checki("ppid of orphan (reparented to init)", g->ppid, 1);

  close(p[0]);
  close(p[1]);
  close(q[0]);
  close(q[1]);
  check(waitstate(&after, gc, UNUSED), "init reaps the orphan after it exits");
}

void
test_nofile(void)
{
  int fds[NOFILE], n = 0, ok;
  struct sysinfo_proc *me;

  snap(&before);
  while(n < NOFILE && (fds[n] = open(testfile, O_RDONLY)) >= 0)
    n++;
  snap(&after);

  me = findproc(&after, mypid);
  checki("files opened until open() failed", n, NOFILE - nfdof(&before, mypid));
  checki("nfd of sysinfotest", me ? me->nfd : -1, NOFILE);
  ok = me != 0;
  for(int i = 0; ok && i < me->nfd; i++)
    ok = me->fds[i] == i;
  check(ok, "fds listed are exactly 0..15");
  check(consistent(&after), "file list and process list agree with each other");

  for(int i = 0; i < n; i++)
    close(fds[i]);
}

void
test_nfile(void)
{
  int done[2], go[2], fd, nfd;
  char c;

  pipe(done);
  pipe(go);
  for(int i = 0; i < NFILLER; i++){
    if(xfork() == 0){
      close(done[0]);
      close(go[1]);
      while(open(testfile, O_RDONLY) >= 0)
        ;
      write(done[1], "x", 1);
      read(go[0], &c, 1);   // returns 0 once the test closes go[1]
      exit(0);
    }
  }
  close(done[1]);
  close(go[0]);
  for(int i = 0; i < NFILLER; i++)
    read(done[0], &c, 1);

  snap(&after);
  checki("nfile (system-wide file table full)", after.nfile, NFILE);
  nfd = nfdof(&after, mypid);
  result(nfd < NOFILE);
  printf("sysinfotest has only %d fds open, NOFILE is not the limit here\n", nfd);
  fd = open(testfile, O_RDONLY);
  check(fd < 0, "open() fails because ftable is full, not because of NOFILE");
  if(fd >= 0)
    close(fd);
  check(consistent(&after), "file list and process list agree with each other");
  check(refs_match(&after), "ref of every file = number of fds pointing to it");

  close(go[1]);
  for(int i = 0; i < NFILLER; i++)
    wait(0);
  close(done[0]);
}

void
test_nproc(void)
{
  int p[2], n = 0, pid;
  char c;

  pipe(p);
  snap(&before);
  while(n < NPROC){
    pid = fork();
    if(pid < 0)
      break;
    if(pid == 0){
      read(p[0], &c, 1);
      exit(0);
    }
    n++;
  }
  snap(&after);

  checki("nproc (process table full)", after.nproc, NPROC);
  checki("children forked until fork() failed", n, NPROC - before.nproc);
  check(consistent(&after), "file list and process list agree with each other");

  for(int i = 0; i < n; i++)
    write(p[1], "x", 1);
  for(int i = 0; i < n; i++)
    wait(0);
  close(p[0]);
  close(p[1]);
}

void
test_badptr(void)
{
  char c;
  uint64 guard = PGROUNDDOWN((uint64)&c) - PGSIZE;
  uint64 heaptop = PGROUNDUP((uint64)sbrk(0));
  struct sysinfo *m;

  checki("sysinfo(0), page 0 is read-only text", sysinfo(0), -1);
  checki("sysinfo(0xeaeb0b5b00002f5e), above MAXVA",
         sysinfo((struct sysinfo *)0xeaeb0b5b00002f5eULL), -1);
  checki("sysinfo(TRAPFRAME), kernel-only page", sysinfo((struct sysinfo *)TRAPFRAME), -1);
  checki("sysinfo(stack guard page)", sysinfo((struct sysinfo *)guard), -1);
  checki("sysinfo(heap end - 16), buffer runs past the heap",
         sysinfo((struct sysinfo *)(heaptop - 16)), -1);

  m = malloc(sizeof(struct sysinfo));
  checki("sysinfo(buffer from malloc)", m ? sysinfo(m) : -1, 0);
  if(m){
    check(findproc(m, mypid) != 0, "sysinfotest is listed in the malloc buffer");
    free(m);
  }
  checki("sysinfo(&global) still works afterwards", sysinfo(&after), 0);
}

struct test {
  void (*fn)(void);
  char *name;
} tests[] = {
  { test_baseline,   "baseline: init, sh, sysinfotest, console" },
  { test_open,       "open() with each mode, a directory and a device" },
  { test_open_twice, "open() the same file twice" },
  { test_dup,        "dup()" },
  { test_fork,       "fork()" },
  { test_pipe,       "pipe()" },
  { test_close,      "close()" },
  { test_zombie,     "ZOMBIE until wait()" },
  { test_sleeping,   "SLEEPING (pipe read, sleep), kill" },
  { test_running,    "RUNNING / RUNNABLE spinners" },
  { test_nsched,     "nsched counting" },
  { test_children,   "many children" },
  { test_orphan,     "orphan reparented to init" },
  { test_nofile,     "per-process limit NOFILE" },
  { test_nfile,      "system-wide limit NFILE" },
  { test_nproc,      "process table full (NPROC)" },
  { test_badptr,     "bad user pointers" },
};
#define NTEST ((int)(sizeof(tests) / sizeof(tests[0])))

void
runtest(int t)
{
  int nfile0, nproc0;

  printf("\n== test %d: %s ==\n", t, tests[t-1].name);
  snap(&before);
  nfile0 = before.nfile;
  nproc0 = before.nproc;
  tests[t-1].fn();
  snap(&after);
  check(after.nfile == nfile0 && after.nproc == nproc0,
        "cleanup: nfile and nproc back to the values before the test");
}

// usage: sysinfotest          run all tests
//        sysinfotest 5 8      run only tests 5 and 8
//        sysinfotest show     print the current snapshot
int
main(int argc, char *argv[])
{
  int fd;

  mypid = getpid();

  if(argc == 2 && strcmp(argv[1], "show") == 0){
    snap(&before);
    print_files(&before, 0);
    print_procs(&before);
    exit(0);
  }

  fd = open(testfile, O_CREATE | O_TRUNC | O_RDWR);
  if(fd < 0 || write(fd, "abcdef", 6) != 6){
    printf("sysinfotest: cannot create %s\n", testfile);
    exit(1);
  }
  close(fd);

  printf("sysinfotest: start\n");
  if(argc < 2){
    for(int t = 1; t <= NTEST; t++)
      runtest(t);
  } else {
    for(int i = 1; i < argc; i++){
      int t = atoi(argv[i]);
      if(t < 1 || t > NTEST){
        printf("sysinfotest: no test %s (1..%d)\n", argv[i], NTEST);
        continue;
      }
      runtest(t);
    }
  }
  unlink(testfile);

  printf("\nsysinfotest: %d passed, %d failed\n", npass, nfail);
  if(nfail > 0){
    printf("sysinfotest: FAIL\n");
    exit(1);
  }
  printf("sysinfotest: OK\n");
  exit(0);
}
