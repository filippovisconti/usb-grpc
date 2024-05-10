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

#include <fcntl.h>
#include <grpc/grpc.h>
#include <grpc/grpc_usb.h>
#include <grpc/support/log.h>
#include <grpc/support/port_platform.h>

#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/tcp_client_posix.h"
#include "src/core/lib/iomgr/usb_endpoint.h"
#include "src/core/lib/surface/api_trace.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/transport/transport.h"

grpc_channel* grpc_insecure_channel_create_from_usb(
    const char* target, int vid, int pid, const grpc_channel_args* args) {
  grpc_core::ExecCtx exec_ctx;
  GRPC_API_TRACE("grpc_insecure_channel_create_from_usb(target=%p, args=%p)", 2,
                 (target, args));

  grpc_arg default_authority_arg = grpc_channel_arg_string_create(
      (char*)GRPC_ARG_DEFAULT_AUTHORITY, (char*)"test.authority");
  grpc_channel_args* final_args =
      grpc_channel_args_copy_and_add(args, &default_authority_arg, 1);

  grpc_endpoint* client =
      grpc_usb_client_create_from_vid_pid(vid, pid, args, "usb-client");
  GPR_ASSERT(client != nullptr);
  grpc_transport* transport =
      grpc_create_chttp2_transport(final_args, client, true);
  GPR_ASSERT(transport != nullptr);
  grpc_channel* channel = grpc_channel_create(
      target, final_args, GRPC_CLIENT_DIRECT_CHANNEL, transport);
  grpc_channel_args_destroy(final_args);
  grpc_chttp2_transport_start_reading(transport, nullptr, nullptr);

  grpc_core::ExecCtx::Get()->Flush();

  return channel != nullptr ? channel
                            : grpc_lame_client_channel_create(
                                  target, GRPC_STATUS_INTERNAL,
                                  "Failed to create client channel");
}
