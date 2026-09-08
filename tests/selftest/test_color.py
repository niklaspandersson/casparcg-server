#!/usr/bin/env python3
import socket
import time

def send_command(sock, cmd):
    sock.sendall((cmd + "\r\n").encode())
    response = b""
    sock.settimeout(5)
    while True:
        try:
            data = sock.recv(4096)
            if not data:
                break
            response += data
            if response.endswith(b"\r\n") and (b"200" in response or b"201" in response or b"202" in response or
                                                b"400" in response or b"401" in response or b"404" in response or
                                                b"500" in response or b"501" in response or b"502" in response):
                break
        except socket.timeout:
            break
    return response.decode().strip()

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(("localhost", 5250))
time.sleep(0.5)

print("Testing VERSION...")
response = send_command(sock, "VERSION")
print(f"Response: {response}")

print("\nTesting PLAY 1-1 COLOR RED...")
response = send_command(sock, "PLAY 1-1 COLOR RED")
print(f"Response: {response}")

sock.close()
