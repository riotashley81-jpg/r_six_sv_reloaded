"""Send commands to a running r6sv started with --console.

usage:
  python tools/r6ctl.py "status"                     one command per argument
  python tools/r6ctl.py "rpad 1 0" "wait 30" "rpad 0 0" "shot"
  python tools/r6ctl.py -f tests/look.txt            commands from a file (one per line)
  python tools/r6ctl.py                              interactive prompt

Commands: tap x y | down/move/up x y id | key code | keydown code | keyup code |
          lpad dx dy | rpad dx dy | shot | wait N | +N <command> | status |
          set <setting> <value> | reload | quit
"""
import socket
import sys

PORT = 47800


def main():
    args = sys.argv[1:]
    port = PORT
    if args[:1] == ["-p"]:
        port, args = int(args[1]), args[2:]
    if args[:1] == ["-f"]:
        with open(args[1]) as f:
            args = [l.strip() for l in f if l.strip() and not l.startswith("#")]
    with socket.create_connection(("127.0.0.1", port)) as s:
        reader = s.makefile("r")

        def run(cmd):
            s.sendall((cmd + "\n").encode())
            print(f"{cmd} -> {reader.readline().rstrip()}", flush=True)

        if args:
            for cmd in args:
                run(cmd)
        else:
            try:
                while True:
                    cmd = input("r6> ").strip()
                    if cmd:
                        run(cmd)
            except (EOFError, KeyboardInterrupt):
                pass


if __name__ == "__main__":
    main()
