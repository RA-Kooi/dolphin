// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Originally written by Sven Peter <sven@fail0verflow.com> for anergistic.

#include "Core/PowerPC/GDBStub.h"

#include <fmt/format.h>
#include <optional>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <WinSock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
typedef SSIZE_T ssize_t;
#define SHUT_RDWR SD_BOTH
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "Common/Event.h"
#include "Common/Logging/Log.h"
#include "Common/SocketContext.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCCache.h"
#include "Core/PowerPC/PowerPC.h"

namespace GDBStub
{
std::optional<Common::SocketContext> s_socket_context;

#define GDB_BFR_MAX 10000

#define GDB_STUB_START '$'
#define GDB_STUB_END '#'
#define GDB_STUB_ACK '+'
#define GDB_STUB_NAK '-'

// We are treating software breakpoints and hardware breakpoints the same way
enum class BreakpointType
{
  ExecuteSoft = 0,
  ExecuteHard,
  Read,
  Write,
  Access,
};

const s64 GDB_UPDATE_CYCLES = 100000;

static bool s_has_control = false;

static int s_tmpsock = -1;
static int s_sock = -1;

static u8 s_cmd_bfr[GDB_BFR_MAX];
static u32 s_cmd_len;

static CoreTiming::EventType* s_update_event;

static const char* CommandBufferAsString()
{
  return reinterpret_cast<const char*>(s_cmd_bfr);
}

// private helpers
static u8 Hex2char(u8 hex)
{
  if (hex >= '0' && hex <= '9')
    return hex - '0';
  else if (hex >= 'a' && hex <= 'f')
    return hex - 'a' + 0xa;
  else if (hex >= 'A' && hex <= 'F')
    return hex - 'A' + 0xa;

  ERROR_LOG_FMT(GDB_STUB, "Invalid nibble: {} ({:02x})", static_cast<char>(hex), hex);
  return 0;
}

static u8 Nibble2hex(u8 n)
{
  n &= 0xf;
  if (n < 0xa)
    return '0' + n;
  else
    return 'A' + n - 0xa;
}

static void Mem2hex(u8* dst, u8* src, u32 len)
{
  while (len-- > 0)
  {
    const u8 tmp = *src++;
    *dst++ = Nibble2hex(tmp >> 4);
    *dst++ = Nibble2hex(tmp);
  }
}

static void Hex2mem(u8* dst, u8* src, u32 len)
{
  while (len-- > 0)
  {
    *dst++ = (Hex2char(*src) << 4) | Hex2char(*(src + 1));
    src += 2;
  }
}

static void UpdateCallback(u64 userdata, s64 cycles_late)
{
  ProcessCommands(false);
  if (IsActive())
    CoreTiming::ScheduleEvent(GDB_UPDATE_CYCLES, s_update_event);
}

static u8 ReadByte()
{
  u8 c = '+';

  const ssize_t res = recv(s_sock, (char*)&c, 1, MSG_WAITALL);
  if (res != 1)
  {
    ERROR_LOG_FMT(GDB_STUB, "recv failed : {}", res);
    Deinit();
  }

  return c;
}

static u8 CalculateChecksum()
{
  u32 len = s_cmd_len;
  u8* ptr = s_cmd_bfr;
  u8 c = 0;

  while (len-- > 0)
    c += *ptr++;

  return c;
}

static void RemoveBreakpoint(BreakpointType type, u32 addr, u32 len)
{
  if (type == BreakpointType::ExecuteHard || type == BreakpointType::ExecuteSoft)
  {
    while (PowerPC::breakpoints.IsAddressBreakPoint(addr))
    {
      PowerPC::breakpoints.Remove(addr);
      INFO_LOG_FMT(GDB_STUB, "gdb: removed a breakpoint: {:08x} bytes at {:08x}", len, addr);
    }
  }
  else
  {
    while (PowerPC::memchecks.GetMemCheck(addr, len) != nullptr)
    {
      PowerPC::memchecks.Remove(addr);
      INFO_LOG_FMT(GDB_STUB, "gdb: removed a memcheck: {:08x} bytes at {:08x}", len, addr);
    }
  }
}

static void Nack()
{
  const char nak = GDB_STUB_NAK;
  const ssize_t res = send(s_sock, &nak, 1, 0);

  if (res != 1)
    ERROR_LOG_FMT(GDB_STUB, "send failed");
}

static void Ack()
{
  const char ack = GDB_STUB_ACK;
  const ssize_t res = send(s_sock, &ack, 1, 0);

  if (res != 1)
    ERROR_LOG_FMT(GDB_STUB, "send failed");
}

static void ReadCommand()
{
  s_cmd_len = 0;
  memset(s_cmd_bfr, 0, sizeof s_cmd_bfr);

  u8 c = ReadByte();
  if (c == '+')
  {
    // ignore ack
    return;
  }
  else if (c == 0x03)
  {
    CPU::Break();
    SendSignal(Signal::Sigtrap);
    s_has_control = true;
    INFO_LOG_FMT(GDB_STUB, "gdb: CPU::Break due to break command");
    return;
  }
  else if (c != GDB_STUB_START)
  {
    WARN_LOG_FMT(GDB_STUB, "gdb: read invalid byte {:02x}", c);
    return;
  }

  while ((c = ReadByte()) != GDB_STUB_END)
  {
    s_cmd_bfr[s_cmd_len++] = c;
    if (s_cmd_len == sizeof s_cmd_bfr)
    {
      ERROR_LOG_FMT(GDB_STUB, "gdb: cmd_bfr overflow");
      Nack();
      return;
    }
  }

  u8 chk_read = Hex2char(ReadByte()) << 4;
  chk_read |= Hex2char(ReadByte());

  const u8 chk_calc = CalculateChecksum();

  if (chk_calc != chk_read)
  {
    ERROR_LOG_FMT(GDB_STUB,
                  "gdb: invalid checksum: calculated {:02x} and read {:02x} for ${}# (length: {})",
                  chk_calc, chk_read, CommandBufferAsString(), s_cmd_len);
    s_cmd_len = 0;

    Nack();
    return;
  }

  DEBUG_LOG_FMT(GDB_STUB, "gdb: read command {} with a length of {}: {}",
                static_cast<char>(s_cmd_bfr[0]), s_cmd_len, CommandBufferAsString());
  Ack();
}

static bool IsDataAvailable()
{
  struct timeval t;
  fd_set _fds, *fds = &_fds;

  FD_ZERO(fds);
  FD_SET(s_sock, fds);

  t.tv_sec = 0;
  t.tv_usec = 20;

  if (select(s_sock + 1, fds, nullptr, nullptr, &t) < 0)
  {
    ERROR_LOG_FMT(GDB_STUB, "select failed");
    return false;
  }

  if (FD_ISSET(s_sock, fds))
    return true;
  return false;
}

static void SendReply(const char* reply)
{
  if (!IsActive())
    return;

  memset(s_cmd_bfr, 0, sizeof s_cmd_bfr);

  s_cmd_len = (u32)strlen(reply);
  if (s_cmd_len + 4 > sizeof s_cmd_bfr)
    ERROR_LOG_FMT(GDB_STUB, "cmd_bfr overflow in gdb_reply");

  memcpy(s_cmd_bfr + 1, reply, s_cmd_len);

  s_cmd_len++;
  const u8 chk = CalculateChecksum();
  s_cmd_len--;
  s_cmd_bfr[0] = GDB_STUB_START;
  s_cmd_bfr[s_cmd_len + 1] = GDB_STUB_END;
  s_cmd_bfr[s_cmd_len + 2] = Nibble2hex(chk >> 4);
  s_cmd_bfr[s_cmd_len + 3] = Nibble2hex(chk);

  DEBUG_LOG_FMT(GDB_STUB, "gdb: reply (len: {}): {}", s_cmd_len, CommandBufferAsString());

  const char* ptr = (const char*)s_cmd_bfr;
  u32 left = s_cmd_len + 4;
  while (left > 0)
  {
    const int n = send(s_sock, ptr, left, 0);
    if (n < 0)
    {
      ERROR_LOG_FMT(GDB_STUB, "gdb: send failed");
      return Deinit();
    }
    left -= n;
    ptr += n;
  }
}

static void HandleQuery()
{
  DEBUG_LOG_FMT(GDB_STUB, "gdb: query '{}'", CommandBufferAsString() + 1);

  if (!strcmp((const char*)(s_cmd_bfr + 1), "TStatus"))
  {
    return SendReply("T0");
  }

  SendReply("");
}

static void HandleSetThread()
{
  if (memcmp(s_cmd_bfr, "Hg0", 3) == 0 || memcmp(s_cmd_bfr, "Hc-1", 4) == 0 ||
      memcmp(s_cmd_bfr, "Hc0", 3) == 0 || memcmp(s_cmd_bfr, "Hc1", 3) == 0)
    return SendReply("OK");
  SendReply("E01");
}

static void HandleIsThreadAlive()
{
  if (memcmp(s_cmd_bfr, "T0", 2) == 0 || memcmp(s_cmd_bfr, "T1", 4) == 0 ||
      memcmp(s_cmd_bfr, "T-1", 3) == 0)
    return SendReply("OK");
  SendReply("E01");
}

static void wbe32hex(u8* p, u32 v)
{
  u32 i;
  for (i = 0; i < 8; i++)
    p[i] = Nibble2hex(v >> (28 - 4 * i));
}

static void wbe64hex(u8* p, u64 v)
{
  u32 i;
  for (i = 0; i < 16; i++)
    p[i] = Nibble2hex(v >> (60 - 4 * i));
}

static u32 re32hex(u8* p)
{
  u32 i;
  u32 res = 0;

  for (i = 0; i < 8; i++)
    res = (res << 4) | Hex2char(p[i]);

  return res;
}

static u64 re64hex(u8* p)
{
  u32 i;
  u64 res = 0;

  for (i = 0; i < 16; i++)
    res = (res << 4) | Hex2char(p[i]);

  return res;
}

static void ReadRegister()
{
  static u8 reply[64];
  u32 id;

  memset(reply, 0, sizeof reply);
  id = Hex2char(s_cmd_bfr[1]);
  if (s_cmd_bfr[2] != '\0')
  {
    id <<= 4;
    id |= Hex2char(s_cmd_bfr[2]);
  }

  if (id < 32)
  {
    wbe32hex(reply, GPR(id));
  }
  else if (id >= 32 && id < 64)
  {
    wbe64hex(reply, rPS(id - 32).PS0AsU64());
  }
  else
  {
    switch (id)
    {
    case 64:
      wbe32hex(reply, PC);
      break;
    case 65:
      wbe32hex(reply, MSR.Hex);
      break;
    case 66:
      wbe32hex(reply, PowerPC::ppcState.cr.Get());
      break;
    case 67:
      wbe32hex(reply, LR);
      break;
    case 68:
      wbe32hex(reply, CTR);
      break;
    case 69:
      wbe32hex(reply, PowerPC::ppcState.spr[SPR_XER]);
      break;
    case 70:
      wbe32hex(reply, 0x0BADC0DE);
      break;
    case 71:
      wbe32hex(reply, FPSCR.Hex);
      break;
    default:
      return SendReply("E01");
      break;
    }
  }

  SendReply((char*)reply);
}

static void ReadRegisters()
{
  static u8 bfr[GDB_BFR_MAX - 4];
  u8* bufptr = bfr;
  u32 i;

  memset(bfr, 0, sizeof bfr);

  for (i = 0; i < 32; i++)
  {
    wbe32hex(bufptr + i * 8, GPR(i));
  }
  bufptr += 32 * 8;

  SendReply((char*)bfr);
}

static void WriteRegisters()
{
  u32 i;
  u8* bufptr = s_cmd_bfr;

  for (i = 0; i < 32; i++)
  {
    GPR(i) = re32hex(bufptr + i * 8);
  }
  bufptr += 32 * 8;

  SendReply("OK");
}

static void WriteRegister()
{
  u32 id;

  u8* bufptr = s_cmd_bfr + 3;

  id = Hex2char(s_cmd_bfr[1]);
  if (s_cmd_bfr[2] != '=')
  {
    ++bufptr;
    id <<= 4;
    id |= Hex2char(s_cmd_bfr[2]);
  }

  if (id < 32)
  {
    GPR(id) = re32hex(bufptr);
  }
  else if (id >= 32 && id < 64)
  {
    rPS(id - 32).SetPS0(re64hex(bufptr));
  }
  else
  {
    switch (id)
    {
    case 64:
      PC = re32hex(bufptr);
      break;
    case 65:
      MSR.Hex = re32hex(bufptr);
      break;
    case 66:
      PowerPC::ppcState.cr.Set(re32hex(bufptr));
      break;
    case 67:
      LR = re32hex(bufptr);
      break;
    case 68:
      CTR = re32hex(bufptr);
      break;
    case 69:
      PowerPC::ppcState.spr[SPR_XER] = re32hex(bufptr);
      break;
    case 70:
      // do nothing, we dont have MQ
      break;
    case 71:
      FPSCR.Hex = re32hex(bufptr);
      break;
    default:
      return SendReply("E01");
      break;
    }
  }

  SendReply("OK");
}

static void ReadMemory()
{
  static u8 reply[GDB_BFR_MAX - 4];
  u32 addr, len;
  u32 i;

  i = 1;
  addr = 0;
  while (s_cmd_bfr[i] != ',')
    addr = (addr << 4) | Hex2char(s_cmd_bfr[i++]);
  i++;

  len = 0;
  while (i < s_cmd_len)
    len = (len << 4) | Hex2char(s_cmd_bfr[i++]);
  INFO_LOG_FMT(GDB_STUB, "gdb: read memory: {:08x} bytes from {:08x}", len, addr);

  if (len * 2 > sizeof reply)
    SendReply("E01");
  u8* data = Memory::GetPointer(addr);
  if (!data)
    return SendReply("E0");
  Mem2hex(reply, data, len);
  reply[len * 2] = '\0';
  SendReply((char*)reply);
}

static void WriteMemory()
{
  u32 addr, len;
  u32 i;

  i = 1;
  addr = 0;
  while (s_cmd_bfr[i] != ',')
    addr = (addr << 4) | Hex2char(s_cmd_bfr[i++]);
  i++;

  len = 0;
  while (s_cmd_bfr[i] != ':')
    len = (len << 4) | Hex2char(s_cmd_bfr[i++]);
  INFO_LOG_FMT(GDB_STUB, "gdb: write memory: {:08x} bytes to {:08x}", len, addr);

  u8* dst = Memory::GetPointer(addr);
  if (!dst)
    return SendReply("E00");
  Hex2mem(dst, s_cmd_bfr + i + 1, len);
  SendReply("OK");
}

static void Step()
{
  CPU::EnableStepping(true);
  Core::CallOnStateChangedCallbacks(Core::State::Paused);
}

static bool AddBreakpoint(BreakpointType type, u32 addr, u32 len)
{
  if (type == BreakpointType::ExecuteHard || type == BreakpointType::ExecuteSoft)
  {
    PowerPC::breakpoints.Add(addr);
    INFO_LOG_FMT(GDB_STUB, "gdb: added {} breakpoint: {:08x} bytes at {:08x}", type, len, addr);
  }
  else
  {
    TMemCheck new_memcheck;
    new_memcheck.start_address = addr;
    new_memcheck.end_address = addr + len - 1;
    new_memcheck.is_ranged = (len > 1);
    new_memcheck.is_break_on_read =
        (type == BreakpointType::Read || type == BreakpointType::Access);
    new_memcheck.is_break_on_write =
        (type == BreakpointType::Write || type == BreakpointType::Access);
    new_memcheck.break_on_hit = true;
    new_memcheck.log_on_hit = false;
    new_memcheck.is_enabled = true;
    PowerPC::memchecks.Add(new_memcheck);
    INFO_LOG_FMT(GDB_STUB, "gdb: added {} memcheck: {:08x} bytes at {:08x}", type, len, addr);
  }
  return true;
}

static void HandleAddBreakpoint()
{
  u32 type;
  u32 i, addr = 0, len = 0;

  type = Hex2char(s_cmd_bfr[1]);
  if (type > 4)
    return SendReply("E01");

  i = 3;
  while (s_cmd_bfr[i] != ',')
    addr = addr << 4 | Hex2char(s_cmd_bfr[i++]);
  i++;

  while (i < s_cmd_len)
    len = len << 4 | Hex2char(s_cmd_bfr[i++]);

  if (!AddBreakpoint(static_cast<BreakpointType>(type), addr, len))
    return SendReply("E02");
  SendReply("OK");
}

static void HandleRemoveBreakpoint()
{
  u32 type, addr, len, i;

  type = Hex2char(s_cmd_bfr[1]);
  if (type >= 4)
    return SendReply("E01");

  addr = 0;
  len = 0;

  i = 3;
  while (s_cmd_bfr[i] != ',')
    addr = (addr << 4) | Hex2char(s_cmd_bfr[i++]);
  i++;

  while (i < s_cmd_len)
    len = (len << 4) | Hex2char(s_cmd_bfr[i++]);

  RemoveBreakpoint(static_cast<BreakpointType>(type), addr, len);
  SendReply("OK");
}

void ProcessCommands(bool loop_until_continue)
{
  while (IsActive())
  {
    if (CPU::GetState() == CPU::State::PowerDown)
    {
      Deinit();
      INFO_LOG_FMT(GDB_STUB, "killed by power down");
      return;
    }

    if (!IsDataAvailable())
    {
      if (loop_until_continue)
        continue;
      else
        return;
    }
    ReadCommand();
    // No more commands
    if (s_cmd_len == 0)
      continue;

    switch (s_cmd_bfr[0])
    {
    case 'q':
      HandleQuery();
      break;
    case 'H':
      HandleSetThread();
      break;
    case 'T':
      HandleIsThreadAlive();
      break;
    case '?':
      SendSignal(Signal::Sigterm);
      break;
    case 'k':
      Deinit();
      INFO_LOG_FMT(GDB_STUB, "killed by gdb");
      return;
    case 'g':
      ReadRegisters();
      break;
    case 'G':
      WriteRegisters();
      break;
    case 'p':
      ReadRegister();
      break;
    case 'P':
      WriteRegister();
      break;
    case 'm':
      ReadMemory();
      break;
    case 'M':
      WriteMemory();
      PowerPC::ppcState.iCache.Reset();
      Host_UpdateDisasmDialog();
      break;
    case 's':
      Step();
      return;
    case 'C':
    case 'c':
      CPU::Continue();
      s_has_control = false;
      return;
    case 'z':
      HandleRemoveBreakpoint();
      break;
    case 'Z':
      HandleAddBreakpoint();
      break;
    default:
      SendReply("");
      break;
    }
  }
}

// exported functions

static void InitGeneric(int domain, const sockaddr* server_addr, socklen_t server_addrlen,
                        sockaddr* client_addr, socklen_t* client_addrlen);

#ifndef _WIN32
void InitLocal(const char* socket)
{
  unlink(socket);

  sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, socket);

  InitGeneric(PF_LOCAL, (const sockaddr*)&addr, sizeof(addr), NULL, NULL);
}
#endif

void Init(u32 port)
{
  sockaddr_in saddr_server = {};
  sockaddr_in saddr_client;

  saddr_server.sin_family = AF_INET;
  saddr_server.sin_port = htons(port);
  saddr_server.sin_addr.s_addr = INADDR_ANY;

  socklen_t client_addrlen = sizeof(saddr_client);

  InitGeneric(PF_INET, (const sockaddr*)&saddr_server, sizeof(saddr_server),
              (sockaddr*)&saddr_client, &client_addrlen);

  saddr_client.sin_addr.s_addr = ntohl(saddr_client.sin_addr.s_addr);
}

static void InitGeneric(int domain, const sockaddr* server_addr, socklen_t server_addrlen,
                        sockaddr* client_addr, socklen_t* client_addrlen)
{
  s_socket_context.emplace();

  s_tmpsock = socket(domain, SOCK_STREAM, 0);
  if (s_tmpsock == -1)
    ERROR_LOG_FMT(GDB_STUB, "Failed to create gdb socket");

  int on = 1;
  if (setsockopt(s_tmpsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on) < 0)
    ERROR_LOG_FMT(GDB_STUB, "Failed to setsockopt");

  if (bind(s_tmpsock, server_addr, server_addrlen) < 0)
    ERROR_LOG_FMT(GDB_STUB, "Failed to bind gdb socket");

  if (listen(s_tmpsock, 1) < 0)
    ERROR_LOG_FMT(GDB_STUB, "Failed to listen to gdb socket");

  INFO_LOG_FMT(GDB_STUB, "Waiting for gdb to connect...");

  s_sock = accept(s_tmpsock, client_addr, client_addrlen);
  if (s_sock < 0)
    ERROR_LOG_FMT(GDB_STUB, "Failed to accept gdb client");
  INFO_LOG_FMT(GDB_STUB, "Client connected.");

#ifdef _WIN32
  closesocket(s_tmpsock);
#else
  close(s_tmpsock);
#endif
  s_tmpsock = -1;

  s_update_event = CoreTiming::RegisterEvent("GDBStubUpdate", UpdateCallback);
  CoreTiming::ScheduleEvent(GDB_UPDATE_CYCLES, s_update_event);
  s_has_control = true;
}

void Deinit()
{
  if (s_tmpsock != -1)
  {
    shutdown(s_tmpsock, SHUT_RDWR);
    s_tmpsock = -1;
  }
  if (s_sock != -1)
  {
    shutdown(s_sock, SHUT_RDWR);
    s_sock = -1;
  }

  s_socket_context.reset();
  s_has_control = false;
}

bool IsActive()
{
  return s_tmpsock != -1 || s_sock != -1;
}

bool HasControl()
{
  return s_has_control;
}

void TakeControl()
{
  s_has_control = true;
}

void SendSignal(Signal signal)
{
  char bfr[128] = {};
  fmt::format_to(bfr, "T{:02x}{:02x}:{:08x};{:02x}:{:08x};", signal, 64, PC, 1, GPR(1));
  SendReply(bfr);
}
}  // namespace GDBStub
