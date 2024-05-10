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

#include <grpc/support/port_platform.h>

#include <grpc/grpc.h>
#include <grpc/grpc_usb.h>
#include <grpc/support/log.h>

#include "absl/strings/str_cat.h"

#include <grpc/support/alloc.h>
#include <grpc/support/string_util.h>

#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/usb_endpoint.h"
#include "src/core/lib/surface/completion_queue.h"
#include "src/core/lib/surface/server.h"

grpc_channel* grpc_server_add_insecure_channel_from_usb(grpc_server* server,
							void* reserved, int vid, int pid) {
  // GPR_ASSERT(reserved == nullptr);

  grpc_core::ExecCtx exec_ctx;
  grpc_core::Server* core_server = server->core_server.get();
  GPR_ASSERT(core_server != nullptr);
  std::cout << "got core_server" << std::endl;
  std::string name = absl::StrCat("server vid:", vid, "pid:", pid);

  grpc_endpoint* server_endpoint = grpc_usb_client_create_from_vid_pid(vid,
	pid, core_server->channel_args(), name.c_str());

  if (!server_endpoint) {
    gpr_log(GPR_ERROR, "Cannot create USB channel: no endpoint");
    return NULL;
  }

  const grpc_channel_args* server_args = core_server->channel_args();
  grpc_transport* transport = grpc_create_chttp2_transport(
      server_args, server_endpoint, false /* is_client */);

  grpc_error* error =
      core_server->SetupTransport(transport, nullptr, server_args, nullptr);
  if (error == GRPC_ERROR_NONE) {
    for (grpc_pollset* pollset : core_server->pollsets()) {
      grpc_endpoint_add_to_pollset(server_endpoint, pollset);
    }
    grpc_chttp2_transport_start_reading(transport, nullptr, nullptr);
  } else {
    gpr_log(GPR_ERROR, "Failed to create channel: %s",
            grpc_error_string(error));
    GRPC_ERROR_UNREF(error);
    grpc_transport_destroy(transport);
    return NULL;
  }
  auto t1 = core_server->GetChannelsLocked();
  auto t2 = t1.front();
  return t2;
}
