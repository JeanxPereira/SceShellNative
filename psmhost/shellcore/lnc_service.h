// SceShellCore's launcher service ("SceLncService", vsh/shell/shell_core/lnc_service) as
// ShellUI's LncUtil client sees it (FW 4.03). See docs/app-launch.md.
#pragma once
#include <cstdint>

namespace ssn::shellcore {

// The event record ShellCore queues for ShellUI: one 5736-byte IPMI message per event on
// message queue 0 of the "SceLncService" session (the client creates the session with a
// 5736-byte message buffer, libSceSystemService 0x6b40, and reads it back with
// tryGetMessage(0, buf, &size, 5736), 0x9000). The managed mirror is
// Sce.Vsh.LncUtil.EventMonitor.Event (Pack = 4); offsets below follow it.
#pragma pack(push, 4)
struct LncEvent {
  int32_t event_type;           // +0    EventMonitor.EventType (1 AppLaunchStart, 2 AppLaunchEnd, ...)
  char title_id[16];            // +4
  int32_t app_id;               // +20
  int32_t app_type;             // +24   EventMonitor.AppType
  int32_t app_attr;             // +28   EventMonitor.AppAttr
  int32_t launch_request_app_id;// +32
  int32_t user_id;              // +36
  uint8_t args[4096];           // +40
  int32_t args_size;            // +4136
  int32_t error_code;           // +4140
  int32_t error_data;           // +4144
  int32_t app_local_pid;        // +4148
  char path[1024];              // +4152
  char sce_sys_dir[256];        // +5176
  int32_t cause_code;           // +5432
  int32_t existing_app_id;      // +5436
  uint64_t check_flag;          // +5440
  uint64_t context_id;          // +5448
  int32_t coredump_reason;      // +5456
  char corefile_name[256];      // +5460
  uint32_t exception_code;      // +5716
  char category[4];             // +5720
  uint32_t app_category_type;   // +5724
  int32_t is_crashed;           // +5728
  uint8_t tail[4];              // +5732 (record size 5736; not read by the managed side)
};
#pragma pack(pop)
static_assert(sizeof(LncEvent) == 5736);

// Queues an event for ShellUI (what ShellCore's PushEventForShellUI does over IPMI).
void push_event_for_shellui(const LncEvent& e);

// sceLncUtilGetEventForShellUI as the host answers it: pops the oldest queued event into
// `out` and returns 0, or returns the empty-queue status when nothing is queued.
int32_t get_event_for_shellui(LncEvent* out);

}  // namespace ssn::shellcore
