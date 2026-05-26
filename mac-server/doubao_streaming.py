"""
Doubao seed-asr streaming (sauc 大模型 2.0) WebSocket client.

Wire protocol (cribbed from xinnan-tech/xiaozhi-esp32-server's doubao_stream.py):
  - WS connect to wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async
    with headers X-Api-App-Key / X-Api-Access-Key / X-Api-Resource-Id / X-Api-Connect-Id
  - Each frame is: [4-byte header] [4-byte big-endian payload length] [gzipped payload]
    - header byte 0: (version<<4) | header_size  (version=1, header_size=1)
    - header byte 1: (message_type<<4) | flags
        - 0x10 = full client request (initial JSON config)
        - 0x20 = audio frame (in-progress PCM)
        - 0x22 = last audio frame (signals end of stream)
        - 0x0F (high nibble) = server error response
        - other = server normal response (utterance partial / final)
    - header byte 2: (serial_method<<4) | compression  (1=JSON, 1=gzip)
    - header byte 3: reserved
  - For audio frames, payload is gzipped raw PCM 16k/16-bit/mono LE
  - Server pushes JSON responses with `utterances` array; entries with
    `definite: true` are finalized text.

Usage:
    s = StreamingASRSession()
    await s.start()
    while pcm := receive_pcm():
        await s.send_audio(pcm)
    text = await s.finalize()
"""
from __future__ import annotations

import asyncio
import gzip
import json
import os
import uuid
from typing import Optional

import websockets


WS_URL          = os.environ.get("DOUBAO_STREAMING_URL", "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async")
RESOURCE_ID     = os.environ.get("DOUBAO_STREAMING_RESOURCE_ID", "volc.seedasr.sauc.duration")
APP_ID          = os.environ.get("DOUBAO_APP_ID", "")
API_KEY         = os.environ.get("DOUBAO_API_KEY", "")


def _build_header(message_type: int, flags: int = 0) -> bytearray:
    """version=1, header_size=1, serial=JSON(1), compression=gzip(1)."""
    return bytearray([
        (0x1 << 4) | 0x1,
        (message_type << 4) | flags,
        (0x1 << 4) | 0x1,
        0,
    ])


def _build_frame(message_type: int, payload_bytes: bytes, last: bool = False) -> bytes:
    flags = 0x2 if last else 0x0
    body = gzip.compress(payload_bytes)
    frame = _build_header(message_type, flags)
    frame.extend(len(body).to_bytes(4, "big"))
    frame.extend(body)
    return bytes(frame)


def _parse_response(data: bytes) -> dict:
    """Return {'error': ...} or {'payload': <dict>}."""
    if len(data) < 4:
        return {"error": f"short response {len(data)}b"}
    message_type = data[1] >> 4

    # Server error response: 4-byte code, 4-byte msg length, JSON.
    if message_type == 0xF:
        code = int.from_bytes(data[4:8], "big")
        msg_len = int.from_bytes(data[8:12], "big")
        try:
            payload = json.loads(data[12:12 + msg_len].decode("utf-8"))
        except Exception:
            payload = {"raw": data[12:].hex()}
        return {"error": f"server_error code={code}", "payload": payload}

    # Normal response: skip 4 bytes header + 4 bytes sequence + 4 bytes length, then JSON.
    if len(data) < 12:
        return {"error": "too short for normal response"}
    length = int.from_bytes(data[8:12], "big")
    body = data[12:12 + length] if 0 < length <= len(data) - 12 else data[8:]
    try:
        return {"payload": json.loads(body.decode("utf-8", errors="replace"))}
    except Exception as e:
        return {"error": f"parse json: {e}", "raw_len": len(body)}


class StreamingASRSession:
    def __init__(self):
        self.ws = None
        self.req_id = str(uuid.uuid4())
        self.text = ""          # latest accumulated text (finalized + current partial)
        self.final_text = ""    # text from utterances with definite=true
        self._seen_finals: set = set()  # keys of utterances already merged into final_text
        self._reader_task: Optional[asyncio.Task] = None
        self._on_partial = None  # callable(text) hook for live UI updates
        # Background-mode bookkeeping. Audio arriving before the WS handshake
        # completes is queued here; once `start()` succeeds, the queue is drained
        # and subsequent `send_audio()` calls go straight to the socket.
        self._connect_task: Optional[asyncio.Task] = None
        self._pending_pcm: Optional[list] = []
        self._send_lock = asyncio.Lock()
        self._ready = asyncio.Event()
        self._connect_error: Optional[Exception] = None

    def set_partial_callback(self, cb):
        self._on_partial = cb

    def start_background(self) -> None:
        """Fire-and-forget WS handshake. `send_audio()` is safe immediately —
        frames are queued until the handshake completes, then drained in order.
        `finalize()` waits for handshake + final result."""
        self._connect_task = asyncio.create_task(self._connect_and_drain())

    async def _connect_and_drain(self) -> None:
        import time as _t
        t0 = _t.time()
        try:
            await self.start()
            print(f"  [stream-asr] connected in {_t.time()-t0:.2f}s")
        except Exception as e:
            print(f"  [stream-asr] connect FAILED after {_t.time()-t0:.2f}s: {e}")
            self._connect_error = e
            self._ready.set()
            return
        async with self._send_lock:
            pending = self._pending_pcm or []
            if pending:
                print(f"  [stream-asr] draining {len(pending)} buffered PCM frames")
            for pcm in pending:
                try:
                    await self.ws.send(_build_frame(0x2, pcm, last=False))
                except Exception as e:
                    print(f"  [stream-asr] drain: {e}")
                    break
            self._pending_pcm = None
            self._ready.set()

    async def start(self) -> None:
        # New-style API key auth (same x-api-key as file-based ASR).
        # Send BOTH old- and new-style headers so the server picks whichever it expects.
        headers = {
            "x-api-key":         API_KEY,
            "X-Api-Access-Key":  API_KEY,
            "X-Api-App-Key":     APP_ID,
            "X-Api-Resource-Id": RESOURCE_ID,
            "X-Api-Connect-Id":  str(uuid.uuid4()),
        }
        self.ws = await websockets.connect(
            WS_URL,
            additional_headers=headers,
            max_size=None,
            ping_interval=None,
            ping_timeout=None,
            close_timeout=10,
            open_timeout=20,
        )
        # Initial config
        config = {
            "app":  {"appid": APP_ID, "token": API_KEY},
            "user": {"uid": "clawpets"},
            "request": {
                "reqid":           self.req_id,
                "workflow":        "audio_in,resample,partition,vad,fe,decode,itn,nlu_punctuate",
                "show_utterances": True,
                "result_type":     "single",
                "sequence":        1,
                "end_window_size": 200,
            },
            "audio": {
                "format":      "pcm",
                "codec":       "pcm",
                "rate":        16000,
                "bits":        16,
                "channel":     1,
                "sample_rate": 16000,
            },
        }
        await self.ws.send(_build_frame(0x1, json.dumps(config).encode("utf-8")))

        # Read initial response
        init_resp = await asyncio.wait_for(self.ws.recv(), timeout=10)
        parsed = _parse_response(init_resp)
        if "error" in parsed:
            raise RuntimeError(f"ASR init failed: {parsed}")

        # Start background reader
        self._reader_task = asyncio.create_task(self._read_loop())

    async def send_audio(self, pcm: bytes) -> None:
        async with self._send_lock:
            if self._pending_pcm is not None:
                # Handshake not done yet — buffer for the drain loop.
                self._pending_pcm.append(pcm)
                return
            if not self.ws:
                return
            try:
                await self.ws.send(_build_frame(0x2, pcm, last=False))
            except Exception as e:
                print(f"  [stream-asr] send_audio: {e}")

    async def finalize(self, timeout_s: float = 10.0) -> str:
        """Send end marker, wait briefly for the final utterance, return text.
        If handshake never completed, returns empty string so caller can fall
        back to file-based ASR on the captured WAV."""
        # Cap how long we wait for the handshake to finish before giving up
        # — the message loop can't be blocked forever.
        if self._connect_task and not self._connect_task.done():
            print(f"  [stream-asr] finalize waiting for in-flight connect (≤3s)")
            try:
                await asyncio.wait_for(asyncio.shield(self._connect_task), timeout=3.0)
            except (asyncio.TimeoutError, Exception):
                print(f"  [stream-asr] connect still pending after 3s → giving up")

        if self._connect_error:
            print(f"  [stream-asr] connect_error: {self._connect_error}")
            await self.close()
            return ""
        if not self.ws:
            print(f"  [stream-asr] no ws after connect — give up")
            await self.close()
            return ""

        # Drain any audio buffered after the connect task finished but before
        # we acquired the lock, then send the end-of-stream marker.
        async with self._send_lock:
            if self._pending_pcm is not None:
                for pcm in self._pending_pcm:
                    try:
                        await self.ws.send(_build_frame(0x2, pcm, last=False))
                    except Exception:
                        pass
                self._pending_pcm = None
            try:
                await self.ws.send(_build_frame(0x2, b"", last=True))
            except Exception:
                pass

        try:
            if self._reader_task:
                await asyncio.wait_for(self._reader_task, timeout=timeout_s)
        except asyncio.TimeoutError:
            pass
        except Exception:
            pass

        await self.close()
        return self.final_text or self.text

    async def close(self) -> None:
        if self._reader_task and not self._reader_task.done():
            self._reader_task.cancel()
            try:
                await self._reader_task
            except Exception:
                pass
        if self.ws:
            try:
                await self.ws.close()
            except Exception:
                pass
            self.ws = None

    async def _read_loop(self) -> None:
        try:
            while self.ws:
                try:
                    data = await self.ws.recv()
                except websockets.ConnectionClosed:
                    return

                parsed = _parse_response(data)
                if "error" in parsed:
                    # Soft errors (e.g. code 1013 no-speech) we just log and continue.
                    payload = parsed.get("payload", {})
                    code = payload.get("code") if isinstance(payload, dict) else None
                    if code == 1013:
                        continue
                    print(f"  [stream-asr] {parsed}")
                    continue

                p = parsed.get("payload", {}) or {}
                if "result" in p:
                    utts = p["result"].get("utterances", []) or []
                    # Doubao streams one VAD-segmented utterance at a time. When
                    # a definite utterance arrives, append it once and remember
                    # it; otherwise the next utterance would overwrite the prior.
                    for u in utts:
                        txt = u.get("text")
                        if u.get("definite") and txt:
                            key = (u.get("start_time"), u.get("end_time"), txt)
                            if key not in self._seen_finals:
                                self._seen_finals.add(key)
                                self.final_text += txt
                    partials_now = [u["text"] for u in utts if not u.get("definite") and u.get("text")]
                    self.text = self.final_text + ("".join(partials_now))
                    if self._on_partial:
                        try:
                            self._on_partial(self.text)
                        except Exception:
                            pass
        except asyncio.CancelledError:
            pass
        except Exception as e:
            print(f"  [stream-asr] reader stopped: {e}")
