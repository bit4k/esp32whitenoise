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

import subprocess

for filename in os.listdir("data"):
    if filename.endswith(".mp3"):
        in_path = os.path.join("data", filename)
        tmp_path = os.path.join("data", "tmp_" + filename)
        cmd = ["ffmpeg", "-y", "-i", in_path, "-ar", "44100", "-ac", "2", "-ab", "128k", tmp_path]
        res = subprocess.run(cmd, capture_output=True)
        if res.returncode == 0:
            os.replace(tmp_path, in_path)

print("Done! All MP3 files resampled to 44100Hz stereo. Upload using PlatformIO: 'Upload Filesystem Image'")
