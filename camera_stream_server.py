#!/usr/bin/env python3
"""TCP JPEG camera stream server for the Meta Quest teleop APK.

Protocol, server -> APK:
  uint32_be header_len
  header_len bytes UTF-8 JSON
  uint32_be jpeg_len
  jpeg_len bytes JPEG

Protocol, APK -> server, optional ASCII commands:
  VIEW 0\n
  VIEW head\n
  NEXT\n
  PREV\n

Each TCP client keeps its own selected view, so one headset can switch streams
without affecting another headset.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import socket
import struct
import sys
import threading
import time
from typing import Protocol

import cv2
import numpy as np


_ROOT = Path(__file__).resolve().parents[1]
_SONIC_ROOT = _ROOT.parent / "TELEOPERATION_SONIC"
for _candidate in (
    _SONIC_ROOT,
    _SONIC_ROOT / "gear_sonic",
    _SONIC_ROOT / "external_dependencies" / "unitree_sdk2_python",
):
    if _candidate.exists() and str(_candidate) not in sys.path:
        sys.path.insert(0, str(_candidate))


@dataclass(frozen=True)
class Frame:
    view_name: str
    jpeg: bytes
    timestamp: float
    width: int | None = None
    height: int | None = None


class FrameSource(Protocol):
    def start(self) -> None: ...

    def stop(self) -> None: ...

    def view_names(self) -> list[str]: ...

    def get_frame(self, view_index: int) -> Frame | None: ...


def _encode_jpeg(image: np.ndarray, quality: int) -> bytes | None:
    if image is None:
        return None
    if image.dtype != np.uint8:
        image = image.astype(np.uint8)
    ok, encoded = cv2.imencode(".jpg", image, [int(cv2.IMWRITE_JPEG_QUALITY), quality])
    if not ok:
        return None
    return encoded.tobytes()


def _jpeg_size(jpeg: bytes) -> tuple[int | None, int | None]:
    image = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    if image is None:
        return None, None
    return int(image.shape[1]), int(image.shape[0])


def _image_size(image: np.ndarray) -> tuple[int | None, int | None]:
    if image is None or image.ndim < 2:
        return None, None
    return int(image.shape[1]), int(image.shape[0])


class LatestFrames:
    def __init__(self, names: list[str]):
        self._names = names
        self._frames: dict[str, Frame] = {}
        self._lock = threading.Lock()

    def set(self, name: str, jpeg: bytes, width: int | None = None, height: int | None = None) -> None:
        with self._lock:
            self._frames[name] = Frame(name, jpeg, time.time(), width, height)

    def get(self, index: int) -> Frame | None:
        names = self._names
        if not names:
            return None
        name = names[index % len(names)]
        with self._lock:
            return self._frames.get(name)

    def names(self) -> list[str]:
        return list(self._names)


class OpenCVCameraSource:
    def __init__(self, devices: list[str], names: list[str], fps: float, quality: int):
        if len(devices) != len(names):
            raise ValueError("--opencv-devices and --view-names must have the same length")
        self.devices = devices
        self.names = names
        self.fps = fps
        self.quality = quality
        self.frames = LatestFrames(names)
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []

    def start(self) -> None:
        for device, name in zip(self.devices, self.names, strict=True):
            thread = threading.Thread(target=self._worker, args=(device, name), daemon=True)
            thread.start()
            self._threads.append(thread)

    def stop(self) -> None:
        self._stop.set()
        for thread in self._threads:
            thread.join(timeout=2.0)

    def view_names(self) -> list[str]:
        return self.frames.names()

    def get_frame(self, view_index: int) -> Frame | None:
        return self.frames.get(view_index)

    def _worker(self, device: str, name: str) -> None:
        device_arg: int | str = int(device) if device.isdigit() else device
        cap = cv2.VideoCapture(device_arg)
        interval = 1.0 / max(self.fps, 1.0)
        try:
            while not self._stop.is_set():
                ok, image = cap.read()
                if ok:
                    width, height = _image_size(image)
                    jpeg = _encode_jpeg(image, self.quality)
                    if jpeg is not None:
                        self.frames.set(name, jpeg, width, height)
                time.sleep(interval)
        finally:
            cap.release()


class RealSenseCameraSource:
    def __init__(self, names: list[str], fps: int, quality: int):
        self.names = names
        self.fps = fps
        self.quality = quality
        self.frames = LatestFrames(names)
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._worker, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)

    def view_names(self) -> list[str]:
        return self.frames.names()

    def get_frame(self, view_index: int) -> Frame | None:
        return self.frames.get(view_index)

    def _worker(self) -> None:
        import pyrealsense2 as rs

        pipeline = rs.pipeline()
        config = rs.config()
        config.enable_stream(rs.stream.color, 640, 480, rs.format.bgr8, self.fps)
        if len(self.names) > 1:
            config.enable_stream(rs.stream.depth, 640, 480, rs.format.z16, self.fps)
        pipeline.start(config)
        try:
            while not self._stop.is_set():
                rs_frames = pipeline.wait_for_frames()
                color_frame = rs_frames.get_color_frame()
                if color_frame:
                    color = np.asanyarray(color_frame.get_data())
                    width, height = _image_size(color)
                    jpeg = _encode_jpeg(color, self.quality)
                    if jpeg is not None:
                        self.frames.set(self.names[0], jpeg, width, height)
                if len(self.names) > 1:
                    depth_frame = rs_frames.get_depth_frame()
                    if depth_frame:
                        depth = np.asanyarray(depth_frame.get_data())
                        depth_vis = cv2.applyColorMap(
                            cv2.convertScaleAbs(depth, alpha=0.03), cv2.COLORMAP_JET
                        )
                        width, height = _image_size(depth_vis)
                        jpeg = _encode_jpeg(depth_vis, self.quality)
                        if jpeg is not None:
                            self.frames.set(self.names[1], jpeg, width, height)
        finally:
            pipeline.stop()


class ZmqCameraSource:
    def __init__(self, server_ip: str, port: int, keys: list[str], fps: float, quality: int):
        self.server_ip = server_ip
        self.port = port
        self.keys = keys
        self.fps = fps
        self.quality = quality
        self.frames = LatestFrames(keys)
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._worker, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)

    def view_names(self) -> list[str]:
        return self.frames.names()

    def get_frame(self, view_index: int) -> Frame | None:
        return self.frames.get(view_index)

    def _worker(self) -> None:
        from gear_sonic.camera.composed_camera import ComposedCameraClientSensor

        client = ComposedCameraClientSensor(server_ip=self.server_ip, port=self.port)
        interval = 1.0 / max(self.fps, 1.0)
        next_read_time = 0.0
        try:
            while not self._stop.is_set():
                now = time.time()
                if now < next_read_time:
                    time.sleep(min(next_read_time - now, 0.01))
                    continue
                next_read_time = now + interval
                message = client.read(blocking=False)
                if not message:
                    time.sleep(0.005)
                    continue
                images = message.get("images", {})
                for key in self.keys:
                    image = images.get(key)
                    if image is None:
                        continue
                    width = None
                    height = None
                    if isinstance(image, bytes | bytearray):
                        jpeg = bytes(image)
                    else:
                        # ImageMessageSchema returns RGB for raw-byte ZMQ images; convert for OpenCV JPEG.
                        if isinstance(image, np.ndarray) and image.ndim == 3:
                            width, height = _image_size(image)
                            image = cv2.cvtColor(image, cv2.COLOR_RGB2BGR)
                        jpeg = _encode_jpeg(image, self.quality)
                    if jpeg is not None:
                        self.frames.set(key, jpeg, width, height)
        finally:
            client.close()


class UnitreeGo2CameraSource:
    def __init__(self, network_interface: str | None, quality: int):
        self.network_interface = network_interface
        self.quality = quality
        self.frames = LatestFrames(["front"])
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._worker, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)

    def view_names(self) -> list[str]:
        return self.frames.names()

    def get_frame(self, view_index: int) -> Frame | None:
        return self.frames.get(view_index)

    def _worker(self) -> None:
        from unitree_sdk2py.core.channel import ChannelFactoryInitialize
        from unitree_sdk2py.go2.video.video_client import VideoClient

        if self.network_interface:
            ChannelFactoryInitialize(0, self.network_interface)
        else:
            ChannelFactoryInitialize(0)
        client = VideoClient()
        client.SetTimeout(3.0)
        client.Init()
        while not self._stop.is_set():
            code, data = client.GetImageSample()
            if code == 0 and data:
                self.frames.set("front", bytes(data))
            else:
                time.sleep(0.05)


class UnitreeB2CameraSource:
    def __init__(self, network_interface: str | None):
        self.network_interface = network_interface
        self.frames = LatestFrames(["front", "back"])
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._worker, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)

    def view_names(self) -> list[str]:
        return self.frames.names()

    def get_frame(self, view_index: int) -> Frame | None:
        return self.frames.get(view_index)

    def _worker(self) -> None:
        from unitree_sdk2py.b2.back_video.back_video_client import BackVideoClient
        from unitree_sdk2py.b2.front_video.front_video_client import FrontVideoClient
        from unitree_sdk2py.core.channel import ChannelFactoryInitialize

        if self.network_interface:
            ChannelFactoryInitialize(0, self.network_interface)
        else:
            ChannelFactoryInitialize(0)
        front = FrontVideoClient()
        front.SetTimeout(3.0)
        front.Init()
        back = BackVideoClient()
        back.SetTimeout(3.0)
        back.Init()
        while not self._stop.is_set():
            front_code, front_data = front.GetImageSample()
            if front_code == 0 and front_data:
                self.frames.set("front", bytes(front_data))
            back_code, back_data = back.GetImageSample()
            if back_code == 0 and back_data:
                self.frames.set("back", bytes(back_data))
            if front_code != 0 and back_code != 0:
                time.sleep(0.05)


class CameraStreamServer:
    def __init__(self, source: FrameSource, host: str, port: int, fps: float):
        self.source = source
        self.host = host
        self.port = port
        self.fps = fps
        self._stop = threading.Event()
        self._client_count = 0

    def serve_forever(self) -> None:
        self.source.start()
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server_sock:
                server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server_sock.bind((self.host, self.port))
                server_sock.listen(4)
                server_sock.settimeout(0.5)
                print(f"Camera stream server listening on {self.host}:{self.port}")
                print(f"Available views: {', '.join(self.source.view_names())}")
                while not self._stop.is_set():
                    try:
                        client_sock, addr = server_sock.accept()
                    except socket.timeout:
                        continue
                    self._client_count += 1
                    client_id = self._client_count
                    thread = threading.Thread(
                        target=self._serve_client,
                        args=(client_id, client_sock, addr),
                        daemon=True,
                    )
                    thread.start()
        finally:
            self.source.stop()

    def stop(self) -> None:
        self._stop.set()

    def _serve_client(self, client_id: int, client_sock: socket.socket, addr) -> None:
        view_index = 0
        command_buffer = ""
        frame_id = 0
        interval = 1.0 / max(self.fps, 1.0)
        client_sock.setblocking(True)
        print(f"Client {client_id} connected from {addr}")
        try:
            with client_sock:
                while not self._stop.is_set():
                    command_buffer, view_index = self._read_commands(
                        client_sock, command_buffer, view_index
                    )
                    frame = self.source.get_frame(view_index)
                    if frame is None:
                        time.sleep(0.01)
                        continue
                    frame_id += 1
                    self._send_frame(client_sock, frame, view_index, frame_id)
                    time.sleep(interval)
        except (BrokenPipeError, ConnectionResetError, OSError) as exc:
            print(f"Client {client_id} disconnected: {exc}")

    def _read_commands(
        self, client_sock: socket.socket, command_buffer: str, view_index: int
    ) -> tuple[str, int]:
        import select
        ready, _, _ = select.select([client_sock], [], [], 0.001)
        if not ready:
            return command_buffer, view_index
        try:
            data = client_sock.recv(1024)
        except (BlockingIOError, socket.timeout):
            return command_buffer, view_index
        if not data:
            return command_buffer, view_index
        command_buffer += data.decode("utf-8", errors="ignore")
        while "\n" in command_buffer:
            command, command_buffer = command_buffer.split("\n", 1)
            view_index = self._apply_command(command.strip(), view_index)
        return command_buffer, view_index

    def _apply_command(self, command: str, view_index: int) -> int:
        views = self.source.view_names()
        if not views or not command:
            return view_index
        parts = command.split(maxsplit=1)
        action = parts[0].upper()
        if action == "NEXT":
            return (view_index + 1) % len(views)
        if action == "PREV":
            return (view_index - 1) % len(views)
        if action == "VIEW" and len(parts) == 2:
            value = parts[1].strip()
            if value.isdigit():
                return int(value) % len(views)
            if value in views:
                return views.index(value)
            print(f"Unknown view '{value}'. Available views: {views}")
        return view_index

    def _send_frame(
        self, client_sock: socket.socket, frame: Frame, view_index: int, frame_id: int
    ) -> None:
        header = {
            "protocol": "meta_quest_teleop.camera.v1",
            "frame_id": frame_id,
            "timestamp": frame.timestamp,
            "view_index": view_index,
            "view_name": frame.view_name,
            "views": self.source.view_names(),
            "encoding": "jpeg",
            "width": frame.width,
            "height": frame.height,
        }
        header_bytes = json.dumps(header, separators=(",", ":")).encode("utf-8")
        client_sock.sendall(struct.pack("!I", len(header_bytes)))
        client_sock.sendall(header_bytes)
        client_sock.sendall(struct.pack("!I", len(frame.jpeg)))
        client_sock.sendall(frame.jpeg)


def _split_csv(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def build_source(args: argparse.Namespace) -> FrameSource:
    if args.source == "opencv":
        devices = _split_csv(args.opencv_devices)
        names = _split_csv(args.view_names) if args.view_names else [f"camera_{i}" for i in range(len(devices))]
        return OpenCVCameraSource(devices, names, args.camera_fps, args.jpeg_quality)
    if args.source == "realsense":
        names = _split_csv(args.view_names) if args.view_names else ["color", "depth"]
        return RealSenseCameraSource(names, int(args.camera_fps), args.jpeg_quality)
    if args.source == "zmq":
        keys = _split_csv(args.zmq_keys)
        return ZmqCameraSource(args.zmq_ip, args.zmq_port, keys, args.camera_fps, args.jpeg_quality)
    if args.source == "unitree-go2":
        return UnitreeGo2CameraSource(args.network_interface, args.jpeg_quality)
    if args.source == "unitree-b2":
        return UnitreeB2CameraSource(args.network_interface)
    raise ValueError(f"Unsupported source: {args.source}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        choices=["realsense", "zmq", "opencv", "unitree-go2", "unitree-b2"],
        default="realsense",
        help="Camera source. Use realsense for G1 head RGB/depth, or zmq for gear_sonic camera server.",
    )
    parser.add_argument("--host", default="0.0.0.0", help="TCP bind host")
    parser.add_argument("--port", type=int, default=5566, help="TCP bind port")
    parser.add_argument("--stream-fps", type=float, default=30.0, help="FPS sent to each Quest client")
    parser.add_argument("--camera-fps", type=float, default=30.0, help="FPS requested from camera source")
    parser.add_argument("--jpeg-quality", type=int, default=80, help="JPEG quality 1-100")
    parser.add_argument(
        "--view-names",
        default="",
        help="Comma-separated view names. Defaults depend on source.",
    )
    parser.add_argument(
        "--opencv-devices",
        default="0,1",
        help="Comma-separated OpenCV devices for --source opencv, e.g. 0,1 or /dev/video0,/dev/video2",
    )
    parser.add_argument("--zmq-ip", default="localhost", help="gear_sonic camera ZMQ server IP")
    parser.add_argument("--zmq-port", type=int, default=5555, help="gear_sonic camera ZMQ server port")
    parser.add_argument(
        "--zmq-keys",
        default="head,ego_view",
        help="Comma-separated ImageMessageSchema keys to expose as switchable views",
    )
    parser.add_argument(
        "--network-interface",
        default=None,
        help="Unitree SDK network interface, e.g. enp2s0. Used by unitree-go2/unitree-b2.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    source = build_source(args)
    server = CameraStreamServer(source, args.host, args.port, args.stream_fps)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("Stopping camera stream server...")
        server.stop()


if __name__ == "__main__":
    main()
