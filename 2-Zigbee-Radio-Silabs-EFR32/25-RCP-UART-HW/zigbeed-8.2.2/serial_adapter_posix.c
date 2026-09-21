/*
 * POSIX transport adapter for the generated Zigbeed application.
 *
 * Supports the original PTY/serial endpoint and a single-client TCP listener.
 * It implements the EmberZNet 8.2.x serial adapter contract without using or
 * redistributing the Silicon Labs serial_adapter.c implementation.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "sl_zigbee_types.h"
#include "serial_adapter_posix.h"
#include "openthread/openthread-system.h"
#include "stack/include/sl_zigbee.h"
#include "app/framework/common/zigbee_app_framework_event.h"

#define INVALID_FD (-1)
#define IO_BUFFER_SIZE 512U
#define DEFAULT_SERIAL_PORT "/tmp/ttyZigbeed"
#define TCP_LISTEN_PREFIX "tcp-listen://"
#define TCP_LISTEN_FD_ENV "ZIGBEED_TCP_LISTEN_FD"
#define TCP_CLIENT_FD_ENV "ZIGBEED_TCP_CLIENT_FD"

typedef enum {
  TRANSPORT_PTY,
  TRANSPORT_TCP,
} transport_kind_t;

char serialPort[SERIAL_PORT_NAME_MAX_LEN] = DEFAULT_SERIAL_PORT;

static transport_kind_t transportKind = TRANSPORT_PTY;
static int serialFd = INVALID_FD;
static int listenFd = INVALID_FD;
static int clientFd = INVALID_FD;
static uint8_t outBuffer[IO_BUFFER_SIZE];
static size_t outOffset;
static size_t outLength;
static uint8_t inBuffer[IO_BUFFER_SIZE];
static size_t inOffset;
static size_t inLength;

#ifdef ZIGBEE_PRO_COMPLIANCE_ON_HOST
#include "sl_cli_threaded_host.h"
extern bool sli_cli_is_input_handled(void);
extern int sli_cli_get_pipe_read_fd(void);
#endif

static void close_fd(int *fd)
{
  if (*fd >= 0) {
    close(*fd);
    *fd = INVALID_FD;
  }
}

static void close_client(const char *reason)
{
  if (clientFd >= 0) {
    fprintf(stderr, "EZSP TCP client disconnected%s%s\n",
            reason == NULL ? "" : ": ", reason == NULL ? "" : reason);
    close_fd(&clientFd);
  }
  (void)unsetenv(TCP_CLIENT_FD_ENV);
  outOffset = 0;
  outLength = 0;
  inOffset = 0;
  inLength = 0;
}

static void transport_close(void)
{
  if (transportKind == TRANSPORT_PTY) {
    if (serialFd >= 0) {
      (void)tcflush(serialFd, TCIOFLUSH);
    }
    close_fd(&serialFd);
  } else {
    close_client(NULL);
    close_fd(&listenFd);
    (void)unsetenv(TCP_LISTEN_FD_ENV);
  }
}

static int set_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL);
  return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_inheritable(int fd)
{
  int flags = fcntl(fd, F_GETFD);
  return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
}

static int remember_tcp_fd(const char *name, int fd)
{
  char value[32];

  (void)snprintf(value, sizeof(value), "%d", fd);
  return setenv(name, value, 1);
}

static int inherited_tcp_fd(const char *name)
{
  const char *value = getenv(name);
  char *end;
  long fd;
  int type;
  socklen_t typeLength = sizeof(type);

  if (value == NULL || *value == '\0') {
    return INVALID_FD;
  }
  errno = 0;
  fd = strtol(value, &end, 10);
  if (errno != 0 || *end != '\0' || fd < 0 || fd > INT_MAX
      || fcntl((int)fd, F_GETFD) < 0
      || getsockopt((int)fd, SOL_SOCKET, SO_TYPE, &type, &typeLength) != 0
      || type != SOCK_STREAM) {
    (void)unsetenv(name);
    return INVALID_FD;
  }
  return (int)fd;
}

static int configure_pty(int fd)
{
  struct termios tios;

  if (tcgetattr(fd, &tios) != 0) {
    return -1;
  }
  cfmakeraw(&tios);
  return tcsetattr(fd, TCSAFLUSH, &tios);
}

static int parse_tcp_endpoint(char *host, size_t hostSize,
                              char *service, size_t serviceSize)
{
  const char *endpoint = serialPort + strlen(TCP_LISTEN_PREFIX);
  const char *port;
  size_t hostLength;
  char *end;
  unsigned long value;

  if (*endpoint == '[') {
    const char *closing = strchr(endpoint, ']');
    if (closing == NULL || closing[1] != ':') {
      return -1;
    }
    hostLength = (size_t)(closing - endpoint - 1);
    port = closing + 2;
    endpoint++;
  } else {
    port = strrchr(endpoint, ':');
    if (port == NULL || strchr(port + 1, ':') != NULL) {
      return -1;
    }
    hostLength = (size_t)(port - endpoint);
    port++;
  }

  if (*port == '\0' || hostLength >= hostSize || strlen(port) >= serviceSize) {
    return -1;
  }
  memcpy(host, endpoint, hostLength);
  host[hostLength] = '\0';
  if (host[0] == '\0') {
    (void)snprintf(host, hostSize, "%s", "127.0.0.1");
  }

  errno = 0;
  value = strtoul(port, &end, 10);
  if (errno != 0 || *end != '\0' || value == 0 || value > 65535UL) {
    return -1;
  }
  (void)snprintf(service, serviceSize, "%lu", value);
  return 0;
}

static sl_status_t open_tcp_listener(void)
{
  char host[NI_MAXHOST];
  char service[NI_MAXSERV];
  struct addrinfo hints;
  struct addrinfo *addresses = NULL;
  struct addrinfo *address;
  int error;

  if (parse_tcp_endpoint(host, sizeof(host), service, sizeof(service)) != 0) {
    fprintf(stderr, "Invalid EZSP TCP listener endpoint: %s\n", serialPort);
    return SL_STATUS_FAIL;
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  error = getaddrinfo(host, service, &hints, &addresses);
  if (error != 0) {
    fprintf(stderr, "Cannot resolve EZSP TCP listener %s: %s\n",
            serialPort, gai_strerror(error));
    return SL_STATUS_FAIL;
  }

  for (address = addresses; address != NULL; address = address->ai_next) {
    int one = 1;
    int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (set_inheritable(fd) != 0
        || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0
        || set_nonblocking(fd) != 0
        || bind(fd, address->ai_addr, address->ai_addrlen) != 0
        || listen(fd, 1) != 0
        || remember_tcp_fd(TCP_LISTEN_FD_ENV, fd) != 0) {
      close(fd);
      continue;
    }
    listenFd = fd;
    break;
  }
  freeaddrinfo(addresses);

  if (listenFd < 0) {
    fprintf(stderr, "Failed to listen on EZSP TCP endpoint %s: %s\n",
            serialPort, strerror(errno));
    return SL_STATUS_FAIL;
  }

  fprintf(stderr, "EZSP transport: TCP listen %s:%s\n", host, service);
  return SL_STATUS_OK;
}

static void accept_pending_clients(void)
{
  for (;;) {
    int fd = accept(listenFd, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    {
      int one = 1;
      if (set_inheritable(fd) != 0 || set_nonblocking(fd) != 0
          || setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        close(fd);
        continue;
      }
    }
    if (clientFd >= 0) {
      fprintf(stderr, "EZSP TCP client rejected: another client is active\n");
      close(fd);
      continue;
    }
    if (remember_tcp_fd(TCP_CLIENT_FD_ENV, fd) != 0) {
      close(fd);
      continue;
    }
    clientFd = fd;
    fprintf(stderr, "EZSP TCP client connected\n");
  }
}

static void write_flush(void)
{
  while (outOffset < outLength) {
    ssize_t written;
    size_t remaining = outLength - outOffset;

    if (transportKind == TRANSPORT_TCP) {
      if (clientFd < 0) {
        outOffset = 0;
        outLength = 0;
        return;
      }
      written = send(clientFd, outBuffer + outOffset, remaining, MSG_NOSIGNAL);
    } else {
      written = write(serialFd, outBuffer + outOffset, remaining);
    }

    if (written > 0) {
      outOffset += (size_t)written;
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    if (transportKind == TRANSPORT_TCP) {
      close_client(written == 0 ? "write closed" : strerror(errno));
    } else {
      outOffset = 0;
      outLength = 0;
    }
    return;
  }
  outOffset = 0;
  outLength = 0;
}

static sl_status_t read_available(uint16_t *count)
{
  ssize_t readCount;

  if (inOffset != inLength) {
    *count = (uint16_t)(inLength - inOffset);
    return SL_STATUS_OK;
  }

  inOffset = 0;
  inLength = 0;
  if (transportKind == TRANSPORT_TCP) {
    accept_pending_clients();
    if (clientFd < 0) {
      *count = 0;
      return SL_STATUS_EMPTY;
    }
  }

  do {
    readCount = read(transportKind == TRANSPORT_TCP ? clientFd : serialFd,
                     inBuffer, sizeof(inBuffer));
  } while (readCount < 0 && errno == EINTR);

  if (readCount > 0) {
    inLength = (size_t)readCount;
    *count = (uint16_t)inLength;
    return SL_STATUS_OK;
  }
  if (transportKind == TRANSPORT_TCP && (readCount == 0
      || (readCount < 0 && errno != EAGAIN && errno != EWOULDBLOCK))) {
    close_client(readCount == 0 ? "peer closed" : strerror(errno));
  }
  *count = 0;
  return SL_STATUS_EMPTY;
}

sl_status_t sli_legacy_serial_init(uint8_t port,
                                   SerialBaudRate rate,
                                   SerialParity parity,
                                   uint8_t stopBits)
{
  bool tcpRequested;

  (void)port;
  (void)rate;
  (void)parity;
  (void)stopBits;

  tcpRequested = strncmp(serialPort, TCP_LISTEN_PREFIX,
                          strlen(TCP_LISTEN_PREFIX)) == 0;

  /*
   * Zigbeed reinitializes its serial interface while handling an ASH reset.
   * A TCP peer owns that same ASH stream, so retaining an already healthy
   * listener and client is required for the peer to receive the reset reply.
   * PTY mode deliberately retains the original close-and-reopen behavior.
   */
  if (tcpRequested && transportKind == TRANSPORT_TCP && listenFd >= 0) {
    outOffset = 0;
    outLength = 0;
    inOffset = 0;
    inLength = 0;
    return SL_STATUS_OK;
  }

  transport_close();
  outOffset = 0;
  outLength = 0;
  inOffset = 0;
  inLength = 0;

  if (tcpRequested) {
    listenFd = inherited_tcp_fd(TCP_LISTEN_FD_ENV);
    clientFd = inherited_tcp_fd(TCP_CLIENT_FD_ENV);
    transportKind = TRANSPORT_TCP;
    if (listenFd >= 0) {
      fprintf(stderr, "EZSP transport: TCP listen resumed %s\n",
              serialPort + strlen(TCP_LISTEN_PREFIX));
      return SL_STATUS_OK;
    }
    if (clientFd >= 0) {
      close_client("listener was not inherited");
    }
    return open_tcp_listener();
  }

  transportKind = TRANSPORT_PTY;
  serialFd = open(serialPort, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (serialFd < 0 || configure_pty(serialFd) != 0) {
    fprintf(stderr, "Failed to open EZSP PTY %s: %s\n", serialPort, strerror(errno));
    close_fd(&serialFd);
    return SL_STATUS_FAIL;
  }
  fprintf(stderr, "EZSP transport: PTY %s\n", serialPort);
  return SL_STATUS_OK;
}

sl_status_t sli_legacy_serial_write_byte(uint8_t port, uint8_t dataByte)
{
  (void)port;
  if (outLength == sizeof(outBuffer)) {
    write_flush();
  }
  if (outLength == sizeof(outBuffer)) {
    return SL_STATUS_FAIL;
  }
  outBuffer[outLength++] = dataByte;
  write_flush();
  return SL_STATUS_OK;
}

uint16_t sli_legacy_serial_write_available(uint8_t port)
{
  (void)port;
  write_flush();
  return outLength < sizeof(outBuffer) ? 1U : 0U;
}

sl_status_t sli_legacy_serial_read_byte(uint8_t port, uint8_t *dataByte)
{
  uint16_t count;
  sl_status_t status;

  (void)port;
  status = read_available(&count);
  if (status == SL_STATUS_OK && count > 0U) {
    *dataByte = inBuffer[inOffset++];
  }
  return status;
}

sl_status_t sli_legacy_serial_write_string(uint8_t port, const char *string)
{
  (void)port;
  (void)string;
  return SL_STATUS_OK;
}

static void monitor_fd(int fd, otSysMainloopContext *mainloop, bool wantWrite)
{
  if (fd < 0) {
    return;
  }
  FD_SET(fd, &mainloop->mReadFdSet);
  FD_SET(fd, &mainloop->mErrorFdSet);
  if (wantWrite) {
    FD_SET(fd, &mainloop->mWriteFdSet);
  }
  if (fd > mainloop->mMaxFd) {
    mainloop->mMaxFd = fd;
  }
}

static void probe_client_disconnect(const otSysMainloopContext *mainloop)
{
  uint8_t byte;
  ssize_t result;

  if (clientFd < 0 || (!FD_ISSET(clientFd, &mainloop->mReadFdSet)
      && !FD_ISSET(clientFd, &mainloop->mErrorFdSet))) {
    return;
  }
  result = recv(clientFd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
  if (result == 0 || (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK
      && errno != EINTR)) {
    close_client(result == 0 ? "peer closed" : strerror(errno));
  }
}

void sli_serial_adapter_tick_callback(void)
{
  otSysMainloopContext mainloop;
  uint32_t timeoutMs = sl_zigbee_ms_to_next_stack_event();
  uint32_t appMs = sli_zigbee_af_ms_to_next_event();

  timeoutMs = timeoutMs < appMs ? timeoutMs : appMs;
  FD_ZERO(&mainloop.mReadFdSet);
  FD_ZERO(&mainloop.mWriteFdSet);
  FD_ZERO(&mainloop.mErrorFdSet);
  mainloop.mMaxFd = INVALID_FD;

  if (transportKind == TRANSPORT_TCP) {
    monitor_fd(listenFd, &mainloop, false);
    monitor_fd(clientFd, &mainloop, outLength != 0U);
  } else {
    monitor_fd(serialFd, &mainloop, outLength != 0U);
  }

#ifdef ZIGBEE_PRO_COMPLIANCE_ON_HOST
  {
    int pipeReadFd = sli_cli_get_pipe_read_fd();
    monitor_fd(pipeReadFd, &mainloop, false);
  }
#endif

  mainloop.mTimeout.tv_sec = timeoutMs / 1000U;
  mainloop.mTimeout.tv_usec = (timeoutMs % 1000U) * 1000U;
  otSysMainloopUpdate(NULL, &mainloop);

  if (otSysMainloopPoll(&mainloop) < 0) {
    if (errno != EINTR) {
      fprintf(stderr, "EZSP transport poll failed: %s\n", strerror(errno));
    }
    return;
  }

  if (transportKind == TRANSPORT_TCP) {
    if (listenFd >= 0 && (FD_ISSET(listenFd, &mainloop.mReadFdSet)
        || FD_ISSET(listenFd, &mainloop.mErrorFdSet))) {
      accept_pending_clients();
    }
    probe_client_disconnect(&mainloop);
    if (clientFd >= 0 && FD_ISSET(clientFd, &mainloop.mWriteFdSet)) {
      write_flush();
    }
  } else if (serialFd >= 0 && FD_ISSET(serialFd, &mainloop.mWriteFdSet)) {
    write_flush();
  }

  otSysMainloopProcess(NULL, &mainloop);
#ifdef ZIGBEE_PRO_COMPLIANCE_ON_HOST
  if (sli_cli_is_input_handled()) {
    char buffer[SL_CLI_THREADED_HOST_PIPE_DATA_LENGTH];
    assert(read(sli_cli_get_pipe_read_fd(), buffer, sizeof(buffer)) == (ssize_t)sizeof(buffer));
  }
#endif
}
