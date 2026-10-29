#ifndef _SYSINFO_H_
#define _SYSINFO_H_

// kernel/sysinfo.h — include sau kernel/types.h và kernel/param.h
struct sysinfo_file {
  int idx;          // vị trí trong ftable.file[] (dùng để nối với process)
  int type;         // FD_NONE / FD_PIPE / FD_INODE / FD_DEVICE
  int readable;
  int writable;
  int ref;          // reference count
  int npids;
  int pids[NPROC];  // các process đang có fd trỏ tới file này
};

struct sysinfo_proc {
  int pid;
  int state;        // giá trị enum procstate
  int ppid;         // 0 nếu không có parent (init)
  int nsched;       // số lần được scheduler chọn chạy
  char name[16];
  int nfd;
  int fds[NOFILE];  // số fd
  int fidx[NOFILE]; // file tương ứng (khớp với sysinfo_file.idx)
};

struct sysinfo {
  int nfile;
  struct sysinfo_file files[NFILE];
  int nproc;
  struct sysinfo_proc procs[NPROC];
};

#endif
