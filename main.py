
import asyncio

from aesp32sense import camera_loop
from openAIKode import ask_openai, listen, speak


async def voice_loop():

    while True:
        print()
        print("1. Start listening")

        try:
            user_text = await asyncio.to_thread(listen)

        except Exception as error:
            print("Speech-to-text error:")
            print(error)
            continue

        if user_text is None:
            print("No speech recognised.")
            continue
            
        user_text = user_text.strip()
        if not user_text:
            continue

        print("User:", user_text)
        print("2. Sending text to OpenAI")

        try:
            answer = await ask_openai(user_text)

        except Exception as error:
            print("OpenAI error:")
            print(error)
            continue

        if answer is None:
            continue

        answer = str(answer).strip()
        print("Robot:", answer)
        print("3. Starting TTS")

        await speak(answer)

        print("4. Going back to listening")


# async def declares a coroutine
# await waits for an async operation to finish
# gather runs multiple coroutines concurrently
# to_thread runs blocking code in a separate thread
async def main():

    print("Starting main")
    await asyncio.gather(
        asyncio.to_thread(camera_loop),
        voice_loop()
    )


if __name__ == "__main__":

    try:
        asyncio.run(main())

    except KeyboardInterrupt:
        print()
        print("Program stopped.")
