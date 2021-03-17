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

#ifndef USB_ENDPOINT_H
#define USB_ENDPOINT_H

#include "src/core/lib/debug/trace.h"
#include "src/core/lib/iomgr/endpoint.h"

extern grpc_core::TraceFlag grpc_usb_trace;

grpc_endpoint* grpc_usb_client_create_from_vid_pid(int vid, int pid, const grpc_channel_args* args,
                               const char* peer_string);

#endif
