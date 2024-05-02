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

#ifndef GRPCPP_SERVER_USB_H
#define GRPCPP_SERVER_USB_H

#include <grpc/support/port_platform.h>
#include <grpcpp/server.h>

#include <memory>

namespace grpc {

void AddInsecureChannelFromUsb(grpc::Server* server, int vid, int pid);

}  // namespace grpc

#endif  // GRPCPP_SERVER_USB_H
