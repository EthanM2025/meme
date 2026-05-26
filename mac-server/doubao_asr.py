"""
Doubao (火山引擎 大模型录音文件识别) ASR client.

Async submit + poll pattern. WAV file is sent inline as base64 in `audio.data`,
so no public URL hosting is needed.
"""
import asyncio
import base64
import os
import time
import uuid

try:
    import aiohttp
except ImportError:
    import sys
    print("Need `pip install aiohttp`", file=sys.stderr)
    raise


SUBMIT_URL = "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit"
QUERY_URL  = "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query"

# Read once from env at import time.
API_KEY     = os.environ.get("DOUBAO_API_KEY", "")
RESOURCE_ID = os.environ.get("DOUBAO_RESOURCE_ID", "volc.seedasr.auc")


def _common_headers(req_id: str) -> dict:
    return {
        "Content-Type": "application/json",
        "x-api-key": API_KEY,
        "X-Api-Resource-Id": RESOURCE_ID,
        "X-Api-Request-Id": req_id,
        "X-Api-Sequence": "-1",
    }


async def transcribe_wav(wav_path: str, timeout_s: float = 15.0) -> dict:
    """
    Submit a WAV file and poll until done. Returns:
      {"text": "...", "duration_ms": int, "raw": <full json>} on success
      {"error": "...", "raw": <full json or None>} on failure
    """
    if not API_KEY:
        return {"error": "DOUBAO_API_KEY env var is empty"}

    with open(wav_path, "rb") as f:
        wav_bytes = f.read()
    b64 = base64.b64encode(wav_bytes).decode("ascii")
    req_id = str(uuid.uuid4())

    body = {
        "user": {"uid": "clawpets"},
        "audio": {
            "data":    b64,
            "format":  "wav",
            "rate":    16000,
            "bits":    16,
            "channel": 1,
        },
        "request": {
            # "bigmodel" = 标准版 (best accuracy, ~300ms slower than bigmodel_fast).
            # Switch back to bigmodel_fast if latency matters more than accuracy.
            "model_name":   "bigmodel",
            "enable_itn":   True,
            "enable_punc":  True,
        },
    }

    async with aiohttp.ClientSession() as sess:
        # 1) Submit
        async with sess.post(SUBMIT_URL, headers=_common_headers(req_id), json=body, timeout=20) as r:
            status = r.headers.get("X-Api-Status-Code", "")
            msg    = r.headers.get("X-Api-Message", "")
            if status != "20000000":
                return {"error": f"submit failed: {status} {msg}", "raw": await r.text()}

        # 2) Poll /query for the result. Start aggressive (100ms) since the fast model
        #    often replies within ~300-500ms. Back off slowly to avoid hammering.
        deadline = time.time() + timeout_s
        delay = 0.1
        while time.time() < deadline:
            await asyncio.sleep(delay)
            async with sess.post(QUERY_URL, headers=_common_headers(req_id), json={}, timeout=10) as r:
                status = r.headers.get("X-Api-Status-Code", "")
                msg    = r.headers.get("X-Api-Message", "")
                data   = await r.json()
                # 20000000 = done, 20000001 = still processing (observed empirically; Volcengine convention)
                if status == "20000000":
                    text = data.get("result", {}).get("text", "")
                    dur  = data.get("audio_info", {}).get("duration", 0)
                    return {"text": text, "duration_ms": dur, "raw": data}
                if status.startswith("4") or status.startswith("5"):
                    return {"error": f"query failed: {status} {msg}", "raw": data}
            delay = min(delay * 1.6, 0.5)  # cap at 500ms — fast model rarely needs more

        return {"error": f"timeout after {timeout_s}s", "raw": None}


if __name__ == "__main__":
    # CLI for quick testing: python3 doubao_asr.py path/to.wav
    import sys
    from dotenv import load_dotenv
    load_dotenv()
    # Refresh from env after dotenv load.
    API_KEY     = os.environ.get("DOUBAO_API_KEY", "") or API_KEY
    RESOURCE_ID = os.environ.get("DOUBAO_RESOURCE_ID", RESOURCE_ID)

    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} path/to/file.wav")
        sys.exit(1)
    res = asyncio.run(transcribe_wav(sys.argv[1]))
    if "error" in res:
        print("ERROR:", res["error"])
        sys.exit(2)
    print(f"[{res['duration_ms']} ms]  {res['text']}")
