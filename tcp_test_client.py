#!/usr/bin/env python3

import argparse
import json
import socket
import time


def send_command(stream, request_id, command):
    message = {
        "type": "command",
        "request_id": request_id,
        "cmd": command,
    }
    stream.write((json.dumps(message, separators=(",", ":")) + "\n").encode())
    stream.flush()


def read_until_response(stream, request_id, deadline):
    while time.monotonic() < deadline:
        line = stream.readline()
        if not line:
            raise ConnectionError("서버가 연결을 종료했습니다.")
        text = line.decode("utf-8").rstrip()
        print(text)
        message = json.loads(text)
        if message.get("type") == "response" and message.get("request_id") == request_id:
            return message
    raise TimeoutError(f"request_id={request_id} 응답 시간이 초과됐습니다.")


def main():
    parser = argparse.ArgumentParser(description="조도 센서 TCP 서버 시험 클라이언트")
    parser.add_argument("host", help="Raspberry Pi IP 주소")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--seconds", type=float, default=5.0)
    args = parser.parse_args()

    with socket.create_connection((args.host, args.port), timeout=3.0) as connection:
        connection.settimeout(2.0)
        with connection.makefile("rwb", buffering=0) as stream:
            send_command(stream, 1, "status")
            read_until_response(stream, 1, time.monotonic() + 2.0)

            send_command(stream, 2, "start")
            read_until_response(stream, 2, time.monotonic() + 2.0)

            finish = time.monotonic() + args.seconds
            while time.monotonic() < finish:
                line = stream.readline()
                if not line:
                    raise ConnectionError("측정 중 서버가 연결을 종료했습니다.")
                print(line.decode("utf-8").rstrip())

            send_command(stream, 3, "stop")
            read_until_response(stream, 3, time.monotonic() + 2.0)


if __name__ == "__main__":
    main()
