/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Timo Teras  2009
 * Copyright (C) Miroslav Lichvar  2009, 2013-2019
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  This file implements socket operations.

  */

#include "config.h"

#include "sysincl.h"

#ifdef HAVE_LINUX_TIMESTAMPING
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#endif

#include "socket.h"
#include "array.h"
#include "logging.h"
#include "privops.h"
#include "util.h"

#define INVALID_SOCK_FD (-4)
#define CMSG_BUF_SIZE 256

union sockaddr_all {
  struct sockaddr_in in4;
#ifdef FEAT_IPV6
  struct sockaddr_in6 in6;
#endif
  struct sockaddr_un un;
  struct sockaddr sa;
};

struct Message {
  union sockaddr_all name;
  struct iovec iov;
  /* Buffer of sufficient length for all expected messages */
  union {
    NTP_Receive_Buffer ntp_msg;
    CMD_Request cmd_request;
    CMD_Request cmd_reply;
  } msg_buf;
  /* Aligned buffer for control messages */
  struct cmsghdr cmsg_buf[CMSG_BUF_SIZE / sizeof (struct cmsghdr)];
};

#ifdef HAVE_RECVMMSG
#define MAX_RECV_MESSAGES SCK_MAX_RECV_MESSAGES
#define MessageHeader mmsghdr
#else
/* Compatible with mmsghdr */
struct MessageHeader {
  struct msghdr msg_hdr;
  unsigned int msg_len;
};

#define MAX_RECV_MESSAGES 1
#endif

static int initialised;

/* Arrays of Message and MessageHeader */
static ARR_Instance recv_messages;
static ARR_Instance recv_headers;

static unsigned int received_messages;

static int (*priv_bind_function)(int sock_fd, struct sockaddr *address,
                                 socklen_t address_len);

/* ================================================== */

static void
prepare_buffers(unsigned int n)
{
  struct MessageHeader *hdr;
  struct Message *msg;
  unsigned int i;

  for (i = 0; i < n; i++) {
    msg = ARR_GetElement(recv_messages, i);
    hdr = ARR_GetElement(recv_headers, i);

    msg->iov.iov_base = &msg->msg_buf;
    msg->iov.iov_len = sizeof (msg->msg_buf);
    hdr->msg_hdr.msg_name = &msg->name;
    hdr->msg_hdr.msg_namelen = sizeof (msg->name);
    hdr->msg_hdr.msg_iov = &msg->iov;
    hdr->msg_hdr.msg_iovlen = 1;
    hdr->msg_hdr.msg_control = &msg->cmsg_buf;
    hdr->msg_hdr.msg_controllen = sizeof (msg->cmsg_buf);
    hdr->msg_hdr.msg_flags = 0;
    hdr->msg_len = 0;
  }
}

/* ================================================== */

static const char *
domain_to_string(int domain)
{
  switch (domain) {
    case AF_INET:
      return "IPv4";
#ifdef AF_INET6
    case AF_INET6:
      return "IPv6";
#endif
    case AF_UNIX:
      return "Unix";
    case AF_UNSPEC:
      return "UNSPEC";
    default:
      return "?";
  }
}

/* ================================================== */

static int
open_socket(int domain, int type, int flags)
{
  int sock_fd;

  sock_fd = socket(domain, type, 0);

  if (sock_fd < 0) {
    DEBUG_LOG("Could not open %s socket : %s",
              domain_to_string(domain), strerror(errno));
    return INVALID_SOCK_FD;
  }

  /* Close the socket automatically on exec */
  if (!UTI_FdSetCloexec(sock_fd)) {
    DEBUG_LOG("Could not set O_CLOEXEC : %s", strerror(errno));
    close(sock_fd);
    return INVALID_SOCK_FD;
  }

  /* Enable non-blocking mode if requested */
  if (flags & SCK_FLAG_NONBLOCK && fcntl(sock_fd, F_SETFL, O_NONBLOCK)) {
    DEBUG_LOG("Could not set O_NONBLOCK : %s", strerror(errno));
    close(sock_fd);
    return INVALID_SOCK_FD;
  }

  return sock_fd;
}

/* ================================================== */

static int
set_socket_options(int sock_fd, int flags)
{
  /* Make the socket capable of sending broadcast packets if requested */
  if (flags & SCK_FLAG_BROADCAST && !SCK_SetIntOption(sock_fd, SOL_SOCKET, SO_BROADCAST, 1))
    ;

  return 1;
}

/* ================================================== */

static int
set_ip_options(int sock_fd, int family, int flags)
{
#if defined(FEAT_IPV6) && defined(IPV6_V6ONLY)
  /* Receive only IPv6 packets on an IPv6 socket */
  if (family == IPADDR_INET6 && !SCK_SetIntOption(sock_fd, IPPROTO_IPV6, IPV6_V6ONLY, 1))
    return 0;
#endif

  /* Provide destination address of received packets if requested */
  if (flags & SCK_FLAG_RX_DEST_ADDR) {
    if (family == IPADDR_INET4) {
#ifdef HAVE_IN_PKTINFO
      if (!SCK_SetIntOption(sock_fd, IPPROTO_IP, IP_PKTINFO, 1))
        ;
#elif defined(IP_RECVDSTADDR)
      if (!SCK_SetIntOption(sock_fd, IPPROTO_IP, IP_RECVDSTADDR, 1))
        ;
#endif
    }
#ifdef FEAT_IPV6
    else if (family == IPADDR_INET6) {
#ifdef HAVE_IN6_PKTINFO
#ifdef IPV6_RECVPKTINFO
      if (!SCK_SetIntOption(sock_fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1))
        ;
#else
      if (!SCK_SetIntOption(sock_fd, IPPROTO_IPV6, IPV6_PKTINFO, 1))
        ;
#endif
#endif
    }
#endif
  }

  return 1;
}

/* ================================================== */

static int
bind_ip_address(int sock_fd, IPSockAddr *addr, int flags)
{
  union sockaddr_all saddr;
  socklen_t saddr_len;
  int s;

  /* Make the socket capable of re-using an old address if binding to a specific port */
  if (addr->port > 0 && !SCK_SetIntOption(sock_fd, SOL_SOCKET, SO_REUSEADDR, 1))
    ;

#ifdef IP_FREEBIND
  /* Allow binding to an address that doesn't exist yet */
  if (!SCK_SetIntOption(sock_fd, IPPROTO_IP, IP_FREEBIND, 1))
    ;
#endif

  saddr_len = SCK_IPSockAddrToSockaddr(addr, (struct sockaddr *)&saddr, sizeof (saddr));
  if (saddr_len == 0)
    return 0;

  if (flags & SCK_FLAG_PRIV_BIND && priv_bind_function)
    s = priv_bind_function(sock_fd, &saddr.sa, saddr_len);
  else
    s = bind(sock_fd, &saddr.sa, saddr_len);

  if (s < 0) {
    DEBUG_LOG("Could not bind socket to %s : %s",
              UTI_IPSockAddrToString(addr), strerror(errno));
    return 0;
  }

  return 1;
}

/* ================================================== */

static int
connect_ip_address(int sock_fd, IPSockAddr *addr)
{
  union sockaddr_all saddr;
  socklen_t saddr_len;

  saddr_len = SCK_IPSockAddrToSockaddr(addr, (struct sockaddr *)&saddr, sizeof (saddr));
  if (saddr_len == 0)
    return 0;

  if (connect(sock_fd, &saddr.sa, saddr_len) < 0) {
    DEBUG_LOG("Could not connect socket to %s : %s",
              UTI_IPSockAddrToString(addr), strerror(errno));
    return 0;
  }

  return 1;
}

/* ================================================== */

static int
open_ip_socket(IPSockAddr *remote_addr, IPSockAddr *local_addr, int type, int flags)
{
  int domain, family, sock_fd;

  if (local_addr)
    family = local_addr->ip_addr.family;
  else if (remote_addr)
    family = remote_addr->ip_addr.family;
  else
    family = IPADDR_INET4;

  switch (family) {
    case IPADDR_INET4:
      domain = AF_INET;
      break;
#ifdef FEAT_IPV6
    case IPADDR_INET6:
      domain = AF_INET6;
      break;
#endif
    default:
      DEBUG_LOG("Unspecified family");
      return INVALID_SOCK_FD;
  }

  sock_fd = open_socket(domain, type, flags);
  if (sock_fd < 0)
    return INVALID_SOCK_FD;

  if (!set_socket_options(sock_fd, flags))
    goto error;

  if (!set_ip_options(sock_fd, family, flags))
    goto error;

  /* Bind the socket if a local address was specified */
  if (local_addr && local_addr->ip_addr.family != IPADDR_UNSPEC &&
      !bind_ip_address(sock_fd, local_addr, flags))
    goto error;

  /* Connect the socket if a remote address was specified */
  if (remote_addr && remote_addr->ip_addr.family != IPADDR_UNSPEC &&
      !connect_ip_address(sock_fd, remote_addr))
    goto error;

  if (remote_addr || local_addr)
    DEBUG_LOG("Opened %s socket fd=%d%s%s%s%s",
              family == IPADDR_INET4 ? "IPv4" : "IPv6",
              sock_fd,
              remote_addr ? " remote=" : "",
              remote_addr ? UTI_IPSockAddrToString(remote_addr) : "",
              local_addr ? " local=" : "",
              local_addr ? UTI_IPSockAddrToString(local_addr) : "");

  return sock_fd;

error:
  SCK_CloseSocket(sock_fd);
  return INVALID_SOCK_FD;
}

/* ================================================== */

static int
bind_unix_address(int sock_fd, const char *addr, int flags)
{
  union sockaddr_all saddr;

  if (snprintf(saddr.un.sun_path, sizeof (saddr.un.sun_path), "%s", addr) >=
      sizeof (saddr.un.sun_path)) {
    DEBUG_LOG("Unix socket path %s too long", addr);
    return 0;
  }
  saddr.un.sun_family = AF_UNIX;

  unlink(addr);

  /* PRV_BindSocket() doesn't support Unix sockets yet */
  if (bind(sock_fd, &saddr.sa, sizeof (saddr.un)) < 0) {
    DEBUG_LOG("Could not bind Unix socket to %s : %s", addr, strerror(errno));
    return 0;
  }

  /* Allow access to everyone with access to the directory if requested */
  if (flags & SCK_FLAG_ALL_PERMISSIONS && chmod(addr, 0666) < 0) {
    DEBUG_LOG("Could not change permissions of %s : %s", addr, strerror(errno));
    return 0;
  }

  return 1;
}

/* ================================================== */

static int
connect_unix_address(int sock_fd, const char *addr)
{
  union sockaddr_all saddr;

  if (snprintf(saddr.un.sun_path, sizeof (saddr.un.sun_path), "%s", addr) >=
      sizeof (saddr.un.sun_path)) {
    DEBUG_LOG("Unix socket path %s too long", addr);
    return 0;
  }
  saddr.un.sun_family = AF_UNIX;

  if (connect(sock_fd, &saddr.sa, sizeof (saddr.un)) < 0) {
    DEBUG_LOG("Could not connect Unix socket to %s : %s", addr, strerror(errno));
    return 0;
  }

  return 1;
}

/* ================================================== */

static int
open_unix_socket(const char *remote_addr, const char *local_addr, int type, int flags)
{
  int sock_fd;

  sock_fd = open_socket(AF_UNIX, type, flags);
  if (sock_fd < 0)
    return INVALID_SOCK_FD;

  if (!set_socket_options(sock_fd, flags))
    goto error;

  /* Bind the socket if a local address was specified */
  if (local_addr && !bind_unix_address(sock_fd, local_addr, flags))
    goto error;

  /* Connect the socket if a remote address was specified */
  if (remote_addr && !connect_unix_address(sock_fd, remote_addr))
    goto error;

  DEBUG_LOG("Opened Unix socket fd=%d%s%s%s%s",
            sock_fd,
            remote_addr ? " remote=" : "", remote_addr ? remote_addr : "",
            local_addr ? " local=" : "", local_addr ? local_addr : "");

  return sock_fd;

error:
  SCK_RemoveSocket(sock_fd);
  SCK_CloseSocket(sock_fd);
  return INVALID_SOCK_FD;
}

/* ================================================== */

static int
get_recv_flags(int flags)
{
  int recv_flags = 0;

  if (flags & SCK_FLAG_MSG_ERRQUEUE) {
#ifdef MSG_ERRQUEUE
    recv_flags |= MSG_ERRQUEUE;
#else
    assert(0);
#endif
  }

  return recv_flags;
}

/* ================================================== */

static void
handle_recv_error(int sock_fd, int flags)
{
#ifdef MSG_ERRQUEUE
  /* If reading from the error queue failed, the select() exception should
     be for a socket error.  Clear the error to avoid a busy loop. */
  if (flags & SCK_FLAG_MSG_ERRQUEUE) {
    int error = 0;
    socklen_t len = sizeof (error);

    if (getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &error, &len))
      DEBUG_LOG("Could not get SO_ERROR");
    if (error)
      errno = error;
  }
#endif

  DEBUG_LOG("Could not receive message fd=%d : %s", sock_fd, strerror(errno));
}

/* ================================================== */

static void
log_message(int sock_fd, int direction, SCK_Message *message, const char *prefix,
            const char *error)
{
  const char *local_addr, *remote_addr;
  char if_index[20], tss[10], tsif[20], tslen[20];

  if (DEBUG <= 0 || log_min_severity < LOGS_DEBUG)
    return;

  remote_addr = NULL;
  local_addr = NULL;
  if_index[0] = '\0';
  tss[0] = '\0';
  tsif[0] = '\0';
  tslen[0] = '\0';

  switch (message->addr_type) {
    case SCK_ADDR_IP:
      if (message->remote_addr.ip.ip_addr.family != IPADDR_UNSPEC)
        remote_addr = UTI_IPSockAddrToString(&message->remote_addr.ip);
      if (message->local_addr.ip.family != IPADDR_UNSPEC) {
        local_addr = UTI_IPToString(&message->local_addr.ip);
      }
      break;
    case SCK_ADDR_UNIX:
      remote_addr = message->remote_addr.path;
      break;
    default:
      break;
  }

  if (message->if_index != INVALID_IF_INDEX)
    snprintf(if_index, sizeof (if_index), " if=%d", message->if_index);

  if (direction > 0) {
    if (!UTI_IsZeroTimespec(&message->timestamp.kernel) ||
        !UTI_IsZeroTimespec(&message->timestamp.hw))
      snprintf(tss, sizeof (tss), " tss=%s%s",
               !UTI_IsZeroTimespec(&message->timestamp.kernel) ? "K" : "",
               !UTI_IsZeroTimespec(&message->timestamp.hw) ? "H" : "");

    if (message->timestamp.if_index != INVALID_IF_INDEX)
      snprintf(tsif, sizeof (tsif), " tsif=%d", message->timestamp.if_index);

    if (message->timestamp.l2_length != 0)
      snprintf(tslen, sizeof (tslen), " tslen=%d", message->timestamp.l2_length);
  }

  DEBUG_LOG("%s message%s%s%s%s fd=%d len=%u%s%s%s%s%s%s",
            prefix,
            remote_addr ? (direction > 0 ? " from " : " to ") : "",
            remote_addr ? remote_addr : "",
            local_addr ? (direction > 0 ? " to " : " from ") : "",
            local_addr ? local_addr : "",
            sock_fd, message->length, if_index,
            tss, tsif, tslen,
            error ? " : " : "", error ? error : "");
}

/* ================================================== */

static void
init_message_addresses(SCK_Message *message, SCK_AddressType addr_type)
{
  message->addr_type = addr_type;

  switch (addr_type) {
    case SCK_ADDR_UNSPEC:
      break;
    case SCK_ADDR_IP:
      message->remote_addr.ip.ip_addr.family = IPADDR_UNSPEC;
      message->remote_addr.ip.port = 0;
      message->local_addr.ip.family = IPADDR_UNSPEC;
      break;
    case SCK_ADDR_UNIX:
      message->remote_addr.path = NULL;
      break;
    default:
      assert(0);
  }
}

/* ================================================== */

static void
init_message_nonaddress(SCK_Message *message)
{
  message->data = NULL;
  message->length = 0;
  message->if_index = INVALID_IF_INDEX;

  UTI_ZeroTimespec(&message->timestamp.kernel);
  UTI_ZeroTimespec(&message->timestamp.hw);
  message->timestamp.if_index = INVALID_IF_INDEX;
  message->timestamp.l2_length = 0;
  message->timestamp.tx_flags = 0;
}

/* ================================================== */

static int
process_header(struct msghdr *msg, unsigned int msg_length, int sock_fd, SCK_Message *message)
{
  struct cmsghdr *cmsg;

  if (msg->msg_iovlen != 1) {
    DEBUG_LOG("Unexpected iovlen");
    return 0;
  }

  if (msg->msg_namelen > sizeof (union sockaddr_all)) {
    DEBUG_LOG("Truncated source address");
    return 0;
  }

  if (msg->msg_namelen > sizeof (((struct sockaddr *)msg->msg_name)->sa_family)) {
    switch (((struct sockaddr *)msg->msg_name)->sa_family) {
      case AF_INET:
#ifdef FEAT_IPV6
      case AF_INET6:
#endif
        init_message_addresses(message, SCK_ADDR_IP);
        SCK_SockaddrToIPSockAddr(msg->msg_name, msg->msg_namelen, &message->remote_addr.ip);
        break;
      case AF_UNIX:
        init_message_addresses(message, SCK_ADDR_UNIX);
        message->remote_addr.path = ((struct sockaddr_un *)msg->msg_name)->sun_path;
        break;
      default:
        DEBUG_LOG("Unexpected address");
        return 0;
    }
  } else {
    init_message_addresses(message, SCK_ADDR_UNSPEC);
  }

  init_message_nonaddress(message);

  message->data = msg->msg_iov[0].iov_base;
  message->length = msg_length;

  if (msg->msg_flags & MSG_TRUNC) {
    log_message(sock_fd, 1, message, "Truncated", NULL);
    return 0;
  }

  if (msg->msg_flags & MSG_CTRUNC) {
    log_message(sock_fd, 1, message, "Truncated cmsg in", NULL);
    return 0;
  }

  for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
#ifdef HAVE_IN_PKTINFO
    if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
      struct in_pktinfo ipi;

      if (message->addr_type != SCK_ADDR_IP)
        init_message_addresses(message, SCK_ADDR_IP);

      memcpy(&ipi, CMSG_DATA(cmsg), sizeof (ipi));
      message->local_addr.ip.addr.in4 = ntohl(ipi.ipi_addr.s_addr);
      message->local_addr.ip.family = IPADDR_INET4;
      message->if_index = ipi.ipi_ifindex;
    }
#elif defined(IP_RECVDSTADDR)
    if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_RECVDSTADDR) {
      struct in_addr addr;

      if (message->addr_type != SCK_ADDR_IP)
        init_message_addresses(message, SCK_ADDR_IP);

      memcpy(&addr, CMSG_DATA(cmsg), sizeof (addr));
      message->local_addr.ip.addr.in4 = ntohl(addr.s_addr);
      message->local_addr.ip.family = IPADDR_INET4;
    }
#endif

#ifdef HAVE_IN6_PKTINFO
    if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
      struct in6_pktinfo ipi;

      if (message->addr_type != SCK_ADDR_IP)
        init_message_addresses(message, SCK_ADDR_IP);

      memcpy(&ipi, CMSG_DATA(cmsg), sizeof (ipi));
      memcpy(&message->local_addr.ip.addr.in6, &ipi.ipi6_addr.s6_addr,
             sizeof (message->local_addr.ip.addr.in6));
      message->local_addr.ip.family = IPADDR_INET6;
      message->if_index = ipi.ipi6_ifindex;
    }
#endif

#ifdef SCM_TIMESTAMP
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMP) {
      struct timeval tv;

      memcpy(&tv, CMSG_DATA(cmsg), sizeof (tv));
      UTI_TimevalToTimespec(&tv, &message->timestamp.kernel);
    }
#endif

#ifdef SCM_TIMESTAMPNS
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPNS) {
      memcpy(&message->timestamp.kernel, CMSG_DATA(cmsg), sizeof (message->timestamp.kernel));
    }
#endif

#ifdef HAVE_LINUX_TIMESTAMPING
#ifdef HAVE_LINUX_TIMESTAMPING_OPT_PKTINFO
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING_PKTINFO) {
      struct scm_ts_pktinfo ts_pktinfo;

      memcpy(&ts_pktinfo, CMSG_DATA(cmsg), sizeof (ts_pktinfo));
      message->timestamp.if_index = ts_pktinfo.if_index;
      message->timestamp.l2_length = ts_pktinfo.pkt_length;
    }
#endif

    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING) {
      struct scm_timestamping ts3;

      memcpy(&ts3, CMSG_DATA(cmsg), sizeof (ts3));
      message->timestamp.kernel = ts3.ts[0];
      message->timestamp.hw = ts3.ts[2];
    }

    if ((cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) ||
        (cmsg->cmsg_level == SOL_IPV6 && cmsg->cmsg_type == IPV6_RECVERR)) {
      struct sock_extended_err err;

      memcpy(&err, CMSG_DATA(cmsg), sizeof (err));

      if (err.ee_errno != ENOMSG || err.ee_info != SCM_TSTAMP_SND ||
          err.ee_origin != SO_EE_ORIGIN_TIMESTAMPING) {
        log_message(sock_fd, 1, message, "Unexpected extended error in", NULL);
        return 0;
      }
    }
#endif
  }

  return 1;
}

/* ================================================== */

static int
receive_messages(int sock_fd, SCK_Message *messages, int max_messages, int flags)
{
  struct MessageHeader *hdr;
  unsigned int i, n;
  int ret, recv_flags = 0;

  assert(initialised);

  if (max_messages < 1)
    return 0;

  /* Prepare used buffers for new messages */
  prepare_buffers(received_messages);
  received_messages = 0;

  hdr = ARR_GetElements(recv_headers);
  n = ARR_GetSize(recv_headers);
  n = MIN(n, max_messages);
  assert(n >= 1);

  recv_flags = get_recv_flags(flags);

#ifdef HAVE_RECVMMSG
  ret = recvmmsg(sock_fd, hdr, n, recv_flags | MSG_DONTWAIT, NULL);
  if (ret >= 0)
    n = ret;
#else
  n = 1;
  ret = recvmsg(sock_fd, &hdr[0].msg_hdr, recv_flags);
  if (ret >= 0)
    hdr[0].msg_len = ret;
#endif

  if (ret < 0) {
    handle_recv_error(sock_fd, flags);
    return 0;
  }

  received_messages = n;

  for (i = 0; i < n; i++) {
    hdr = ARR_GetElement(recv_headers, i);
    if (!process_header(&hdr->msg_hdr, hdr->msg_len, sock_fd, &messages[i]))
      return 0;

    log_message(sock_fd, 1, &messages[i],
                flags & SCK_FLAG_MSG_ERRQUEUE ? "Received error" : "Received", NULL);
  }

  return n;
}

/* ================================================== */

static void *
add_control_message(struct msghdr *msg, int level, int type, size_t length, size_t buf_length)
{
  struct cmsghdr *cmsg;
  size_t cmsg_space;

  /* Avoid using CMSG_NXTHDR as the one in glibc does not support adding
     control messages: https://sourceware.org/bugzilla/show_bug.cgi?id=13500 */

  cmsg = msg->msg_control;
  cmsg_space = CMSG_SPACE(length);

  if (!cmsg || length > buf_length || msg->msg_controllen + cmsg_space > buf_length) {
    DEBUG_LOG("Could not add control message level=%d type=%d", level, type);
    return NULL;
  }

  cmsg = (struct cmsghdr *)((char *)cmsg + msg->msg_controllen);

  memset(cmsg, 0, cmsg_space);

  cmsg->cmsg_level = level;
  cmsg->cmsg_type = type;
  cmsg->cmsg_len = CMSG_LEN(length);

  msg->msg_controllen += cmsg_space;

  return CMSG_DATA(cmsg);
}

/* ================================================== */

static int
send_message(int sock_fd, SCK_Message *message, int flags)
{
  struct cmsghdr cmsg_buf[CMSG_BUF_SIZE / sizeof (struct cmsghdr)];
  union sockaddr_all saddr;
  socklen_t saddr_len;
  struct msghdr msg;
  struct iovec iov;

  assert(flags == 0);

  switch (message->addr_type) {
    case SCK_ADDR_UNSPEC:
      saddr_len = 0;
      break;
    case SCK_ADDR_IP:
      saddr_len = SCK_IPSockAddrToSockaddr(&message->remote_addr.ip,
                                           (struct sockaddr *)&saddr, sizeof (saddr));
      break;
    case SCK_ADDR_UNIX:
      memset(&saddr, 0, sizeof (saddr));
      if (snprintf(saddr.un.sun_path, sizeof (saddr.un.sun_path), "%s",
                   message->remote_addr.path) >= sizeof (saddr.un.sun_path)) {
        DEBUG_LOG("Unix socket path %s too long", message->remote_addr.path);
        return 0;
      }
      saddr.un.sun_family = AF_UNIX;
      saddr_len = sizeof (saddr.un);
      break;
    default:
      assert(0);
  }

  if (saddr_len) {
    msg.msg_name = &saddr.un;
    msg.msg_namelen = saddr_len;
  } else {
    msg.msg_name = NULL;
    msg.msg_namelen = 0;
  }

  iov.iov_base = message->data;
  iov.iov_len = message->length;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;

  if (message->addr_type == SCK_ADDR_IP) {
    if (message->local_addr.ip.family == IPADDR_INET4) {
#ifdef HAVE_IN_PKTINFO
      struct in_pktinfo *ipi;

      ipi = add_control_message(&msg, IPPROTO_IP, IP_PKTINFO, sizeof (*ipi),
                                sizeof (cmsg_buf));
      if (!ipi)
        return 0;

      ipi->ipi_spec_dst.s_addr = htonl(message->local_addr.ip.addr.in4);
      if (message->if_index != INVALID_IF_INDEX)
        ipi->ipi_ifindex = message->if_index;

#elif defined(IP_SENDSRCADDR)
      struct in_addr *addr;

      addr = add_control_message(&msg, IPPROTO_IP, IP_SENDSRCADDR, sizeof (*addr),
                                 sizeof (cmsg_buf));
      if (!addr)
        return 0;

      addr->s_addr = htonl(message->local_addr.ip.addr.in4);
#endif
    }

#ifdef HAVE_IN6_PKTINFO
    if (message->local_addr.ip.family == IPADDR_INET6) {
      struct in6_pktinfo *ipi;

      ipi = add_control_message(&msg, IPPROTO_IPV6, IPV6_PKTINFO, sizeof (*ipi),
                                sizeof (cmsg_buf));
      if (!ipi)
        return 0;

      memcpy(&ipi->ipi6_addr.s6_addr, &message->local_addr.ip.addr.in6,
             sizeof(ipi->ipi6_addr.s6_addr));
      if (message->if_index != INVALID_IF_INDEX)
        ipi->ipi6_ifindex = message->if_index;
    }
#endif
  }

#ifdef HAVE_LINUX_TIMESTAMPING
  if (message->timestamp.tx_flags) {
    int *ts_tx_flags;

    /* Set timestamping flags for this message */

    ts_tx_flags = add_control_message(&msg, SOL_SOCKET, SO_TIMESTAMPING,
                                      sizeof (*ts_tx_flags), sizeof (cmsg_buf));
    if (!ts_tx_flags)
      return 0;

    *ts_tx_flags = message->timestamp.tx_flags;
  }
#endif

  /* This is apparently required on some systems */
  if (msg.msg_controllen == 0)
    msg.msg_control = NULL;

  if (sendmsg(sock_fd, &msg, 0) < 0) {
    log_message(sock_fd, -1, message, "Could not send", strerror(errno));
    return 0;
  }

  log_message(sock_fd, -1, message, "Sent", NULL);

  return 1;
}

/* ================================================== */

void
SCK_Initialise(void)
{
  recv_messages = ARR_CreateInstance(sizeof (struct Message));
  ARR_SetSize(recv_messages, MAX_RECV_MESSAGES);
  recv_headers = ARR_CreateInstance(sizeof (struct MessageHeader));
  ARR_SetSize(recv_headers, MAX_RECV_MESSAGES);

  received_messages = MAX_RECV_MESSAGES;

  priv_bind_function = NULL;

  initialised = 1;
}

/* ================================================== */

void
SCK_Finalise(void)
{
  ARR_DestroyInstance(recv_headers);
  ARR_DestroyInstance(recv_messages);

  initialised = 0;
}

/* ================================================== */

void
SCK_GetAnyLocalIPAddress(int family, IPAddr *local_addr)
{
  local_addr->family = family;

  switch (family) {
    case IPADDR_INET4:
      local_addr->addr.in4 = INADDR_ANY;
      break;
    case IPADDR_INET6:
#ifdef FEAT_IPV6
      memcpy(&local_addr->addr.in6, &in6addr_any, sizeof (local_addr->addr.in6));
#else
      memset(&local_addr->addr.in6, 0, sizeof (local_addr->addr.in6));
#endif
      break;
  }
}

/* ================================================== */

void
SCK_GetLoopbackIPAddress(int family, IPAddr *local_addr)
{
  local_addr->family = family;

  switch (family) {
    case IPADDR_INET4:
      local_addr->addr.in4 = INADDR_LOOPBACK;
      break;
    case IPADDR_INET6:
#ifdef FEAT_IPV6
      memcpy(&local_addr->addr.in6, &in6addr_loopback, sizeof (local_addr->addr.in6));
#else
      memset(&local_addr->addr.in6, 0, sizeof (local_addr->addr.in6));
      local_addr->addr.in6[15] = 1;
#endif
      break;
  }
}

/* ================================================== */

void
SCK_SetPrivBind(int (*function)(int sock_fd, struct sockaddr *address,
                                socklen_t address_len))
{
  priv_bind_function = function;
}

/* ================================================== */

int
SCK_OpenUdpSocket(IPSockAddr *remote_addr, IPSockAddr *local_addr, int flags)
{
  return open_ip_socket(remote_addr, local_addr, SOCK_DGRAM, flags);
}

/* ================================================== */

int
SCK_OpenUnixDatagramSocket(const char *remote_addr, const char *local_addr, int flags)
{
  return open_unix_socket(remote_addr, local_addr, SOCK_DGRAM, flags);
}

/* ================================================== */

int
SCK_OpenUnixStreamSocket(const char *remote_addr, const char *local_addr, int flags)
{
  return open_unix_socket(remote_addr, local_addr, SOCK_STREAM, flags);
}

/* ================================================== */

int
SCK_SetIntOption(int sock_fd, int level, int name, int value)
{
  if (setsockopt(sock_fd, level, name, &value, sizeof (value)) < 0) {
    DEBUG_LOG("setsockopt() failed fd=%d level=%d name=%d value=%d : %s",
              sock_fd, level, name, value, strerror(errno));
    return 0;
  }

  return 1;
}

/* ================================================== */

int
SCK_EnableKernelRxTimestamping(int sock_fd)
{
#ifdef SO_TIMESTAMPNS
  if (SCK_SetIntOption(sock_fd, SOL_SOCKET, SO_TIMESTAMPNS, 1))
    return 1;
#endif
#ifdef SO_TIMESTAMP
  if (SCK_SetIntOption(sock_fd, SOL_SOCKET, SO_TIMESTAMP, 1))
    return 1;
#endif

  return 0;
}

/* ================================================== */

int
SCK_Receive(int sock_fd, void *buffer, unsigned int length, int flags)
{
  int r;

  r = recv(sock_fd, buffer, length, get_recv_flags(flags));

  if (r < 0) {
    handle_recv_error(sock_fd, flags);
    return r;
  }

  DEBUG_LOG("Received data fd=%d len=%d", sock_fd, r);

  return r;
}

/* ================================================== */

int
SCK_Send(int sock_fd, void *buffer, unsigned int length, int flags)
{
  int r;

  assert(flags == 0);

  r = send(sock_fd, buffer, length, 0);

  if (r < 0) {
    DEBUG_LOG("Could not send data fd=%d len=%u : %s", sock_fd, length, strerror(errno));
    return r;
  }

  DEBUG_LOG("Sent data fd=%d len=%d", sock_fd, r);

  return r;
}

/* ================================================== */

int
SCK_ReceiveMessage(int sock_fd, SCK_Message *message, int flags)
{
  return SCK_ReceiveMessages(sock_fd, message, 1, flags);
}

/* ================================================== */

int
SCK_ReceiveMessages(int sock_fd, SCK_Message *messages, int max_messages, int flags)
{
  return receive_messages(sock_fd, messages, max_messages, flags);
}

/* ================================================== */

void
SCK_InitMessage(SCK_Message *message, SCK_AddressType addr_type)
{
  init_message_addresses(message, addr_type);
  init_message_nonaddress(message);
}

/* ================================================== */

int
SCK_SendMessage(int sock_fd, SCK_Message *message, int flags)
{
  return send_message(sock_fd, message, flags);
}

/* ================================================== */

int
SCK_RemoveSocket(int sock_fd)
{
  union sockaddr_all saddr;
  socklen_t saddr_len;

  saddr_len = sizeof (saddr);

  if (getsockname(sock_fd, &saddr.sa, &saddr_len) < 0) {
    DEBUG_LOG("getsockname() failed : %s", strerror(errno));
    return 0;
  }

  if (saddr_len > sizeof (saddr) || saddr_len <= sizeof (saddr.sa.sa_family) ||
      saddr.sa.sa_family != AF_UNIX)
    return 0;

  if (unlink(saddr.un.sun_path) < 0) {
    DEBUG_LOG("unlink(%s) failed : %s", saddr.un.sun_path, strerror(errno));
    return 0;
  }

  DEBUG_LOG("Removed %s", saddr.un.sun_path);

  return 1;
}

/* ================================================== */

void
SCK_CloseSocket(int sock_fd)
{
  close(sock_fd);
}

/* ================================================== */

void
SCK_SockaddrToIPSockAddr(struct sockaddr *sa, int sa_length, IPSockAddr *ip_sa)
{
  ip_sa->ip_addr.family = IPADDR_UNSPEC;
  ip_sa->port = 0;

  switch (sa->sa_family) {
    case AF_INET:
      if (sa_length < sizeof (struct sockaddr_in))
        return;
      ip_sa->ip_addr.family = IPADDR_INET4;
      ip_sa->ip_addr.addr.in4 = ntohl(((struct sockaddr_in *)sa)->sin_addr.s_addr);
      ip_sa->port = ntohs(((struct sockaddr_in *)sa)->sin_port);
      break;
#ifdef FEAT_IPV6
    case AF_INET6:
      if (sa_length < sizeof (struct sockaddr_in6))
        return;
      ip_sa->ip_addr.family = IPADDR_INET6;
      memcpy(&ip_sa->ip_addr.addr.in6, ((struct sockaddr_in6 *)sa)->sin6_addr.s6_addr,
             sizeof (ip_sa->ip_addr.addr.in6));
      ip_sa->port = ntohs(((struct sockaddr_in6 *)sa)->sin6_port);
      break;
#endif
    default:
      break;
  }
}

/* ================================================== */

int
SCK_IPSockAddrToSockaddr(IPSockAddr *ip_sa, struct sockaddr *sa, int sa_length)
{
  switch (ip_sa->ip_addr.family) {
    case IPADDR_INET4:
      if (sa_length < sizeof (struct sockaddr_in))
        return 0;
      memset(sa, 0, sizeof (struct sockaddr_in));
      sa->sa_family = AF_INET;
      ((struct sockaddr_in *)sa)->sin_addr.s_addr = htonl(ip_sa->ip_addr.addr.in4);
      ((struct sockaddr_in *)sa)->sin_port = htons(ip_sa->port);
#ifdef SIN6_LEN
      ((struct sockaddr_in *)sa)->sin_len = sizeof (struct sockaddr_in);
#endif
      return sizeof (struct sockaddr_in);
#ifdef FEAT_IPV6
    case IPADDR_INET6:
      if (sa_length < sizeof (struct sockaddr_in6))
        return 0;
      memset(sa, 0, sizeof (struct sockaddr_in6));
      sa->sa_family = AF_INET6;
      memcpy(&((struct sockaddr_in6 *)sa)->sin6_addr.s6_addr, ip_sa->ip_addr.addr.in6,
             sizeof (((struct sockaddr_in6 *)sa)->sin6_addr.s6_addr));
      ((struct sockaddr_in6 *)sa)->sin6_port = htons(ip_sa->port);
#ifdef SIN6_LEN
      ((struct sockaddr_in6 *)sa)->sin6_len = sizeof (struct sockaddr_in6);
#endif
      return sizeof (struct sockaddr_in6);
#endif
    default:
      if (sa_length < sizeof (struct sockaddr))
        return 0;
      memset(sa, 0, sizeof (struct sockaddr));
      sa->sa_family = AF_UNSPEC;
      return 0;
  }
}