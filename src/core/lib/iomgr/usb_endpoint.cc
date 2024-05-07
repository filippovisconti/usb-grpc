/*
 *
 * Copyright 2016 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/* With the addition of a libuv endpoint, sockaddr.h now includes uv.h when
   using that endpoint. Because of various transitive includes in uv.h,
   including windows.h on Windows, uv.h must be included before other system
   headers. Therefore, sockaddr.h must always be included first */
#include "src/core/lib/iomgr/sockaddr.h"
#include <inttypes.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>

#include <libusb-1.0/libusb.h>

#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/debug/stats.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/iomgr/endpoint.h"

typedef struct usb_endpoint {
  grpc_endpoint base;

  grpc_fd* event_fd[20];
  int max_event_fd;
  grpc_closure event_done_closure[20];

  struct libusb_transfer *transfer_in;

  /* Used by the endpoint read function to distinguish the very first read call
   * from the rest */
  bool is_first_read;
  double target_length;
  double bytes_read_this_round;
  grpc_core::RefCount refcount;
  gpr_atm shutdown_count;

  int min_read_chunk_size;
  int max_read_chunk_size;

  /* garbage after the last read */
  grpc_slice_buffer last_read_buffer;

  grpc_slice_buffer* incoming_buffer;
  grpc_slice_buffer* outgoing_buffer;
  /** byte within outgoing_buffer->slices[0] to write next */
  size_t outgoing_byte_idx;

  grpc_closure* read_cb;
  grpc_closure* write_cb;

  grpc_closure read_done_closure;
  grpc_closure write_done_closure;

  grpc_resource_user* resource_user;
  grpc_resource_user_slice_allocator slice_allocator;

  libusb_context *ctx;
  libusb_device_handle *handle;
  int endpoint_address_in;
  int endpoint_address_out;
  int endpoint_address_in_max_pkt_size;

  bool is_shutdown;

  std::string peer_string;
  std::string local_address;
} usb_endpoint;

typedef struct arg_event_usb {
  int i;
  usb_endpoint* usb;
} arg_event_usb;

#define DEFAULT_TIMEOUT_BULK_USB_MS    30000
#define MAX_READ_SLICE_NB 16

static void notify_on_event(arg_event_usb* arg_event, grpc_fd* event_fd);
static void usb_handle_write(void* arg /* arg_event_usb */, grpc_error* error);
static void usb_do_read(usb_endpoint* usb);
static size_t get_target_read_size(usb_endpoint* udp);
static void usb_free(usb_endpoint* usb);

extern grpc_core::TraceFlag grpc_usb_trace;

#ifndef NDEBUG
#define USB_UNREF(usb, reason) usb_unref((usb), (reason), DEBUG_LOCATION)
#define USB_REF(usb, reason) usb_ref((usb), (reason), DEBUG_LOCATION)
static void usb_unref(usb_endpoint* usb, const char* reason,
                      const grpc_core::DebugLocation& debug_location) {
  if (GPR_UNLIKELY(usb->refcount.Unref(debug_location, reason))) {
    usb_free(usb);
  }
}

static void usb_ref(usb_endpoint* usb, const char* reason,
                    const grpc_core::DebugLocation& debug_location) {
  gpr_log(GPR_INFO, "usb ref run");
  usb->refcount.Ref();
  exit(1);
}
#else
#define USB_UNREF(usb, reason) usb_unref((usb))
#define USB_REF(usb, reason) usb_ref((usb))
static void usb_unref(usb_endpoint* usb) {
  if (GPR_UNLIKELY(usb->refcount.Unref())) {
    usb_free(usb);
  }
}

static void usb_ref(usb_endpoint* usb) { usb->refcount.Ref(); }
#endif

void usb_free(usb_endpoint* usb) {
  gpr_log(GPR_INFO, "USB free");

  for (int i=0; i < usb->max_event_fd; i++){
    grpc_fd_orphan(usb->event_fd[i], nullptr, nullptr , "usb_unref_orphan");
  }
  grpc_slice_buffer_destroy_internal(&usb->last_read_buffer);
  grpc_resource_user_unref(usb->resource_user);
  gpr_free(usb);
}

static void add_to_estimate(usb_endpoint* usb, size_t bytes) {
  usb->bytes_read_this_round += static_cast<double>(bytes);
  gpr_log(GPR_INFO, "bytes read this round ... %lf", usb->bytes_read_this_round);
}

void print_libusb_transfer(struct libusb_transfer *p_t)
{
    if ( NULL == p_t){
        gpr_log(GPR_INFO, "No libusb_transfer");
    }
    else {
        gpr_log(GPR_INFO, "libusb_transfer structure: %p", p_t);
        gpr_log(GPR_INFO, "flags   =%x", p_t->flags);
        gpr_log(GPR_INFO, "endpoint=%x", p_t->endpoint);
        gpr_log(GPR_INFO, "type    =%x", p_t->type);
        gpr_log(GPR_INFO, "timeout =%d", p_t->timeout);
        gpr_log(GPR_INFO, "status  =%u", p_t->status);
        // length, and buffer are commands sent to the device
        gpr_log(GPR_INFO, "length        =%d", p_t->length);
        gpr_log(GPR_INFO, "actual_length =%d", p_t->actual_length);
        gpr_log(GPR_INFO, "buffer    =%p", p_t->buffer);
        char* dump = gpr_dump(reinterpret_cast<const char*>(p_t->buffer), p_t->actual_length, GPR_DUMP_HEX | GPR_DUMP_ASCII);
        gpr_log(GPR_INFO, "%s", dump);
    }
    return;
}

void print_slice_buffer(grpc_slice_buffer* incoming_buffer)
{
    if (!incoming_buffer || !GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
        return;
    }

    gpr_log(GPR_INFO, "***************************************************************************************");
    gpr_log(GPR_INFO, "* print_slice_buffer:");
    gpr_log(GPR_INFO, "* address = %p", incoming_buffer);
    gpr_log(GPR_INFO, "* slice_array = %p", GRPC_SLICE_START_PTR(incoming_buffer->slices[0]));
    gpr_log(GPR_INFO, "* slice_count = %zu", incoming_buffer->count);
    gpr_log(GPR_INFO, "* slice_length = %zu", incoming_buffer->length);
    gpr_log(GPR_INFO, "* slice_capacity = %zu", incoming_buffer->capacity);
    gpr_log(GPR_INFO, "***************************************************************************************");
}

static void usb_align_slice_buffer(grpc_slice_buffer* slice_buffer, size_t alignment)
{
  if (!slice_buffer || !slice_buffer->count) {
    return;
  }

  gpr_log(GPR_DEBUG, "usb_align_slice_buffer: %p [%zu bytes]", slice_buffer, GRPC_SLICE_LENGTH(slice_buffer->slices[0]));

  for (size_t i=0; i < slice_buffer->count; i++) {
    if (GRPC_SLICE_LENGTH(slice_buffer->slices[i]) < alignment) {
      gpr_log(GPR_DEBUG, "usb_align_slice_buffer: trash buffer '%zu'", i);
      grpc_slice_unref_internal(grpc_slice_buffer_take_first(slice_buffer));
    }
  }
}

static void call_read_cb(usb_endpoint* usb, grpc_error* error) {
  grpc_closure* cb = usb->read_cb;

  if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
    gpr_log(GPR_INFO, "USB:%p call_cb %p %p:%p", usb, cb, cb->cb, cb->cb_arg);
    size_t i;
    const char* str = grpc_error_string(error);
    gpr_log(GPR_INFO, "read: error=%s", str);

    if (usb->incoming_buffer) {
      for (i = 0; i < usb->incoming_buffer->count; i++) {
        char* dump = grpc_dump_slice(usb->incoming_buffer->slices[i],
                                     GPR_DUMP_HEX | GPR_DUMP_ASCII);
        gpr_log(GPR_INFO, "READ %p : %s", usb, dump);
        gpr_free(dump);
      }
    }
  }

  usb->read_cb = nullptr;
  usb->incoming_buffer = nullptr;
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, cb, error);
}

static void usb_continue_read(usb_endpoint* usb) {
  size_t target_read_size = get_target_read_size(usb);
  size_t alloc_size = 0;

  if (target_read_size % usb->endpoint_address_in_max_pkt_size) {
     alloc_size = ((target_read_size / usb->endpoint_address_in_max_pkt_size) + 1)
                   * usb->endpoint_address_in_max_pkt_size;
  } else {
    alloc_size = target_read_size;
  }

  if (usb->incoming_buffer->length < target_read_size &&
      usb->incoming_buffer->count < MAX_READ_SLICE_NB) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
      gpr_log(GPR_INFO, "USB:%p alloc_slices", usb);
    }

    if (GPR_UNLIKELY(!grpc_resource_user_alloc_slices(&usb->slice_allocator,
						      alloc_size,
						      MAX_READ_SLICE_NB - usb->incoming_buffer->count,
						      usb->incoming_buffer))) {
      // Wait for allocation.
      return;
    }
  } else {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
      gpr_log(GPR_INFO, "USB:%p do_read", usb);
    }
    usb_do_read(usb);
  }
}

void trans_cb(struct libusb_transfer *transfer){
    usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(transfer->user_data);
    if (usb->incoming_buffer) {
      gpr_log(GPR_INFO, "transfer completed on USB actual length = %lu (slice buffer length %lu)", transfer->actual_length, usb->incoming_buffer);
    } else {
      gpr_log(GPR_INFO, "transfer completed on USB actual length = %lu (slice buffer is NULL !!!!!)");
    }
    print_libusb_transfer(transfer);
    print_slice_buffer(usb->incoming_buffer);

    if (transfer->actual_length == 0) {
        if (!usb->is_shutdown && usb->incoming_buffer) {
           grpc_slice_buffer_reset_and_unref_internal(usb->incoming_buffer);
           call_read_cb(usb, GRPC_ERROR_CREATE_FROM_STATIC_STRING("USB device disconnected"));
           USB_UNREF(usb, "read");
           usb->is_shutdown = true;
        }
    }
    else {
        add_to_estimate(usb, static_cast<size_t>(transfer->actual_length));
        gpr_log(GPR_INFO, "incoming buffer length %lu", usb->incoming_buffer->length);
        GPR_ASSERT((size_t)transfer->actual_length <= usb->incoming_buffer->length);
        if (static_cast<size_t>(transfer->actual_length) < usb->incoming_buffer->length) {
          grpc_slice_buffer_trim_end(
              usb->incoming_buffer,
              usb->incoming_buffer->length - static_cast<size_t>(transfer->actual_length),
              &usb->last_read_buffer);

          usb_align_slice_buffer(&usb->last_read_buffer, usb->endpoint_address_in_max_pkt_size);
        }
        GPR_ASSERT((size_t)transfer->actual_length == usb->incoming_buffer->length);
        call_read_cb(usb, GRPC_ERROR_NONE);
        USB_UNREF(usb, "read");
    }
}

static void usb_handle_read(void* arg /* usb_endpoint */, grpc_error* error) {
  usb_endpoint* usb = static_cast<usb_endpoint*>(arg);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
    gpr_log(GPR_INFO, "USB:%p got_read: %s", usb, grpc_error_string(error));
  }

  if (error != GRPC_ERROR_NONE) {
    grpc_slice_buffer_reset_and_unref_internal(usb->incoming_buffer);
    grpc_slice_buffer_reset_and_unref_internal(&usb->last_read_buffer);
    call_read_cb(usb, GRPC_ERROR_REF(error));
    USB_UNREF(usb, "read");
  } else {
    usb_continue_read(usb);
  }
}

static void notify_on_read(usb_endpoint* usb) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
    gpr_log(GPR_INFO, "USB:%p notify_on_read", usb);
  }
}

static void usb_read(grpc_endpoint* ep, grpc_slice_buffer* incoming_buffer,
		     grpc_closure* cb, bool urgent) {

    usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
    GPR_ASSERT(usb->read_cb == nullptr);
    usb->read_cb = cb;
    usb->incoming_buffer = incoming_buffer;
    grpc_slice_buffer_reset_and_unref_internal(incoming_buffer);
    grpc_slice_buffer_swap(incoming_buffer, &usb->last_read_buffer);
    USB_REF(usb, "read");

    if (usb->is_first_read) {

      /* Endpoint read called for the very first time. Register read callback with
       * the polling engine */
      usb->is_first_read = false;
      notify_on_read(usb);
      grpc_core::Closure::Run(DEBUG_LOCATION, &usb->read_done_closure,
			      GRPC_ERROR_NONE);
    } else {
      /* Not the first time. We may or may not have more bytes available. In any
       * case call usb->read_done_closure (i.e usb_handle_read()) which does the
       * right thing (i.e calls usb_do_read() which either reads the available
       * bytes or calls notify_on_read() to be notified when new bytes become
       * available */
      grpc_core::Closure::Run(DEBUG_LOCATION, &usb->read_done_closure,
			      GRPC_ERROR_NONE);
    }
}

static void usb_drop_uncovered_then_handle_write(void* arg, grpc_error* error) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
    gpr_log(GPR_INFO, "USB:%p got_write: %s", arg, grpc_error_string(error));
  }
  usb_handle_write(arg, error);
}

static void notify_on_write(usb_endpoint* usb) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
    gpr_log(GPR_INFO, "USB:%p notify_on_write", usb);
  }
}

/* returns true if done, false if pending; if returning true, *error is set */
#define MAX_WRITE_IOVEC 1000
static bool usb_flush(usb_endpoint* usb, grpc_error** error) {
  struct iovec iov[MAX_WRITE_IOVEC];
  size_t iov_size;
  int sent_length;
  int sent_length_chunk = 0;
  size_t sending_length;
  size_t trailing;
  size_t unwind_slice_idx;
  size_t unwind_byte_idx;
  int r_usb = 0;
  // We always start at zero, because we eagerly unref and trim the slice
  // buffer as we write
  size_t outgoing_slice_idx = 0;

  for (;;) {
    sending_length = 0;
    unwind_slice_idx = outgoing_slice_idx;
    unwind_byte_idx = usb->outgoing_byte_idx;
    for (iov_size = 0; outgoing_slice_idx != usb->outgoing_buffer->count &&
                       iov_size != MAX_WRITE_IOVEC;
         iov_size++) {
      iov[iov_size].iov_base =
          GRPC_SLICE_START_PTR(
              usb->outgoing_buffer->slices[outgoing_slice_idx]) +
          usb->outgoing_byte_idx;
      iov[iov_size].iov_len =
          GRPC_SLICE_LENGTH(usb->outgoing_buffer->slices[outgoing_slice_idx]) -
          usb->outgoing_byte_idx;
      sending_length += iov[iov_size].iov_len;
      outgoing_slice_idx++;
      usb->outgoing_byte_idx = 0;
    }
    GPR_ASSERT(iov_size > 0);

    GRPC_STATS_INC_TCP_WRITE_SIZE(sending_length);
    GRPC_STATS_INC_TCP_WRITE_IOV_SIZE(iov_size);

    GPR_TIMER_SCOPE("sendmsg", 1);

    sent_length = 0;
    gpr_log(GPR_INFO, "write DATA with iov size : %ld", iov_size);
    for (size_t i = 0; i < iov_size && r_usb == 0; i++) {
      sent_length_chunk = 0;
      gpr_log(GPR_INFO, "write DATA with length : %ld on chunk %ld", iov[i].iov_len, i);
      r_usb = libusb_bulk_transfer(usb->handle, usb->endpoint_address_out, (unsigned char*)iov[i].iov_base, (int)iov[i].iov_len, &sent_length_chunk, DEFAULT_TIMEOUT_BULK_USB_MS);
      sent_length += sent_length_chunk;
      gpr_log(GPR_INFO, "sent data : %d", sent_length_chunk);
    }

    if (sent_length <= 0) {
      if (r_usb == LIBUSB_ERROR_BUSY) {
        usb->outgoing_byte_idx = unwind_byte_idx;
        // unref all and forget about all slices that have been written to this
        // point
        for (size_t idx = 0; idx < unwind_slice_idx; ++idx) {
          grpc_slice_unref_internal(
              grpc_slice_buffer_take_first(usb->outgoing_buffer));
        }
        return false;
    } else if (r_usb == LIBUSB_ERROR_PIPE) {
        *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(libusb_strerror((libusb_error)r_usb));
        grpc_slice_buffer_reset_and_unref_internal(usb->outgoing_buffer);
        return true;
      } else {
        *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(libusb_strerror((libusb_error)r_usb));
        grpc_slice_buffer_reset_and_unref_internal(usb->outgoing_buffer);
        return true;
      }
    }

    GPR_ASSERT(usb->outgoing_byte_idx == 0);
    trailing = sending_length - static_cast<size_t>(sent_length);
    while (trailing > 0) {
      size_t slice_length;

      outgoing_slice_idx--;
      slice_length =
          GRPC_SLICE_LENGTH(usb->outgoing_buffer->slices[outgoing_slice_idx]);
      if (slice_length > trailing) {
        usb->outgoing_byte_idx = slice_length - trailing;
        break;
      } else {
        trailing -= slice_length;
      }
    }

    if (outgoing_slice_idx == usb->outgoing_buffer->count) {
      *error = GRPC_ERROR_NONE;
      grpc_slice_buffer_reset_and_unref_internal(usb->outgoing_buffer);
      return true;
    }
  }
}

void usb_handle_write(void* arg /* usb_endpoint */, grpc_error* error) {
  usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(arg);
  grpc_closure* cb;

  if (error != GRPC_ERROR_NONE) {
    cb = usb->write_cb;
    usb->write_cb = nullptr;
    cb->cb(cb->cb_arg, error);
    USB_UNREF(usb, "write");
    return;
  }

  if (!usb_flush(usb, &error)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
      gpr_log(GPR_INFO, "write: delayed");
    }
    notify_on_write(usb);
  } else {
    cb = usb->write_cb;
    usb->write_cb = nullptr;
    if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
      const char* str = grpc_error_string(error);
      gpr_log(GPR_INFO, "write: %s", str);
    }

    grpc_core::Closure::Run(DEBUG_LOCATION, cb, error);
    USB_UNREF(usb, "write");
  }
}

static void usb_write(grpc_endpoint* ep, grpc_slice_buffer* slices,
		      grpc_closure* cb, void* arg) {

    usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
    grpc_error* error = GRPC_ERROR_NONE;

     if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
       size_t i;

       for (i = 0; i < slices->count; i++) {
         char* data =
             grpc_dump_slice(slices->slices[i], GPR_DUMP_HEX | GPR_DUMP_ASCII);
         gpr_log(GPR_INFO, "WRITE %p : %s", usb, data);
         gpr_free(data);
       }
     }

     GPR_ASSERT(usb->write_cb == nullptr);

     if (slices->length == 0) {
         bool fds_are_shutdown = false;
         for (int i=0; i < usb->max_event_fd; i++) {
             fds_are_shutdown = fds_are_shutdown ? fds_are_shutdown : grpc_fd_is_shutdown(usb->event_fd[i]);
         }
	 grpc_core::Closure::Run(DEBUG_LOCATION, cb, fds_are_shutdown
                    ? GRPC_ERROR_CREATE_FROM_STATIC_STRING("EOF")
                    : GRPC_ERROR_NONE);
	 return;
     }
     usb->outgoing_buffer = slices;
     usb->outgoing_byte_idx = 0;

     if (!usb_flush(usb, &error)) {
       USB_REF(usb, "write");
       usb->write_cb = cb;
       if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
         gpr_log(GPR_INFO, "write: delayed");
       }
       notify_on_write(usb);
     } else {
       if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
         const char* str = grpc_error_string(error);
         gpr_log(GPR_INFO, "write: %s", str);
       }
       grpc_core::Closure::Run(DEBUG_LOCATION, cb, error);
     }
}

static void usb_add_to_pollset(grpc_endpoint* ep, grpc_pollset* pollset) {
    usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
    for (int i=0; i < usb->max_event_fd; i++){
        grpc_pollset_add_fd(pollset, usb->event_fd[i]);
        gpr_log(GPR_INFO, "usb add to fd %d", grpc_fd_wrapped_fd(usb->event_fd[i]));
    }
}

static void usb_add_to_pollset_set(grpc_endpoint* ep,
                                  grpc_pollset_set* pollset_set) {
    usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
    gpr_log(GPR_INFO, "usb_add_to_pollset_set");
    for (int i=0; i < usb->max_event_fd; i++){
        grpc_pollset_set_add_fd(pollset_set, usb->event_fd[i]);
        gpr_log(GPR_INFO, "usb add to add_set fd %d", grpc_fd_wrapped_fd(usb->event_fd[i]));
    }
}

static void usb_delete_from_pollset_set(grpc_endpoint* ep,
                                       grpc_pollset_set* pollset_set) {
    usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
    for (int i=0; i < usb->max_event_fd; i++){
        grpc_pollset_set_del_fd(pollset_set, usb->event_fd[i]);
    }
}

static void usb_shutdown(grpc_endpoint* ep, grpc_error* why) {
  usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
  gpr_log(GPR_INFO, "USB shutdown endpoint");

  if (!usb->is_shutdown && usb->incoming_buffer) {
        grpc_slice_buffer_reset_and_unref_internal(usb->incoming_buffer);
        call_read_cb(usb, GRPC_ERROR_CREATE_FROM_STATIC_STRING("USB device disconnected"));
        USB_UNREF(usb, "read");
        usb->is_shutdown = true;
  }

  for (int i=0; i < usb->max_event_fd; i++){
      grpc_fd_shutdown(usb->event_fd[i], GRPC_ERROR_REF(why));
  }
  GRPC_ERROR_UNREF(why);
  grpc_resource_user_shutdown(usb->resource_user);
}

static void usb_destroy(grpc_endpoint* ep) {
  gpr_log(GPR_INFO, "USB destroy endpoint");

  usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
  grpc_slice_buffer_reset_and_unref_internal(&usb->last_read_buffer);
  libusb_exit(usb->ctx);
  if (grpc_event_engine_can_track_errors()) {
    gpr_atm_no_barrier_store(&usb->shutdown_count, true);
    for (int i=0; i < usb->max_event_fd; i++) {
      grpc_fd_set_error(usb->event_fd[i]);
    }
  }
  USB_UNREF(usb, "destroy");
}

static absl::string_view usb_get_peer(grpc_endpoint* ep) {
  usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
  return usb->peer_string;
}

static absl::string_view usb_get_local_address(grpc_endpoint* ep) {
  usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
  return usb->local_address;
}

static grpc_resource_user* usb_get_resource_user(grpc_endpoint* ep) {
  usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
  return usb->resource_user;
}

static bool usb_can_track_err(grpc_endpoint* ep) {
  usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
  if (!grpc_event_engine_can_track_errors()) {
    return false;
  }
  // TODO: what to do for checking if can track errors ?
  return true;
}


size_t get_target_read_size(usb_endpoint* udp) {
  grpc_resource_quota* rq = grpc_resource_user_quota(udp->resource_user);
  double pressure = grpc_resource_quota_get_memory_pressure(rq);
  double target =
      udp->target_length * (pressure > 0.8 ? (1.0 - pressure) / 0.2 : 1.0);
  size_t sz = ((static_cast<size_t> GPR_CLAMP(target, udp->min_read_chunk_size,
                                              udp->max_read_chunk_size)) +
               255) &
              ~static_cast<size_t>(255);
  /* don't use more than 1/16th of the overall resource quota for a single read
   * alloc */
  size_t rqmax = grpc_resource_quota_peek_size(rq);
  if (sz > rqmax / 16 && rqmax > 1024) {
    sz = rqmax / 16;
  }
  return sz;
}

void usb_do_read(usb_endpoint* usb) {
  GPR_TIMER_SCOPE("usb_do_read", 0);

  GPR_ASSERT(usb->incoming_buffer->count <= MAX_READ_SLICE_NB);
  usb->transfer_in = libusb_alloc_transfer(0);
  print_slice_buffer(usb->incoming_buffer);

  libusb_fill_bulk_transfer(usb->transfer_in, usb->handle, usb->endpoint_address_in,
      GRPC_SLICE_START_PTR(usb->incoming_buffer->slices[0]), GRPC_SLICE_LENGTH(usb->incoming_buffer->slices[0]), trans_cb, usb, DEFAULT_TIMEOUT_BULK_USB_MS);

  gpr_log(GPR_DEBUG, "usb_do_read: submit transfer request '%p' [buf=%p sz=%zu]", usb->transfer_in,
      GRPC_SLICE_START_PTR(usb->incoming_buffer->slices[0]),
      GRPC_SLICE_LENGTH(usb->incoming_buffer->slices[0]));
  libusb_submit_transfer(usb->transfer_in);
  return;
}

static void usb_read_allocation_done(void* usbp, grpc_error* error) {
  usb_endpoint* usb = static_cast<usb_endpoint*>(usbp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
    gpr_log(GPR_INFO, "USB:%p read_allocation_done: %s", usb,
            grpc_error_string(error));
  }
  if (error != GRPC_ERROR_NONE) {
    grpc_slice_buffer_reset_and_unref_internal(usb->incoming_buffer);
    grpc_slice_buffer_reset_and_unref_internal(&usb->last_read_buffer);
    call_read_cb(usb, GRPC_ERROR_REF(error));
    USB_UNREF(usb, "read");
  } else {
    usb_do_read(usb);
  }
}

static int usb_get_fd(grpc_endpoint* ep) {
    usb_endpoint* usb = reinterpret_cast<usb_endpoint*>(ep);
    return grpc_fd_wrapped_fd(usb->event_fd[0]);
}

static const grpc_endpoint_vtable vtable = {
    usb_read,
    usb_write,
    usb_add_to_pollset,
    usb_add_to_pollset_set,
    usb_delete_from_pollset_set,
    usb_shutdown,
    usb_destroy,
    usb_get_resource_user,
    usb_get_peer,
    usb_get_local_address,
    usb_get_fd,
    usb_can_track_err,
};

static void usb_handle_event(void* arg /* usb_endpoint */, grpc_error* error) {
  arg_event_usb* arg_event = static_cast<arg_event_usb*>(arg);
  usb_endpoint* usb = arg_event->usb;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
    gpr_log(GPR_INFO, "USB:%p got_event: %s", arg_event->usb, grpc_error_string(error));
  }

  if (error != GRPC_ERROR_NONE) {
    USB_UNREF(usb, "event");
  } else {
      struct timeval tv = { 0, 100 };
      libusb_handle_events_timeout(usb->ctx, &tv);

      gpr_log(GPR_INFO, "USB:%p finish_got_event", usb);
      notify_on_event(arg_event, usb->event_fd[arg_event->i]);
  }
}

void notify_on_event(arg_event_usb* arg_event, grpc_fd* event_fd) {
  usb_endpoint* usb = arg_event->usb;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace)) {
    gpr_log(GPR_INFO, "USB:%p notify_on_event with fd %d", usb, grpc_fd_wrapped_fd(event_fd));
  }
  GRPC_CLOSURE_INIT(&usb->event_done_closure[arg_event->i], usb_handle_event, arg_event,
                    grpc_schedule_on_exec_ctx);
  grpc_fd_notify_on_write(event_fd, &usb->event_done_closure[arg_event->i]);
}

static int usb_get_max_packet_size(libusb_device_handle *handle, int endpoint_address) {
  int packet_size = libusb_get_max_packet_size(libusb_get_device(handle), endpoint_address);

  if ((packet_size == LIBUSB_ERROR_NOT_FOUND) || (packet_size == LIBUSB_ERROR_OTHER)) {
    packet_size = -1;
  }

  gpr_log(GPR_DEBUG, "usb_get_max_packet_size: endpoint '%x' = %d bytes", endpoint_address, packet_size);
  return packet_size;
}

#define MAX_CHUNK_SIZE 32 * 1024 * 1024

grpc_endpoint* grpc_usb_client_create_from_vid_pid(int vid, int pid, const grpc_channel_args* channel_args,
                               const char* peer_string) 
{
  usb_endpoint* usb = new usb_endpoint();
  usb->base.vtable = &vtable;
  usb->peer_string = peer_string;
  usb->local_address = "";

  grpc_tracer_set_enabled("usb", 1);

  gpr_log(GPR_INFO, "USB client create from VID PID");

  int tcp_read_chunk_size = GRPC_TCP_DEFAULT_READ_SLICE_SIZE;
  int tcp_max_read_chunk_size = 4 * 1024 * 1024;
  int tcp_min_read_chunk_size = 256;

  grpc_resource_quota* resource_quota = grpc_resource_quota_create(nullptr);
  if (channel_args != nullptr) {
    for (size_t i = 0; i < channel_args->num_args; i++) {
      if (0 ==
          strcmp(channel_args->args[i].key, GRPC_ARG_TCP_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 == strcmp(channel_args->args[i].key,
                             GRPC_ARG_TCP_MIN_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_min_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 == strcmp(channel_args->args[i].key,
                             GRPC_ARG_TCP_MAX_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_max_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 ==
                 strcmp(channel_args->args[i].key, GRPC_ARG_RESOURCE_QUOTA)) {
        grpc_resource_quota_unref_internal(resource_quota);
        resource_quota =
            grpc_resource_quota_ref_internal(static_cast<grpc_resource_quota*>(
                channel_args->args[i].value.pointer.p));
      }
    }
  }

  usb->read_cb = nullptr;
  usb->write_cb = nullptr;
  usb->incoming_buffer = nullptr;

  usb->target_length = static_cast<double>(tcp_read_chunk_size);
  usb->min_read_chunk_size = tcp_min_read_chunk_size;
  usb->max_read_chunk_size = tcp_max_read_chunk_size;
  usb->bytes_read_this_round = 0;

  /* Will be set to false by the very first endpoint read function */
  usb->is_first_read = true;
  /* paired with unref in grpc_usb_destroy */
  new (&usb->refcount) grpc_core::RefCount(
      1, GRPC_TRACE_FLAG_ENABLED(grpc_usb_trace) ? "usb" : nullptr);
  gpr_atm_no_barrier_store(&usb->shutdown_count, 0);

  int init_res = libusb_init(&usb->ctx);
  GPR_ASSERT(init_res == 0);
  usb->handle = libusb_open_device_with_vid_pid(usb->ctx, vid, pid);
  if (usb->handle == NULL){
      gpr_log(GPR_INFO, "cannot handling USB, end of USB endpoint");
      libusb_exit(usb->ctx);
      return NULL;
  }
  libusb_claim_interface(usb->handle, 0);

  struct libusb_config_descriptor *config = NULL;
  int ret_value = libusb_get_active_config_descriptor (libusb_get_device(usb->handle), &config);
  if (ret_value != 0) {
      gpr_log(GPR_INFO, "found one or more interfaces USB");
      libusb_exit(usb->ctx);
      return NULL;
  }

  if (config->bNumInterfaces >= 1) {
    gpr_log(GPR_INFO, "found one or more interfaces USB");
    gpr_log(GPR_INFO, "number of endpoint %d", config->interface[0].altsetting[0].bNumEndpoints);
    usb->endpoint_address_in = config->interface[0].altsetting[0].endpoint[0].bEndpointAddress;
    usb->endpoint_address_out = config->interface[0].altsetting[0].endpoint[1].bEndpointAddress;
    gpr_log(GPR_INFO, "endpoint address IN %x", usb->endpoint_address_in);
    gpr_log(GPR_INFO, "endpoint address OUT %x", usb->endpoint_address_out);
  }
  else {
      gpr_log(GPR_INFO, "cannot found interface, take default value");
      // default value
      usb->endpoint_address_in = 0x81;
      usb->endpoint_address_out = 0x02;
  }
  libusb_free_config_descriptor (config);

  ret_value = usb_get_max_packet_size(usb->handle, usb->endpoint_address_in);
  if (ret_value == -1) {
      gpr_log(GPR_ERROR, "Cannot get IN endpoint max packet size");
      libusb_exit(usb->ctx);
      return NULL;
  }
  usb->endpoint_address_in_max_pkt_size = ret_value;

  const struct libusb_pollfd** list_fd = libusb_get_pollfds(usb->ctx);
  usb->max_event_fd = 0;
  for (int i = 0; list_fd[i] != NULL; i++){
      char* name;
      gpr_asprintf(&name, "usb-fd %d", list_fd[i]->fd);
      grpc_fd* event_fd = grpc_fd_create(list_fd[i]->fd, name, true);
      gpr_free(name);
      usb->event_fd[i] = event_fd;
      arg_event_usb* arg_event = new arg_event_usb();
      USB_REF(usb, "event");
      arg_event->usb = usb;
      arg_event->i = i;
      notify_on_event(arg_event, event_fd);
      usb->max_event_fd = i+1;
  }

  grpc_slice_buffer_init(&usb->last_read_buffer);
  usb->resource_user = grpc_resource_user_create(resource_quota, peer_string);
  grpc_resource_user_slice_allocator_init(
      &usb->slice_allocator, usb->resource_user, usb_read_allocation_done, usb);

#if 0 // TODO:
  // This following block which exists in gRPC v1.13.0 is no more,
  /* Tell network status tracker about new endpoint */
  grpc_network_status_register_endpoint(&usb->base);

  // should we replace it by something equivalent to what was found in tcp_posix.cc:

  if (grpc_event_engine_can_track_errors()) {
    /* Grab a ref to usb so that we can safely access the usb struct when
     * processing errors. We unref when we no longer want to track errors
     * separately. */
    TCP_REF(tcp, "error-tracking");
    gpr_atm_rel_store(&usb->stop_error_notification, 0);
    GRPC_CLOSURE_INIT(&usb->error_closure, usb_handle_error, usb,
                      grpc_schedule_on_exec_ctx);
    grpc_fd_notify_on_error(usb->em_fd, &usb->error_closure);
  }


#endif
  grpc_resource_quota_unref_internal(resource_quota);
  GRPC_CLOSURE_INIT(&usb->read_done_closure, usb_handle_read, usb,
                    grpc_schedule_on_exec_ctx);
  if (grpc_event_engine_run_in_background()) {
    // If there is a polling engine always running in the background, there is
    // no need to run the backup poller.
    GRPC_CLOSURE_INIT(&usb->write_done_closure, usb_handle_write, usb,
                      grpc_schedule_on_exec_ctx);
  } else {
    GRPC_CLOSURE_INIT(&usb->write_done_closure,
                      usb_drop_uncovered_then_handle_write, usb,
                      grpc_schedule_on_exec_ctx);
  }
  usb->is_shutdown = false;
  return &usb->base;
}
