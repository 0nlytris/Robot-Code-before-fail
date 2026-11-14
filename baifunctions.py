import requests
import time



ARDUINO_URL = "http://192.168.10.137:84"

# Reuse the HTTP connection
session = requests.Session()


def send_move(direction, duration_seconds):

    duration_ms = int(round(duration_seconds * 1000))

    try:
        response = session.get(f"{ARDUINO_URL}/move/{direction}", params={"ms": duration_ms}, timeout=2.0)
        response.raise_for_status()
        print(f"Robot moved {direction} for {duration_ms} ms")
        return True

    except requests.RequestException as error:
        print("Could not communicate with Arduino:")
        print(error)
        return False


def move_left(duration_seconds):
    return send_move("left", duration_seconds)

def move_right(duration_seconds):
    return send_move("right", duration_seconds)

def navigate(person_name):
    # Lazy import avoids the circular import with baifunctions.py.
    from baifunctions import center_person

    # The camera/PC must align with the person before the Arduino samples
    # the target distance and starts autonomous driving.
    if not center_person(person_name):
        print("Kunne ikke sentrere personen før navigasjon")
        return False

    try:
        session.get(f"{ARDUINO_URL}/move/navigate", timeout=2).raise_for_status()

        while True:
            response = session.get(f"{ARDUINO_URL}/navigate/status", timeout=2)
            response.raise_for_status()
            status = response.text.strip()

            # After obstacle avoidance the Arduino waits here while the PC
            # reacquires and centres the same person using the camera.
            if status == "CENTER":
                ok = center_person(person_name)
                session.get(
                    f"{ARDUINO_URL}/navigate/centered",
                    params={"ok": int(ok)},
                    timeout=2
                ).raise_for_status()

                if not ok:
                    return False

            elif status in ("ARRIVED", "FAILED", "IDLE"):
                return status == "ARRIVED"

            elif status != "RUNNING":
                raise ValueError(f"Ukjent navigasjonsstatus: {status}")

            time.sleep(0.2)

    except (requests.RequestException, ValueError) as error:
        print("Navigasjonsfeil:", error)
        return False
