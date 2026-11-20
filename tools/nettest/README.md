# nettest: a harness for vana.http / vana.ws / vana.promise

Runs VanaCore's real network layer (`src\core\net.cpp`, `lua_net.cpp`, `lua\net.lua`, with `lua\vana.lua` and
`lua\settings.lua` under it) in one LuaJIT state without the game, against local servers, and checks the results.

```
build.bat                                  :: makes harness.exe (needs the LuaJIT the Core build leaves in build\obj\luajit)
python servers.py                          :: HTTP on 127.0.0.1:18080, WebSocket on :18081 (and HTTPS on :18443 if cert.pem/key.pem are here)
harness.exe ..\..\src\core\lua\vana.lua ..\..\src\core\lua\settings.lua ..\..\src\core\lua\net.lua test.lua 120
harness.exe ..\..\src\core\lua\vana.lua ..\..\src\core\lua\settings.lua ..\..\src\core\lua\net.lua stress.lua 150
```

- `test.lua`: about 90 checks (methods, bodies, redirects, gzip, cookies, big bodies, timeouts, cancels, parallel requests,
  promises, downloads, WebSocket text/binary/close/drop/handshake refusal). `NETTEST_TLS=1` adds the HTTPS checks
  (make a self-signed pair first: `openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 2 -subj /CN=127.0.0.1`),
  `NETTEST_INTERNET=1` one real HTTPS request.
- `stress.lua`: 600 requests, the caps (256 requests, 16 sockets), 40 socket open/close cycles and a socket idle for 40 s.
- The harness steps `frame()` every 16 ms until the script sets the global `finished`, and prints the slowest frame.
