#ifndef _SYSINFO_H_
#define _SYSINFO_H_

struct sysinfo_file {
  int idx;          
  int type;         
  int readable;
  int writable;
  int ref;         
  int npids;
  int pids[NPROC];
};

struct sysinfo_proc {
  int pid;
  int state;        
  int ppid;         
  int nsched;       
  char name[16];
  int nfd;
  int fds[NOFILE];  
  int fidx[NOFILE]; 
};

struct sysinfo {
  int nfile;
  struct sysinfo_file files[NFILE];
  int nproc;
  struct sysinfo_proc procs[NPROC];
};

#endif
