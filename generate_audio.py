import os
try:
    from gtts import gTTS
except ImportError:
    print("Please install gTTS: pip install gTTS")
    exit(1)

noises = [
    "White", "Pink", "Brown", "Blue", "Violet",
    "Ocean", "Pink Ocean", "Deep", "Breathing", "Fast"
]

timers = {
    "timer_30.mp3": "30 minutes",
    "timer_60.mp3": "1 hour",
    "timer_endless.mp3": "Endless"
}

os.makedirs("data", exist_ok=True)

for i, name in enumerate(noises):
    print(f"Generating {name}...")
    tts = gTTS(text=name, lang='en')
    tts.save(f"data/{i}.mp3")

for filename, text in timers.items():
    print(f"Generating {filename}...")
    tts = gTTS(text=text, lang='en')
    tts.save(f"data/{filename}")

print("Done! You can now upload the filesystem using PlatformIO: 'Upload Filesystem Image'")
