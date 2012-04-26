// Copyright (C) 2007 Manfred Georg <mgeorg@arl.wustl.edu>
//  
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software 
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

// Author: Manfred Georg
// Created: October 2007
// Modified: July 2009

#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <termios.h>

extern char *optarg;
extern int optind, opterr, optopt;
#include <getopt.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/select.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>

#define MAXPROMPTS 32
#define MAXFIFO 16
#define BUFSIZE 262144 // 256 KiB
#define CONTROL_MSG_SIZE 128

int childpid;
int quit;
int waitForReadline;
int pamperReadline;
int onNormalPrompt;
int clearPendingOnInterrupt;
int clearPending;
int doStatusReport;

void childDead(int signum) {
//  printf("child process died\n");
//  fflush(stdout);
  quit = 1;
}

void interruptHandler(int signum) {
//  printf("sending SIGINT to child\n");
//  printf("ignoring interrupt");
//  fflush(stdout);
  waitForReadline = 0;
  if(clearPendingOnInterrupt) {
    clearPending = 1;
  }
  kill(childpid, SIGINT);
  signal(signum,interruptHandler);
}

void statusReportHandler(int signum) {
  char str[1024];
  snprintf(str,1024,"childpid %d, quit %d, waitForReadline %d,\r\npamperReadline %d, onNormalPrompt %d,\r\nclearPendingOnInterrupt %d, clearPending %d\r\n",
           childpid, quit, waitForReadline, pamperReadline,
           onNormalPrompt, clearPendingOnInterrupt, clearPending);
  write(1,str,strlen(str));
  doStatusReport = 10;
  signal(signum,statusReportHandler);
}

//void clearPamperReadline(int signum) {
//  waitForReadline = 0;
//  pamperReadline = 0;
//  onNormalPrompt = 1;
//  signal(signum,clearPamperReadline);
//}
//
//void setPamperReadline(int signum) {
//  waitForReadline = 0;
//  pamperReadline = 1;
//  onNormalPrompt = 1;
//  signal(signum,setPamperReadline);
//}

// a function for the debugger to check what is available for reading
// and writing (debugfd should be same as out probably)
void testSelect(int in, int out, int fifo, int fd, int debugfd) {
  int max;
  fd_set readSet;
  fd_set writeSet;
  fd_set errorSet;
  max = -1;
  max = (max>in)?max:in;
  max = (max>out)?max:out;
  max = (max>fifo)?max:fifo;
  max = (max>fd)?max:fd;
  FD_SET(in,&errorSet);
  FD_SET(out,&errorSet);
  if(fifo != -1) {
    FD_SET(fifo,&errorSet);
  }
  FD_SET(fd,&errorSet);
  FD_SET(in,&readSet);
  FD_SET(out,&readSet);
  if(fifo != -1) {
    FD_SET(fifo,&readSet);
  }
  FD_SET(fd,&readSet);
  FD_SET(in,&writeSet);
  FD_SET(out,&writeSet);
  if(fifo != -1) {
    FD_SET(fifo,&writeSet);
  }
  FD_SET(fd,&writeSet);
  char buf[1024];
  int size;
  size = snprintf(buf,1024,"%d%d%d%d %d%d%d%d %d%d%d%d - ",
                  FD_ISSET(0,&readSet),FD_ISSET(1,&readSet),
                  FD_ISSET(fifo,&readSet),FD_ISSET(fd,&readSet),
                  FD_ISSET(0,&writeSet),FD_ISSET(1,&writeSet),
                  FD_ISSET(fifo,&writeSet),FD_ISSET(fd,&writeSet),
                  FD_ISSET(0,&errorSet),FD_ISSET(1,&errorSet),
                  FD_ISSET(fifo,&errorSet),FD_ISSET(fd,&errorSet));
  write(debugfd,buf,size);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  int ret = select(max+1,&readSet,&writeSet,&errorSet,&tv);
  size = snprintf(buf,1024,"%d%d%d%d %d%d%d%d %d%d%d%d select returned %d",
                  FD_ISSET(0,&readSet),FD_ISSET(1,&readSet),
                  FD_ISSET(fifo,&readSet),FD_ISSET(fd,&readSet),
                  FD_ISSET(0,&writeSet),FD_ISSET(1,&writeSet),
                  FD_ISSET(fifo,&writeSet),FD_ISSET(fd,&writeSet),
                  FD_ISSET(0,&errorSet),FD_ISSET(1,&errorSet),
                  FD_ISSET(fifo,&errorSet),FD_ISSET(fd,&errorSet),ret);
  write(debugfd,buf,size);
}

int main(int argc, char **argv) {
  int fd;
  int pty;
  int ret;
  char ptyName[80];

  const char* ifnames[MAXFIFO];
  const char* cfname = NULL;
  const char* ofname = NULL;
  const char* dfname = NULL;
  const char* debugfname = NULL;
  int numPrompts = 0;
  char *prompts[MAXPROMPTS];
  int normalPrompt[MAXPROMPTS];
  int fifos[MAXFIFO];
  int numFifo=0;
  int dumpFd=-1;
  int outFd=-1;
  int controlFd=-1;
  int laxWaitForReadline=0;

  pamperReadline = 0;
  clearPendingOnInterrupt = 0;
  clearPending = 0;
  doStatusReport = 0;
  int doEcho = 1;

  int c;
  while ((c = getopt(argc, argv, "+i:o:c:d:D:r:R:e::l::z::")) != -1) {
    switch(c) {
    case 'i':
      if(numFifo >= MAXFIFO) {
        fprintf(stderr,"too many fifo\n");
        exit(1);
      }
      ifnames[numFifo] = optarg;
      numFifo++;
      break;
    case 'o':
      ofname = optarg;
      break;
    case 'c':
      cfname = optarg;
      break;
    case 'd':
      dfname = optarg;
      break;
    case 'D':
      debugfname = optarg;
      break;
    case 'R':
    case 'r':
      if(numPrompts >= MAXPROMPTS) {
        fprintf(stderr,"too many prompts\n");
        exit(1);
      }
      pamperReadline = 1;
      prompts[numPrompts] = optarg;
      if(c=='R') {
        normalPrompt[numPrompts] = 0;
        if(*prompts[numPrompts] == '\0') {
          write(2,"can't have empty string be non-normal prompt\n",45);
          normalPrompt[numPrompts] = 1;
        }
      } else {
        normalPrompt[numPrompts] = 1;
      }
      numPrompts++;
      break;
    case 'e':
      doEcho = 0;
      if(optarg) {
        doEcho = strtol(optarg,NULL,10);
      }
      break;
    case 'l':
      laxWaitForReadline = 0;
      if(optarg) {
        laxWaitForReadline = strtol(optarg,NULL,10);
      }
      break;
    case 'z':
      clearPendingOnInterrupt = 1;
      clearPending = 1; // to show that 
      if(optarg) {
        clearPendingOnInterrupt = strtol(optarg,NULL,10);
      }
      break;
    }
  }
  if(pamperReadline && !clearPending) {
    clearPendingOnInterrupt = 1; // default with readline if not set explicitly
    clearPending = 0;
  }

  quit = 0;
  struct termios origTermData;
  tcgetattr(0,&origTermData);

//  openpty(&fd,&slave,NULL,&tt,&win);
//  fd = getpt();
  // perhaps O_ASYNC is interesting
  fd = open("/dev/ptmx", O_RDWR|O_NOCTTY);
  if(fd == -1) {
    printf("error with getpt\n");
    exit(1);
  }
  ret = grantpt(fd);
  if(ret != 0) {
    printf("error with grantpt\n");
    exit(1);
  }
  ret = unlockpt(fd);
  if(ret != 0) {
    printf("error with unlockpt\n");
    exit(1);
  }

  switch(childpid = fork()) {
  case -1:
    printf("ERROR!!!!\n");
    exit(1);
  case 0:
//    printf("child\n");
    ret = ptsname_r(fd,ptyName,80);
    if(ret != 0) {
      printf("ERROR: ptsname\n");
      exit(1);
    }
    pty = open(ptyName,O_RDWR);
    if(pty == -1) {
      printf("ERROR: could not open '%s'\n",ptyName);
      exit(1);
    }
    if(doEcho) {
      tcsetattr(pty,TCSANOW,&origTermData);
    } else {
      struct termios termData;
      int setTerm=0;
      setTerm = 1;
      termData = origTermData;
      termData.c_lflag &= ~ECHO;
      tcsetattr(pty,TCSANOW,&termData);
    }
    if(pty == 0) {
      printf("ERROR: tcsetattr (in child)\n");
      exit(1);
    }
    close(0);
    close(1);
    close(2);
    dup2(pty,0);
    dup2(pty,1);
    dup2(pty,2);
    setsid(); // connect the new process to the tty

    ret = ioctl(0, TIOCSCTTY, 0);
    if(ret == -1) {
      printf("error with ioctl\n");
      fflush(stdout);
      exit(1);
    }
    close(fd);
    close(pty);
//    char program[80] = "echoAsterix";
//    char arg1[80] = "--raw";
    char program[80] = "R";
    char arg1[80] = "--no-save";
//    char program[80] = "bash";
//    char arg1[80] = "";
    char *execArgs[3];
    execArgs[0] = program;
    execArgs[1] = arg1;
    execArgs[2] = (char*)NULL;
//    execArgs[1] = (char*)NULL;
    if(argc > optind) {
      execvp(argv[optind],&argv[optind]);
    } else {
      execvp(execArgs[0],execArgs);
    }
//    execlp("R", "--no-save", (char*)0);
//    execlp("matlab", "-nojvm", "-nosplash", "-nodesktop", (char*)0);
    printf("CHILD HAS AN ERROR...DIDN'T execlp");
    exit(1);
  }
//  printf("parent\n");
//  signal(SIGINT,SIG_IGN);
  signal(SIGCHLD,childDead);
//  signal(SIGUSR1,clearPamperReadline);
//  signal(SIGUSR2,setPamperReadline);
  signal(SIGINT,interruptHandler);
  signal(SIGUSR1,statusReportHandler);
  int i;

  struct stat statOfFile;

  int curFifo;
  for(curFifo=0;curFifo<numFifo;curFifo++) {
    ret = stat(ifnames[curFifo],&statOfFile);
    if(ret != 0) {
      if(errno == ENOENT) { // file does not exist, create it
        ret = mkfifo(ifnames[curFifo],S_IRWXU | S_IRWXG | S_IRWXO );
        if(ret != 0) {
          perror(argv[0]);
          exit(1);
        }
      } else {
        perror(argv[0]);
        exit(1);
      }
    } else {
      if(!S_ISFIFO(statOfFile.st_mode)) {
        printf("'%s' is not a FIFO\n",ifnames[curFifo]);
        exit(1);
      }
    }

    fifos[curFifo] = open(ifnames[curFifo],O_RDWR);
    if(fifos[curFifo] == -1) {
      printf("error opening FIFO '%s'\n",ifnames[curFifo]);
      exit(1);
    }
    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_pid = 0;
    if(fcntl(fifos[curFifo],F_SETLK,&fl) == -1) {
      fcntl(fifos[curFifo],F_GETLK,&fl);
      printf("fifo '%s' is locked by %d\n",ifnames[curFifo],fl.l_pid);
      close(fifos[curFifo]);
      exit(1);
    }
  }
  if(ofname) {
    outFd = open(ofname,O_WRONLY | O_CREAT | O_TRUNC,0644);
    if(outFd == -1) {
      printf("error opening file '%s'\n",ofname);
      exit(1);
    }
  }
  if(cfname) {
    // TODO make this a subroutine
    ret = stat(cfname,&statOfFile);
    if(ret != 0) {
      if(errno == ENOENT) { // file does not exist, create it
        ret = mkfifo(cfname,S_IRWXU | S_IRWXG | S_IRWXO );
        if(ret != 0) {
          perror(argv[0]);
          exit(1);
        }
      } else {
        perror(argv[0]);
        exit(1);
      }
    } else {
      if(!S_ISFIFO(statOfFile.st_mode)) {
        printf("'%s' is not a FIFO\n",cfname);
        exit(1);
      }
    }
    controlFd = open(cfname,O_RDWR);
    if(controlFd == -1) {
      printf("error opening control file '%s'\n",cfname);
      exit(1);
    }
    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_pid = 0;
    if(fcntl(controlFd,F_SETLK,&fl) == -1) {
      fcntl(controlFd,F_GETLK,&fl);
      printf("fifo '%s' is locked by %d\n",cfname,fl.l_pid);
      close(controlFd);
      exit(1);
    }
  }
  if(dfname) {
    dumpFd = open(dfname,O_WRONLY | O_CREAT | O_TRUNC,0644);
    if(dumpFd == -1) {
      printf("error opening file '%s'\n",dfname);
      exit(1);
    }
  }
  
  struct termios termData;
  int setTerm=0;
  setTerm = 1;
  termData = origTermData;
//  tcgetattr(0,&termData);
  cfmakeraw(&termData);
//  termData.c_iflag &= ~IGNBRK;
  if(clearPendingOnInterrupt) {
    // catch interrupts instead of passing them through the data stream.
    // We will interrupt child instead of letting the terminal do it.
    // This is usually undesirable since they can no longer control
    // interrupt behavior through their terminal.
    termData.c_lflag |= ISIG;
  }
//  termData.c_lflag &= ~ICANON;
//  termData.c_lflag &= ~ECHO;
//  termData.c_lflag |= ECHO; // local echo
//  termData.c_cc[VMIN] = 1;
//  termData.c_cc[VTIME] = 0;
  tcsetattr(0,TCSANOW,&termData);
//  if(argc>1 && strcmp(ifname,"--raw")==0) {
//  }

//  fcntl(fd,F_SETFL,O_NONBLOCK);

  fd_set readSet;
  fd_set writeSet;
  fd_set errorSet;
  int numBytes;
  int max;

  char toFdBuf[BUFSIZE];
  char *toFdPos = toFdBuf;
  int toFdSize = 0;
  char toOutBuf[BUFSIZE];
  char *toOutPos = toOutBuf;
  int toOutSize = 0;
  waitForReadline=0;
  char *posInPrompts[MAXPROMPTS];
  int curPrompt;
  for(curPrompt=0;curPrompt<numPrompts;curPrompt++) {
    posInPrompts[curPrompt] = prompts[curPrompt];
  }
  onNormalPrompt = 1;

  int debugfd=-1;
  if(debugfname) {
    debugfd = open(debugfname,O_WRONLY | O_CREAT | O_TRUNC,0644);
    if(debugfd == -1) {
      printf("error opening debug file '%s'",debugfname);
      exit(1);
    }
  }

  while(!quit) {
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);

    max = -1;
    FD_SET(0,&errorSet);
    max = (max>0)?max:0;
    FD_SET(1,&errorSet);
    max = (max>1)?max:1;
    for(curFifo=0;curFifo<numFifo;curFifo++) {
      FD_SET(fifos[curFifo],&errorSet);
      max = (max>fifos[curFifo])?max:fifos[curFifo];
    }
    if(controlFd != -1) {
      FD_SET(controlFd,&errorSet);
      max = (max>controlFd)?max:controlFd;
    }
    FD_SET(fd,&errorSet);
    max = (max>fd)?max:fd;

    if(toFdSize < BUFSIZE) {
      FD_SET(0,&readSet);
      max = (max>0)?max:0;
      if(onNormalPrompt) {
        for(curFifo=0;curFifo<numFifo;curFifo++) {
          FD_SET(fifos[curFifo],&readSet);
          max = (max>fifos[curFifo])?max:fifos[curFifo];
        }
      }
    }
    if(toOutSize < BUFSIZE) {
      FD_SET(fd,&readSet);
      max = (max>fd)?max:fd;
    }
    if(controlFd != -1) {
      FD_SET(controlFd,&readSet);
      max = (max>controlFd)?max:controlFd;
    }

    if(!waitForReadline && toFdSize > 0) {
      FD_SET(fd,&writeSet);
      max = (max>fd)?max:fd;
    }
    if(toOutSize > 0) {
      FD_SET(1,&writeSet);
      max = (max>1)?max:1;
    }
//    ret = select(max+1,&readSet,&writeSet,&errorSet,NULL);
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if(debugfd != -1) {
      char buf[1024];
      int size;
      size = snprintf(buf,1024,"%d%d%d%d %d%d%d%d %d%d%d%d - ",
                      FD_ISSET(0,&readSet),FD_ISSET(1,&readSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&readSet),
                      FD_ISSET(fd,&readSet),
                      FD_ISSET(0,&writeSet),FD_ISSET(1,&writeSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&writeSet),
                      FD_ISSET(fd,&writeSet),
                      FD_ISSET(0,&errorSet),FD_ISSET(1,&errorSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&errorSet),
                      FD_ISSET(fd,&errorSet));
      write(debugfd,buf,size);
    }
    errno = 0;
    if(doStatusReport) {
      char buf[1024];
      int size;
      size = snprintf(buf,1024,"before %d%d%d%d %d%d%d%d %d%d%d%d\r\n",
                      FD_ISSET(0,&readSet),FD_ISSET(1,&readSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&readSet),
                      FD_ISSET(fd,&readSet),
                      FD_ISSET(0,&writeSet),FD_ISSET(1,&writeSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&writeSet),
                      FD_ISSET(fd,&writeSet),
                      FD_ISSET(0,&errorSet),FD_ISSET(1,&errorSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&errorSet),
                      FD_ISSET(fd,&errorSet));
      write(1,buf,size);
      doStatusReport--;
    }
    ret = select(max+1,&readSet,&writeSet,&errorSet,&tv);
    if(doStatusReport) {
      char buf[1024];
      int size;
      size = snprintf(buf,1024,"after  %d%d%d%d %d%d%d%d %d%d%d%d\r\n",
                      FD_ISSET(0,&readSet),FD_ISSET(1,&readSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&readSet),
                      FD_ISSET(fd,&readSet),
                      FD_ISSET(0,&writeSet),FD_ISSET(1,&writeSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&writeSet),
                      FD_ISSET(fd,&writeSet),
                      FD_ISSET(0,&errorSet),FD_ISSET(1,&errorSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&errorSet),
                      FD_ISSET(fd,&errorSet));
      write(1,buf,size);
    }
    if(debugfd != -1) {
      char buf[1024];
      int size;
      size = snprintf(buf,1024,"%d%d%d%d %d%d%d%d %d%d%d%d ",
                      FD_ISSET(0,&readSet),FD_ISSET(1,&readSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&readSet),
                      FD_ISSET(fd,&readSet),
                      FD_ISSET(0,&writeSet),FD_ISSET(1,&writeSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&writeSet),
                      FD_ISSET(fd,&writeSet),
                      FD_ISSET(0,&errorSet),FD_ISSET(1,&errorSet),
                      FD_ISSET(numFifo>0?fifos[0]:0,&errorSet),
                      FD_ISSET(fd,&errorSet));
      write(debugfd,buf,size);
    }
    if(ret == -1) {
      if(errno == EINTR) {
        if(doStatusReport) {
          write(1,"caught a signal\r\n",strlen("caught a signal\r\n"));
        }
        if(debugfd != -1) {
          char buf = '\n';
          write(debugfd,&buf,1);
        }
        continue;
      } else {
        printf("select had an error\n");
        break;
      }
    }
    if(clearPending) {
      toFdSize = 0;
      clearPending = 0;
      if(debugfd != -1) {
        char buf = '\n';
        write(debugfd,&buf,1);
      }
      continue;
    }
    if(errno == EINTR) {
      write(1,"caught a signal with good return value\r\n",
            strlen("caught a signal with good return value\r\n"));
      if(debugfd != -1) {
        char buf = '\n';
        write(debugfd,&buf,1);
      }
      continue;
    }
    if(ret == 0) {
      if(debugfd != -1) {
        char buf = '\n';
        write(debugfd,&buf,1);
      }
      if(laxWaitForReadline) {
        waitForReadline = 0;
      }
      continue;
    }

    // check for errors
    if(FD_ISSET(0,&errorSet)) {
      error_at_line(0,errno,__FILE__,__LINE__,"0 is set in errorSet");
      break;
    }
    if(FD_ISSET(1,&errorSet)) {
      error_at_line(0,errno,__FILE__,__LINE__,"1 is set in errorSet");
      break;
    }
    for(curFifo=0;curFifo<numFifo;curFifo++) {
      if(FD_ISSET(fifos[curFifo],&errorSet)) {
        error_at_line(0,errno,__FILE__,__LINE__,
                      "fifos[%d] is set in errorSet",curFifo);
        break;
      }
    }
    if(FD_ISSET(fd,&errorSet)) {
      error_at_line(0,errno,__FILE__,__LINE__,"fd is set in errorSet");
      break;
    }
    if(controlFd != -1 && FD_ISSET(controlFd,&errorSet)) {
      error_at_line(0,errno,__FILE__,__LINE__,"controlFd is set in errorSet");
      break;
    }

    if(controlFd != -1 && FD_ISSET(controlFd,&readSet)) {
      // TODO not interrupt safe
      int k;
      char command;
      write(2,"eet command ",12);
      numBytes = read(controlFd,&command,1);
      if(numBytes != 1) continue;
      write(2,&command,1);
      write(2," '",2);
      char *buf = malloc(CONTROL_MSG_SIZE);
      buf[CONTROL_MSG_SIZE] = '\0';
      for(k=0;k<CONTROL_MSG_SIZE-1;k++) {
        numBytes = read(controlFd,&buf[k],1);
        if(numBytes != 1) {
          buf[k] = '\0';
          break;
        }
        if(buf[k] == '\0') {
          break;
        }
      }
      write(2,buf,strlen(buf));
      write(2,"'",1);
      if(numBytes != 1 || k >= CONTROL_MSG_SIZE-1) {
        write(2,"improperly terminated",21);
      }
      write(2,"\r\n",2);
      switch(command) {
      case 'R':
      case 'r':
        if(numPrompts >= MAXPROMPTS) {
          fprintf(stderr,"too many prompts\n");
          exit(1);
        }
        pamperReadline = 1;
        prompts[numPrompts] = buf;
        if(command=='R') {
          normalPrompt[numPrompts] = 0;
          if(*prompts[numPrompts] == '\0') {
            write(2,"can't have empty string be non-normal prompt\n",45);
            normalPrompt[numPrompts] = 1;
          }
        } else {
          normalPrompt[numPrompts] = 1;
        }
        posInPrompts[numPrompts] = prompts[numPrompts];
        numPrompts++;
        waitForReadline = 0;
        break;
      case '*':
        waitForReadline = 0;
        break;
      case 'p':
        waitForReadline = 0;
        pamperReadline = 0;
        if(buf && buf[0] != '\0') {
          pamperReadline = strtol(buf,NULL,10);
        }
        onNormalPrompt = 1;
        break;
      case 'l':
        laxWaitForReadline = 0;
        if(buf && buf[0] != '\0') {
          laxWaitForReadline = strtol(buf,NULL,10);
        }
        break;
      case 'z':
        clearPendingOnInterrupt = 1;
        if(buf) {
          clearPendingOnInterrupt = strtol(buf,NULL,10);
        }
        break;
      }
      continue;
    }

    int numBytes0=0,numBytes1=0,numBytesFifo=0,
        numBytesFdIn=0,numBytesFdOut=0;

    // write to these
    if(FD_ISSET(1,&writeSet)) {
      int numToWrite;
      if(toOutPos-toOutBuf+toOutSize < BUFSIZE) {
        numToWrite = toOutSize;
      } else {
        numToWrite = BUFSIZE - (toOutPos-toOutBuf);
      }
      numBytes = write(1,toOutPos,numToWrite);
      if(numBytes == -1) {
        error_at_line(0,errno,__FILE__,__LINE__,"write failed");continue; }
      if(outFd != -1) { // everything going to stdout also goes to outFd
        write(outFd,toOutPos,numBytes); // hope no error (don't reuse numBytes)
      }
      toOutSize -= numBytes;
      toOutPos += numBytes;
      if(toOutPos - toOutBuf >= BUFSIZE) {
        toOutPos -= BUFSIZE;
      }
//      printf("write to stdout %d bytes\r\nbase 0x%tx\r\n"
//             "pos  0x%tx toOutSize is now %d numToWrite %d\r\n",
//             numBytes, (ptrdiff_t)toOutBuf,
//             (ptrdiff_t)toOutPos,toOutSize,numToWrite);
      numBytes1 = numBytes;
    }
    if(FD_ISSET(fd,&writeSet)) {
      int numToWrite;
      if(toFdPos-toFdBuf+toFdSize < BUFSIZE) {
        numToWrite = toFdSize;
      } else {
        numToWrite = BUFSIZE - (toFdPos-toFdBuf);
      }
      if(pamperReadline) {
        int i;
        char *tmp=toFdPos;
        for(i=0;i<numToWrite;i++) {
          if(*tmp == '\r' || *tmp == '\n') {
            numToWrite = i+1;
//              write(1,"newline",strlen("newline"));
            waitForReadline = 1;
            break;
          }
          tmp++;
          if(tmp - toFdBuf >= BUFSIZE) {
            // not technically needed (range of numToWrite will not cross this)
            tmp -= BUFSIZE;
          }
        }
      }
      if(clearPending) {
        toFdSize = 0;
        clearPending = 0;
        if(debugfd != -1) {
          char buf = '\n';
          write(debugfd,&buf,1);
        }
        continue;
      }
      numBytes = write(fd,toFdPos,numToWrite);
      if(pamperReadline && numBytes != numToWrite) {
        waitForReadline = 0;
//        write(1,"unsetting waitForReadline",strlen("unsetting waitForReadline"));
      }
      if(numBytes == -1) {
        error_at_line(0,errno,__FILE__,__LINE__,"write failed");continue; }
      if(dumpFd != -1) { // duplicate traffic to terminal program in dumpFd
        write(dumpFd,toFdPos,numBytes); // hope no error (don't reuse numBytes)
      }
      toFdSize -= numBytes;
      toFdPos += numBytes;
      if(toFdPos - toFdBuf >= BUFSIZE) {
        toFdPos -= BUFSIZE;
      }
//      printf("write to fd %d bytes\r\nbase 0x%tx\r\n"
//             "pos  0x%tx toFdSize is now %d numToWrite %d\r\n",
//             numBytes, (ptrdiff_t)toFdBuf,
//             (ptrdiff_t)toFdPos,toFdSize,numToWrite);
      numBytesFdIn = numBytes;
    }

    // read from these
    if(FD_ISSET(0,&readSet)) {
      int numToRead;
      char *startPos;
      if(toFdPos-toFdBuf+toFdSize < BUFSIZE) {
        startPos = toFdPos+toFdSize;
        numToRead = BUFSIZE - (startPos - toFdBuf);
      } else {
        startPos = toFdPos+toFdSize-BUFSIZE;
        numToRead = BUFSIZE - toFdSize;
      }
      numBytes = read(0,startPos,numToRead);
      if(numBytes == -1) {
        error_at_line(0,errno,__FILE__,__LINE__,"read failed");continue; }
      toFdSize += numBytes;
//      printf("read from stdin %d bytes\r\nat   0x%tx\r\nbase 0x%tx\r\n"
//             "pos  0x%tx toFdSize is now %d numToRead %d\r\n",
//             numBytes,(ptrdiff_t)startPos, (ptrdiff_t)toFdBuf,
//             (ptrdiff_t)toFdPos,toFdSize,numToRead);
      numBytes0 = numBytes;
    }
    for(curFifo=0;curFifo<numFifo;curFifo++) {
      if(FD_ISSET(fifos[curFifo],&readSet)) {
        int numToRead;
        char *startPos;
        if(toFdPos-toFdBuf+toFdSize < BUFSIZE) {
          startPos = toFdPos+toFdSize;
          numToRead = BUFSIZE - (startPos - toFdBuf);
        } else {
          startPos = toFdPos+toFdSize-BUFSIZE;
          numToRead = BUFSIZE - toFdSize;
        }
        numBytes = read(fifos[curFifo],startPos,numToRead);
        if(numBytes == -1) {
          error_at_line(0,errno,__FILE__,__LINE__,"read failed");continue; }
        toFdSize += numBytes;
  //      printf("read from fifos[%d] %d bytes\r\nat   0x%tx\r\nbase 0x%tx\r\n"
  //             "pos  0x%tx toFdSize is now %d numToRead %d\r\n",curFifo,
  //             numBytes,(ptrdiff_t)startPos, (ptrdiff_t)toFdBuf,
  //             (ptrdiff_t)toFdPos,toFdSize,numToRead);
        numBytesFifo += numBytes;
      }
    }
    if(FD_ISSET(fd,&readSet)) {
      int numToRead;
      char *startPos;
      if(toOutPos-toOutBuf+toOutSize < BUFSIZE) {
        startPos = toOutPos+toOutSize;
        numToRead = BUFSIZE - (startPos - toOutBuf);
      } else {
        startPos = toOutPos+toOutSize-BUFSIZE;
        numToRead = BUFSIZE - toOutSize;
      }
      numBytes = read(fd,startPos,numToRead);
      if(pamperReadline && (waitForReadline || !onNormalPrompt)) {
        // need to look all the time when on a non-normal prompt since
        // we may not input a newline on a non-normal prompt and have
        // it transition to a normal prompt (think q given to less)
        int i;
        int curPrompt;
        for(curPrompt=0;curPrompt<numPrompts;curPrompt++) {
          if(*prompts[curPrompt] != '\0') {
            for(i=0;i<numBytes;i++) {
              if(*posInPrompts[curPrompt] == '\0') {
                posInPrompts[curPrompt] = prompts[curPrompt];
              }
              if(*posInPrompts[curPrompt] == startPos[i]) {
                posInPrompts[curPrompt]++;
              } else {
                posInPrompts[curPrompt] = prompts[curPrompt];
              }
            }
          }
        }
        int checkForMore = 0;
        int normal = 1;
        for(curPrompt=0;curPrompt<numPrompts;curPrompt++) {
          if(*posInPrompts[curPrompt] == '\0') {
            checkForMore = 1;
            normal = normal && normalPrompt[curPrompt];
          }
        }
        if(checkForMore || numPrompts <= 0) {
          // TODO this should really be rolled into the select in the main
          // loop so we can do other things instead of waiting in a sleep
          struct timespec ts;
          ts.tv_sec = 0;
//          ts.tv_nsec = 10000000; // 10ms
          ts.tv_nsec = 1000000; // 1ms
//          write(1,"waiting",strlen("waiting"));
          nanosleep(&ts,NULL);
          FD_ZERO(&readSet);
          FD_SET(fd,&readSet);
          tv.tv_sec = 0;
          tv.tv_usec = 0;
          ret = select(fd+1,&readSet,NULL,NULL,&tv);
          if(!FD_ISSET(fd,&readSet)) { // no more input available (on prompt)
            waitForReadline = 0;
            onNormalPrompt = normal;
//            if(onNormalPrompt) {
//              write(2,"normalPrompt",strlen("normalPrompt"));
//            } else {
//              write(2,"notNormalPrompt",strlen("notNormalPrompt"));
//            }
          }
        }
      }
//      if(numBytes == -1) {error_at_line(0,errno,__FILE__,__LINE__,"read failed");continue; }
      if(numBytes == -1) {break;} // usually pipe breaks before SIGCHLD
      toOutSize += numBytes;
//      printf("read from fd %d bytes\r\nat   0x%tx\r\nbase 0x%tx\r\n"
//             "pos  0x%tx toOutSize is now %d numToRead %d\r\n",
//             numBytes,(ptrdiff_t)startPos, (ptrdiff_t)toOutBuf,
//             (ptrdiff_t)toOutPos,toOutSize,numToRead);
      numBytesFdOut = numBytes;
    }

    if(debugfd != -1) {
      // TODO do the fifos right in the dump (instead of only first one)
      char buf[1024];
      int size;
      size = snprintf(buf,1024,
                      "fd%5d out%5d numBytes x:%d %d:x x:%d %d:%d\r\n",
                      toFdSize,toOutSize,
                      numBytes0,numBytes1,numBytesFifo,
                      numBytesFdIn,numBytesFdOut);
      write(debugfd,buf,size);
    }
  }

  if(debugfd != -1) {
    close(debugfd);
  }
  close(fd);
  for(curFifo=0;curFifo<numFifo;curFifo++) {
    close(fifos[curFifo]);
  }
  if(outFd != -1) {
    close(outFd);
  }
  if(dumpFd != -1) {
    close(dumpFd);
  }
  if(setTerm) {
    tcsetattr(0,TCSANOW,&origTermData);
  }

}
