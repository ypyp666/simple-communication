# AGENTS.md

C++20 TCP server (single executable `im_server`) that accepts newline-delimited JSON over raw sockets, dispatches to MySQL-backed business handlers, and replies with JSON. Developed in CLion. Not a git repo.

## Layout quirks
- Sources live in `src` (not `src/`); headers in `include/`; nlohmann json vendored at `nlohmann/json.hpp`.
- `include/mysql.h` is a **project wrapper class `mysqlconn`**, not the system header. It itself `#include <mysql/mysql.h>`. Do not confuse the two; use `mysqlconn` for all DB work.
- `CMakeLists.txt` adds both `include/` and `nlohmann/` to the global include path.

## Build
- Requires CMake >= 4.2, pkg-config, and the `mysqlclient` dev package (headers at `/usr/include/mysql`).
- Build dir `cmake-build-debug/` uses the Ninja generator driven by CLion's bundled ninja (`/snap/clion/*/bin/ninja/...`, not on PATH).
- Working command: `cmake --build cmake-build-debug` (uses cached generator/make program). For a fresh configure: `cmake -S . -B cmake-build-debug -G Ninja` with ninja on PATH, or reconfigure from CLion.
- No tests, no CI, no linter. Verify by building and running the binary; server listens on port 8899, SIGINT/SIGTERM shut it down gracefully via `signal_handler` in `src`.

## Architecture
- Entry: `src` creates `TcpServer`, installs signal handlers, calls `server.Start(8899)`.
- `TcpServer` (`src`) does socket/bind/listen, accept loop, one detached pthread per client (`ClientWork`).
- Per-client flow: accumulate bytes into a `std::queue<char>` cache (capped at 4096, busy-wait when full), split on `\n` into single JSON messages, call `JsonParsing()`, `send()` the returned reply.
- Each client thread creates its **own** `mysqlconn` and connects with hardcoded credentials (host/port/user/pass/db) at `src`; change there if the dev MySQL VM moves.

## Wire protocol
- Messages are one JSON object per line, `\n`-terminated. Do not send/expect anything else.
- Request keys read by `JsonParsing` (`src`): `type`, `account`, `password`.
- Type dispatch in `src` is **case-sensitive and inconsistent**: `login` is lowercase; `Logout`, `ModifyPwd`, `ModifyNam`, `Repost` are camelCase.
- Response shape used by handlers: `type`, `code`, `error`, `success`.

## Business logic gotchas
- Handler table `cmd_table` (`include/table.h`) registers only `Login`, `Register`, `ModifyPwd`, `Logout`. `ModifyNam`/`Repost` map to enum values but have **no registered handler**, and `Unknown` is also unregistered — such messages fall through `cmd_table.find()` and the client gets an empty reply. Fix by registering the handler and implementing it.
- Only `HandleLogin` is actually implemented. `HandleRegister`/`HandleModifyPwd`/`HandleLogout` return the literal string `"0"` — not JSON.
- Login calls the MySQL stored function `Login(account, pwd)`; its return code maps in `src`: 0 = data crash, 1 = account missing, 2 = wrong password, 3 = success.
- `src` is a large commented-out legacy implementation superseded by the `TcpServer` class; don't treat it as live code.

## Style
- All code comments are in Chinese; keep that convention when editing.
