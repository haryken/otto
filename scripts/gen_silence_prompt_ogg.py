#!/usr/bin/env python3
"""Generate main/assets/common/silence_prompt.ogg for SILENCE_PROMPT uplink injection.

Requires: pip install edge-tts av

The clip is Opus-in-OGG at 16 kHz mono so the server STT pipeline accepts it like mic audio.
Default spoken text (English): system silence prompt for server-side LLM/STT.
Sent via SendOggUplink at real-time pace (60 ms/frame), same as mic uplink.
"""

import argparse
import asyncio
import io
import os
import sys

try:
    import av
    import edge_tts
except ImportError:
    print("Install dependencies: pip install edge-tts av", file=sys.stderr)
    sys.exit(1)

DEFAULT_TEXT = (
    "System notification, user silent brief reply in current language to keep chat alive. "
    "Briefly recap the lesson or current topic, then ask the user to speak "
    "or give a short hint to help them remember."
)
DEFAULT_VOICE = "en-US-JennyNeural"


async def synthesize_ogg(text: str, voice: str, output_path: str) -> None:
    communicate = edge_tts.Communicate(text, voice)
    mp3_buffer = io.BytesIO()
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            mp3_buffer.write(chunk["data"])

    mp3_buffer.seek(0)
    if mp3_buffer.getbuffer().nbytes == 0:
        raise RuntimeError("edge-tts returned no audio; check network and voice name")

    inp = av.open(mp3_buffer, format="mp3")
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    out = av.open(output_path, "w", format="ogg")
    in_stream = inp.streams.audio[0]
    out_stream = out.add_stream("libopus", rate=16000)
    out_stream.layout = "mono"
    out_stream.bit_rate = 24000
    out_stream.options = {"application": "voip", "frame_duration": "60"}
    resampler = av.AudioResampler(format="s16", layout="mono", rate=16000)

    for frame in inp.decode(in_stream):
        for resampled in resampler.resample(frame):
            for packet in out_stream.encode(resampled):
                out.mux(packet)
    for packet in out_stream.encode(None):
        out.mux(packet)

    out.close()
    inp.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--text", default=DEFAULT_TEXT, help="Phrase to synthesize")
    parser.add_argument("--voice", default=DEFAULT_VOICE, help="edge-tts voice id")
    parser.add_argument(
        "--output",
        default=os.path.join("main", "assets", "common", "silence_prompt.ogg"),
        help="Output OGG path",
    )
    args = parser.parse_args()
    asyncio.run(synthesize_ogg(args.text, args.voice, args.output))
    print(f"Wrote {args.output} ({os.path.getsize(args.output)} bytes)")


if __name__ == "__main__":
    main()
