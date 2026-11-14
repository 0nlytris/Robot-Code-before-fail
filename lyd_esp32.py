import time

import requests

# IP-adressen lydkortet skriver i Serial Monitor ("Lyd-URL: ...")
ESP32_LYD_URL = "http://192.168.10.125:85"
PCM_RATE = 24000  # OpenAI "pcm": 24 kHz, 16 bit, mono


def snakk(tekst, client):
    """Tekst -> OpenAI-tale -> høyttaleren på roboten. Venter til talen er ferdig."""
    pcm = client.audio.speech.create(
        model="gpt-4o-mini-tts",
        voice="marin",
        input=tekst,
        instructions="Speak naturally and clearly.",
        response_format="pcm",
    ).content

    if not pcm:
        return

    try:
        svar = requests.post(f"{ESP32_LYD_URL}/audio", data=pcm, timeout=(2, 30))
    except requests.RequestException as feil:
        print(f"ADVARSEL: fikk ikke kontakt med lydkortet på {ESP32_LYD_URL}: {feil}")
        return

    if svar.status_code != 200:
        print(f"ADVARSEL: lydkortet svarte {svar.status_code}: {svar.text.strip()}")
        return

    vent_til_ferdig(len(pcm) / 2 / PCM_RATE)


def vent_til_ferdig(sekunder):
    """Sover mens talen spilles, og spør lydkortet til det svarer IDLE (maks 3 s ekstra).
    Da kommer aldri en ny setning mens den forrige spilles, og roboten hører ikke seg selv."""
    time.sleep(max(sekunder - 0.3, 0))

    slutt = time.monotonic() + 3.3
    while time.monotonic() < slutt:
        try:
            if requests.get(f"{ESP32_LYD_URL}/status", timeout=2).text.strip() == "IDLE":
                return
        except requests.RequestException:
            pass
        time.sleep(0.1)
