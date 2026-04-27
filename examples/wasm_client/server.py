#!/usr/bin/env python3
"""Simple WebSocket echo server for testing the WASM client."""

import asyncio
import websockets


async def echo_handler(websocket):
    peer = websocket.remote_address
    print(f"Client connected from {peer}")
    try:
        async for message in websocket:
            print(f"  Received: {message}")
            reply = f"Echo: {message}"
            await websocket.send(reply)
            print(f"  Sent: {reply}")
    except websockets.exceptions.ConnectionClosed:
        print(f"Client {peer} disconnected")


async def main():
    host = "localhost"
    port = 8765
    async with websockets.serve(echo_handler, host, port):
        print(f"WebSocket echo server listening on ws://{host}:{port}")
        print("Press Ctrl+C to stop")
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nServer stopped.")
