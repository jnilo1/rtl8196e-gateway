#ifndef TEST_OPENTHREAD_SYSTEM_H
#define TEST_OPENTHREAD_SYSTEM_H
#include <sys/select.h>
#include <sys/time.h>
typedef struct {
  fd_set mReadFdSet;
  fd_set mWriteFdSet;
  fd_set mErrorFdSet;
  int mMaxFd;
  struct timeval mTimeout;
} otSysMainloopContext;
void otSysMainloopUpdate(void *instance, otSysMainloopContext *context);
int otSysMainloopPoll(otSysMainloopContext *context);
void otSysMainloopProcess(void *instance, const otSysMainloopContext *context);
#endif
