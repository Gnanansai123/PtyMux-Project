# PtyMux — Terminal Multiplexer (C11)

A learning-focused Unix/Linux terminal multiplexer prototype. It creates PTY-backed Bash sessions and uses one real terminal to switch between them.

## Implemented in this version

- `posix_openpt`, `grantpt`, `unlockpt`, `ptsname`: allocate a PTY
- `fork`, `setsid`, `ioctl(TIOCSCTTY)`, `dup2`, `exec`: start Bash attached to the PTY slave
- `select`: monitor keyboard input and multiple PTY master file descriptors
- `termios`: raw mode so PtyMux can read the Ctrl-B prefix
- `SIGWINCH` and `TIOCSWINSZ`: propagate terminal size
- `SIGCHLD` and `waitpid`: notice/reap child processes
- Session table: create, switch, list, and kill sessions

## Build (Ubuntu / WSL)

Open Ubuntu, then go to the folder where you extracted this project:

```bash
cd ptymux_project
make
```

If `make` or `gcc` is missing:

```bash
sudo apt update
sudo apt install build-essential
```

Run from an interactive Ubuntu terminal:

```bash
./ptymux
```

## Key bindings

Press **Ctrl-B**, release it, then press the second key:

- `Ctrl-B` then `1` ... `8`: switch to session ID
- `Ctrl-B` then `c`: create a new Bash session
- `Ctrl-B` then `l`: list sessions and show key help
- `Ctrl-B` then `n`: switch to the next running session
- `Ctrl-B` then `k`: terminate the active session
- `Ctrl-B` then `q`: quit PtyMux and terminate sessions
- `Ctrl-B` then `Ctrl-B`: send a literal Ctrl-B to the active shell

Session IDs are assigned in creation order and are not reused during one run.

## Important scope / limitations

This is the first multi-session prototype, not a production tmux replacement.
- Detach/reattach through a Unix-domain-socket server/client is not implemented yet.
- Scrollback, split panes, persistent sessions, and named-session management are not implemented yet.
- Output from inactive sessions is read and discarded rather than stored in scrollback.
- The implementation uses `select`, so it is intended for a small number of sessions.
- This starter version forwards keyboard bytes one at a time and is intended for learning and lab demonstrations.
- Some terminal-specific behavior may differ across terminals/WSL. Test in Ubuntu's interactive terminal, not an IDE output console.

## Troubleshooting

- If it says `PtyMux must run in an interactive terminal`, run it directly in Ubuntu/WSL terminal.
- To exit, use `Ctrl-B` then `q`.
- If the terminal looks strange after an unexpected crash, run `stty sane`.
- If compilation warns about PTY declarations, make sure `_XOPEN_SOURCE 600` appears at the top of `src/main.c` before system headers.

## Syllabus concepts mapped to the code

| OS / systems concept | Where / how it appears |
|---|---|
| System calls and file descriptors | PTY master/slave, `open`, `read`, `write`, `ioctl` |
| Process creation | `fork()` |
| Program replacement | `exec` / `execl()` |
| Process/session setup | `setsid()`, `TIOCSCTTY` |
| Standard streams | `dup2()` maps slave PTY to stdin/stdout/stderr |
| IPC / producer-consumer style data flow | bytes flow between PTY master and slave |
| Multiplexing / event-driven I/O | `select()` watches stdin and all active PTYs |
| Terminal control | `termios` raw mode |
| Signals | `SIGWINCH`, `SIGCHLD`, `SIGHUP`, `SIGTERM` |
| Process lifecycle / zombies | `waitpid()` reaps exited children |
| Resource management | close file descriptors and terminate child shells |
| Advanced next stage | Unix-domain socket server/client for detach/reattach |

## Suggested next milestones

1. Improve keyboard forwarding and terminal signal handling.
2. Add per-session scrollback ring buffers.
3. Add Unix-domain socket server/client architecture.
4. Implement detach/reattach.
5. Add split panes and stronger lifecycle/error handling.
