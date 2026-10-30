// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/http/server.h"
#include "common/media/profiles.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static const unsigned char sps[]={0x67,0x4d,0x40,0x28},pps[]={0x68,0xee,0x3c,0x80};
static char response[4096];
static void request(const char *method,const char *path,int code) {
  int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
  struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(18092),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  assert(!connect(fd,(struct sockaddr*)&address,sizeof address));
  char req[512];int n=snprintf(req,sizeof req,"%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n",method,path);
  assert(send(fd,req,n,0)==n);
  size_t used=0;ssize_t got;
  while((got=recv(fd,response+used,sizeof response-1-used,0))>0)used+=(size_t)got;
  response[used]=0;close(fd);
  char status[32];snprintf(status,sizeof status,"HTTP/1.1 %d",code);assert(strstr(response,status));
}
static void switch_to(int height,unsigned feature) {
  char path[80];snprintf(path,sizeof path,"/capture/profile?height=%d",height);
  request("POST",path,200);
  request("POST",path,409); // Do not overwrite an in-flight transaction.
  request("POST","/record/start",409);
  assert(pbs_h264_take_restart_request());
  int requested=0;assert(pbs_h264_take_profile_request(&requested)&&requested==height);
  assert(!pbs_h264_take_profile_request(&requested));
  pbs_capture_profile profile;assert(!pbs_capture_profile_for_height(requested,&profile));
  assert(profile.feature==feature && profile.width*9==profile.height*16);
  pbs_h264_discontinuity();
  assert(!pbs_h264_server_start(18092,profile.width,profile.height,sps,sizeof sps,pps,sizeof pps));
  request("GET","/config.json",200);
  char size[64];snprintf(size,sizeof size,"\"height\":%d",height);
  assert(strstr(response,size)&&strstr(response,"\"profile_changing\":false"));
}
int main(void) {
  assert(!pbs_h264_server_start(18092,2560,1440,sps,sizeof sps,pps,sizeof pps));
  request("POST","/capture/profile?height=1080",501);
  pbs_h264_enable_profiles();
  request("POST","/capture/profile?height=999",400);
  request("POST","/capture/profile?height=1080&extra=1",400);
  switch_to(2160,10);switch_to(1080,12);switch_to(720,12);switch_to(1440,11);
  request("POST","/record/start",200);
  request("POST","/capture/profile?height=1080",409);
  request("POST","/record/stop",200);
  request("POST","/capture/profile?height=2160",200);
  pbs_h264_profile_failed();
  request("GET","/config.json",200);
  assert(strstr(response,"\"height\":1440")&&strstr(response,"\"profile_error\":true"));
  /* Regression: a change that ends at another height (the automatic 1080p
   * fallback, or a rejected reopen that restarts at the previous size) must
   * resolve the transaction. Leaving it pending answers 409 to every later
   * request for the rest of the session. */
  assert(!pbs_h264_server_start(18092,2560,1440,sps,sizeof sps,pps,sizeof pps));
  request("POST","/capture/profile?height=2160",200);
  assert(pbs_h264_take_restart_request());
  int fallback=0;
  assert(pbs_h264_take_profile_request(&fallback)&&fallback==2160);
  assert(!pbs_h264_server_start(18092,1920,1080,sps,sizeof sps,pps,sizeof pps));
  request("GET","/config.json",200);
  assert(strstr(response,"\"height\":1080")&&strstr(response,"\"profile_changing\":false")&&strstr(response,"\"profile_error\":true"));
  /* The next request must be accepted, and a start at the requested size
   * confirms it and clears the failure flag. */
  request("POST","/capture/profile?height=2160",200);
  assert(pbs_h264_take_restart_request());
  int retry=0;
  assert(pbs_h264_take_profile_request(&retry)&&retry==2160);
  assert(!pbs_h264_server_start(18092,3840,2160,sps,sizeof sps,pps,sizeof pps));
  request("GET","/config.json",200);
  assert(strstr(response,"\"height\":2160")&&strstr(response,"\"profile_changing\":false")&&strstr(response,"\"profile_error\":false"));
  pbs_h264_server_stop();pbs_h264_server_wait();
  puts("Profile transitions, feature mapping, rollback state and recording conflicts verified");
}
