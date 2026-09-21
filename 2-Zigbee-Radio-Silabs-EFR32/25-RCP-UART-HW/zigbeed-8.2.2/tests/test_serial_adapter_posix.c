#include <assert.h>
#include <errno.h>
#include <pty.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>

#include "serial_adapter_posix.h"
#include "openthread/openthread-system.h"

uint32_t sl_zigbee_ms_to_next_stack_event(void) { return 1U; }
uint32_t sli_zigbee_af_ms_to_next_event(void) { return 1U; }
void otSysMainloopUpdate(void *instance, otSysMainloopContext *context) { (void)instance; (void)context; }
int otSysMainloopPoll(otSysMainloopContext *context)
{
  return select(context->mMaxFd + 1, &context->mReadFdSet, &context->mWriteFdSet,
                &context->mErrorFdSet, &context->mTimeout);
}
void otSysMainloopProcess(void *instance, const otSysMainloopContext *context)
{
  (void)instance;
  (void)context;
}

static int reserve_loopback_port(void)
{
  struct sockaddr_in address;
  socklen_t length = sizeof(address);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  assert(bind(fd, (const struct sockaddr *)&address, sizeof(address)) == 0);
  assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
  close(fd);
  return ntohs(address.sin_port);
}

static int connect_loopback(int port)
{
  struct sockaddr_in address;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((uint16_t)port);
  assert(connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0);
  return fd;
}

static uint8_t read_adapter_byte(void)
{
  uint8_t byte = 0;
  int attempts;
  for (attempts = 0; attempts < 10; ++attempts) {
    if (sli_legacy_serial_read_byte(1, &byte) == SL_STATUS_OK) {
      return byte;
    }
    sli_serial_adapter_tick_callback();
  }
  assert(!"adapter did not provide an expected byte");
  return 0;
}

static void test_pty(void)
{
  int master;
  int slave;
  char path[128];
  char byte;

  assert(openpty(&master, &slave, path, NULL, NULL) == 0);
  close(slave);
  snprintf(serialPort, SERIAL_PORT_NAME_MAX_LEN, "%s", path);
  assert(sli_legacy_serial_init(1, 0, 0, 1) == SL_STATUS_OK);
  assert(sli_legacy_serial_write_byte(1, 0x41) == SL_STATUS_OK);
  assert(read(master, &byte, 1) == 1 && byte == 0x41);
  assert(write(master, "B", 1) == 1);
  assert(read_adapter_byte() == 0x42);
  close(master);
}

static void test_tcp_lifecycle(void)
{
  int port = reserve_loopback_port();
  int first;
  int second;
  int replacement;
  char byte;

  snprintf(serialPort, SERIAL_PORT_NAME_MAX_LEN, "tcp-listen://127.0.0.1:%d", port);
  assert(sli_legacy_serial_init(1, 0, 0, 1) == SL_STATUS_OK);
  first = connect_loopback(port);
  sli_serial_adapter_tick_callback();
  assert(sli_legacy_serial_write_byte(1, 0x31) == SL_STATUS_OK);
  assert(read(first, &byte, 1) == 1 && byte == 0x31);

  /* Zigbeed reinitializes this interface while processing an ASH reset. */
  assert(sli_legacy_serial_init(1, 0, 0, 1) == SL_STATUS_OK);
  assert(write(first, "R", 1) == 1);
  assert(read_adapter_byte() == 0x52);
  assert(sli_legacy_serial_write_byte(1, 0x33) == SL_STATUS_OK);
  assert(read(first, &byte, 1) == 1 && byte == 0x33);

  second = connect_loopback(port);
  sli_serial_adapter_tick_callback();
  /* A locally queued write can succeed before the peer observes its close. */
  (void)send(second, "x", 1, MSG_NOSIGNAL);
  {
    fd_set readable;
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    FD_ZERO(&readable);
    FD_SET(second, &readable);
    assert(select(second + 1, &readable, NULL, NULL, &timeout) == 1);
    assert(recv(second, &byte, 1, 0) == 0);
  }
  assert(write(first, "A", 1) == 1);
  assert(read_adapter_byte() == 0x41);
  close(second);

  close(first);
  sli_serial_adapter_tick_callback();
  replacement = connect_loopback(port);
  sli_serial_adapter_tick_callback();
  assert(write(replacement, "B", 1) == 1);
  assert(read_adapter_byte() == 0x42);
  assert(sli_legacy_serial_write_byte(1, 0x32) == SL_STATUS_OK);
  assert(read(replacement, &byte, 1) == 1 && byte == 0x32);
  close(replacement);
}

int main(void)
{
  test_pty();
  test_tcp_lifecycle();
  puts("serial_adapter_posix tests passed");
  return EXIT_SUCCESS;
}
