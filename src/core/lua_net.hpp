#pragma once

struct lua_State;

// The native half of vana.http, vana.ws and vana.promise (net.hpp is the engine, lua/net.lua the Lua half).
namespace vanacore::lua_net {

// A new table of the calls lua/net.lua is given, left on the stack. Each addon state has its own mailbox in the engine,
// which the state's garbage collection ends (cancelling whatever the addon still has going) when the addon unloads:
//   http(spec) -> id | nil, error           spec: url, method, headers {name = value}, body, timeout, max_time, follow,
//                                           max_redirects, max_size, save, insecure
//   websocket(spec) -> id | nil, error      spec: url, headers, protocols {..}, timeout, keepalive, max_size, insecure
//   cancel(id)
//   send(id, data, binary) -> true | nil, error
//   close(id, code, reason)
//   poll() -> { event, ... } | nil          what finished since the last call (see net.lua)
//   info() -> { requests, sockets, waiting, workers }
void push(lua_State* L);

} // namespace vanacore::lua_net
