// SceShellCore's DbPreparation manager and its IPMI server "SceDbPreparationServer"
// (FW 4.03; server object 0x77CB90, dispatcher 0x77D0F0), as the ShellUI eboot's
// DbPreparation client uses it. The IPMI transport is replaced by direct calls; each
// function names the method id the client sends.
#pragma once
#include <cstdint>

#include "shellcore/db_preparation_server_common.h"

namespace ssn::shellcore::dbp_server {

// The client's connect (eboot 0x39D70 / 0x39EB0). HOST POLICY: SceShellCore boots before
// the ShellUI on the console and runs its own DbPreparation (its CEX initialize thread)
// then; the host starts that the first time a client connects.
int32_t connect();

int32_t initialize_db_preparation(const dbp::Event& ev);  // 0x160000 -> 0x7785B0
int32_t terminate_db_preparation(const dbp::Event& ev);   // 0x160001 -> 0x778640
int32_t notify_db_initialized(const dbp::Event& ev);      // 0x160002 -> 0x778710
int32_t notify_db_terminating(const dbp::Event& ev);      // 0x160003 -> 0x7787C0
int32_t get_invalid_user_num(int32_t* num);               // 0x160004 -> 0x77CA00
int32_t get_invalid_user(int32_t index, int32_t* user);   // 0x160005 -> 0x77CA10
int32_t delete_invalid_user();                            // 0x160006 -> 0x777340
int32_t is_bad_power_down_detected(uint8_t* detected);    // 0x160007 -> 0x77CA30
int32_t get_status(uint8_t out[24]);                      // 0x160008 -> 0x77CA60
// 0x160009 (async, 0x77D6E0 -> 0x77CAA0 "SceDbPreparationWaitThread") + 0x16000A: waits for
// SceShellCore's preparation thread (0x778630 = WaitThread of 1AB4858) and returns its
// result as the async request's result.
int32_t wait_for_initialization(int32_t* result);

}  // namespace ssn::shellcore::dbp_server
