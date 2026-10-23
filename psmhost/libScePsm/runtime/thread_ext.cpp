// System.Threading.ExtendedMethods (Sce.PlayStation.BclExtensions): GetNativeIdInternal is an
// icall of the firmware's Mono runtime that answers the thread's native (kernel) thread id. The
// host runtime keeps that id in the managed InternalThread.thread_id field (the id of the OS
// thread running the managed thread); it is read from there.
#include <cstdint>

#include <mono/metadata/class.h>
#include <mono/metadata/object.h>

#include "host/boundary.h"

namespace ssn::psm {
namespace {

uint64_t GetNativeIdInternal(MonoObject* thread) {
  if (!thread) return 0;
  static MonoClassField* internal_field = nullptr;
  static MonoClassField* tid_field = nullptr;
  if (!internal_field) {
    MonoClass* tc = mono_object_get_class(thread);
    internal_field = mono_class_get_field_from_name(tc, "internal_thread");
  }
  MonoObject* internal = nullptr;
  if (internal_field) mono_field_get_value(thread, internal_field, &internal);
  if (!internal) return 0;
  if (!tid_field) tid_field = mono_class_get_field_from_name(mono_object_get_class(internal), "thread_id");
  int64_t tid = 0;
  if (tid_field) mono_field_get_value(internal, tid_field, &tid);
  return static_cast<uint64_t>(tid);
}
SSN_ICALL("System.Threading.ExtendedMethods::GetNativeIdInternal", GetNativeIdInternal);

}  // namespace
}  // namespace ssn::psm
