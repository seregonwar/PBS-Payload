// SPDX-License-Identifier: GPL-3.0-or-later
/* One-shot, read-only export of a fixed diagnostic file list. No injection. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int send_all(int fd,const void *p,size_t n) {
  const unsigned char *b=p;
  while (n) {
    ssize_t r=send(fd,b,n,0);
    if (r<=0) return -1;
    b+=r;n-=(size_t)r;
  }
  return 0;
}

int main(void) {
  const char *names[]={"open.log","capture.log","reads.csv","capture.h264"};
  signal(SIGPIPE,SIG_IGN);
  int listener=socket(AF_INET,SOCK_STREAM,0),client=-1,result=1;
  if (listener<0) return 1;
  int yes=1;
  setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
  struct sockaddr_in address={0};
  address.sin_len=sizeof address;
  address.sin_family=AF_INET;address.sin_port=htons(9401);address.sin_addr.s_addr=htonl(INADDR_ANY);
  if (bind(listener,(struct sockaddr*)&address,sizeof address) || listen(listener,1)) goto done;
  fd_set readable;FD_ZERO(&readable);FD_SET(listener,&readable);
  struct timeval wait={30,0};
  if (select(listener+1,&readable,NULL,NULL,&wait)<=0) goto done;
  client=accept(listener,NULL,NULL);
  if (client<0) goto done;
  struct timeval timeout={10,0};
  setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
  if (send_all(client,"PBS940COLLECT1\n",15)) goto done;
  for (unsigned i=0;i<sizeof names/sizeof *names;++i) {
    char path[160],header[160];struct stat st;
    snprintf(path,sizeof path,"/data/pbs/diag-940/%s",names[i]);
    FILE *f=fopen(path,"rb");
    long long length=-1;
    if (f && !fstat(fileno(f),&st) && S_ISREG(st.st_mode) && st.st_size>=0 && st.st_size<=100*1024*1024)
      length=st.st_size;
    int n=snprintf(header,sizeof header,"FILE %s %lld\n",names[i],length);
    if (send_all(client,header,(size_t)n)) { if(f)fclose(f);goto done; }
    unsigned char block[32768];
    while (length>0) {
      size_t want=length>(long long)sizeof block ? sizeof block : (size_t)length;
      size_t count=fread(block,1,want,f);
      if (count!=want || send_all(client,block,count)) { fclose(f);goto done; }
      length-=count;
    }
    if (f) fclose(f);
    if (send_all(client,"\n",1)) goto done;
  }
  if (!send_all(client,"END\n",4)) result=0;
done:
  if(client>=0)close(client);
  close(listener);
  return result;
}
