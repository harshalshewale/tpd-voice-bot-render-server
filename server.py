"""
ESP32 Voice Bot Backend Server (Render.com / Cloud-friendly)
=============================================================
Lightweight server that uses Google Gemini for BOTH speech-to-text and chat
(in a single API call), then gTTS for free text-to-speech.

Single API key required: GEMINI_API_KEY  (free at https://aistudio.google.com/apikey)
RAM footprint: ~80 MB. Fits on Render free tier (512 MB).

Endpoints:
  POST /chat        body: raw PCM-16 mono 16kHz audio bytes
                    -> 200, headers X-Transcript / X-Reply, MP3 cached for /reply.mp3
                    -> 204 if no speech detected
  GET  /reply.mp3   -> serves the most recent TTS reply
  POST /reset       -> clear conversation memory
  GET  /health      -> simple ok

Run locally:
  pip install -r requirements.txt
  export GEMINI_API_KEY="..."
  python server.py
"""

import io
import os
import sys
import time
import wave
import logging
import threading
from urllib.parse import quote

from flask import Flask, request, send_file, jsonify, Response
import google.generativeai as genai
from gtts import gTTS

# ============================================================
# CONFIG
# ============================================================
PORT             = int(os.environ.get("PORT", 5005))   # Render injects PORT env
SAMPLE_RATE      = 16000
GEMINI_MODEL     = "gemini-2.5-flash"
TTS_LANG         = "en"
TTS_TLD          = "com"
SYSTEM_PROMPT    = (
    "You are a friendly, helpful voice assistant. "
    "First, transcribe the user's audio in your head, then reply to them. "
    "Reply in 1-2 short conversational sentences. "
    "Do NOT use markdown, bullet points, or special characters — your response will be spoken aloud. "
    "Always start your reply with the marker [HEARD] followed by what you heard the user say, then [REPLY] followed by your spoken response. "
    "Example: [HEARD] What is the capital of France? [REPLY] The capital of France is Paris."
)
MAX_HISTORY      = 6   # last N turns kept in memory

# ============================================================
# STATE
# ============================================================
conversation_history = []   # list of {"role": "user"|"model", "parts": [...]}
last_mp3_bytes  = b""
last_mp3_lock   = threading.Lock()

# ============================================================
# LOGGING
# ============================================================
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s | %(levelname)s | %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("voicebot")

# ============================================================
# GEMINI CLIENT
# ============================================================
api_key = os.environ.get("GEMINI_API_KEY")
if not api_key:
    log.error("Set the GEMINI_API_KEY environment variable!")
    sys.exit(1)
genai.configure(api_key=api_key)
gemini = genai.GenerativeModel(GEMINI_MODEL, system_instruction=SYSTEM_PROMPT)
log.info("Gemini ready (model=%s).", GEMINI_MODEL)

# ============================================================
# HELPERS
# ============================================================
def pcm_to_wav_bytes(pcm_bytes: bytes, sample_rate: int = SAMPLE_RATE) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    buf.seek(0)
    return buf.read()


def parse_response(raw: str):
    """Split [HEARD] ... [REPLY] ... format. Falls back if model didn't follow it."""
    raw = raw.strip().replace("\n", " ")
    transcript = ""
    reply = raw
    if "[HEARD]" in raw and "[REPLY]" in raw:
        try:
            after_heard = raw.split("[HEARD]", 1)[1]
            transcript, reply = after_heard.split("[REPLY]", 1)
            transcript = transcript.strip()
            reply = reply.strip()
        except Exception:
            pass
    return transcript, reply


def chat_with_audio(pcm_bytes: bytes):
    """Send audio + history to Gemini. Returns (transcript, reply)."""
    global conversation_history

    wav_bytes = pcm_to_wav_bytes(pcm_bytes)

    # Add user audio turn to history
    user_turn = {
        "role": "user",
        "parts": [
            {"mime_type": "audio/wav", "data": wav_bytes},
        ],
    }
    # Compose contents: history + this turn
    contents = list(conversation_history) + [user_turn]

    response = gemini.generate_content(contents)
    raw = response.text
    transcript, reply = parse_response(raw)

    # Save text-only versions in history (keep memory small)
    if transcript:
        conversation_history.append({"role": "user", "parts": [transcript]})
    else:
        # If parsing failed, store something so context is preserved
        conversation_history.append({"role": "user", "parts": ["(audio message)"]})
    conversation_history.append({"role": "model", "parts": [reply]})
    conversation_history = conversation_history[-MAX_HISTORY:]

    return transcript, reply


def synthesize(text: str) -> bytes:
    buf = io.BytesIO()
    gTTS(text=text, lang=TTS_LANG, tld=TTS_TLD, slow=False).write_to_fp(buf)
    return buf.getvalue()


# ============================================================
# FLASK
# ============================================================
app = Flask(__name__)


@app.route("/", methods=["GET"])
def index():
    return "ESP32 Voice Bot is running. POST audio to /chat", 200


@app.route("/health", methods=["GET"])
def health():
    return jsonify(status="ok"), 200


@app.route("/reset", methods=["POST"])
def reset():
    global conversation_history
    conversation_history = []
    log.info("Conversation history cleared.")
    return jsonify(status="ok"), 200


@app.route("/chat", methods=["POST"])
def chat():
    global last_mp3_bytes
    t0 = time.time()
    pcm = request.get_data()
    if not pcm:
        return jsonify(error="empty_body"), 400
    log.info("Received %d bytes of PCM (~%.2f sec)",
             len(pcm), len(pcm) / (SAMPLE_RATE * 2))

    try:
        # 1. Gemini does STT + chat in one call
        t = time.time()
        transcript, reply = chat_with_audio(pcm)
        log.info("Gemini (%.2fs)  heard=%r  reply=%r",
                 time.time() - t, transcript, reply)

        if not reply:
            return ("", 204)

        # 2. TTS
        t = time.time()
        mp3 = synthesize(reply)
        log.info("TTS (%.2fs): %d bytes mp3", time.time() - t, len(mp3))

        with last_mp3_lock:
            last_mp3_bytes = mp3

        log.info("Total turn: %.2fs", time.time() - t0)

        resp = Response("ok", status=200, mimetype="text/plain")
        resp.headers["X-Transcript"] = quote(transcript[:300])
        resp.headers["X-Reply"]      = quote(reply[:300])
        return resp

    except Exception as e:
        log.exception("chat() failed")
        return jsonify(error=str(e)), 500


@app.route("/reply.mp3", methods=["GET"])
def reply_mp3():
    with last_mp3_lock:
        data = last_mp3_bytes
    if not data:
        return jsonify(error="no_reply_yet"), 404
    return send_file(
        io.BytesIO(data),
        mimetype="audio/mpeg",
        as_attachment=False,
        download_name="reply.mp3",
    )


# ============================================================
# RUN
# ============================================================
if __name__ == "__main__":
    log.info("Listening on http://0.0.0.0:%d", PORT)
    app.run(host="0.0.0.0", port=PORT, debug=False, threaded=True)
