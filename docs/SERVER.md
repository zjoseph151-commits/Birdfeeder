# Server Notes

The server is a local Flask app that accepts raw MJPEG uploads from the ESP32 and converts them to AVI files.

The server does not publish MQTT. MQTT metadata is published only by the ESP32 firmware.

## Raspberry Pi Run

Expected Pi path:

```bash
/home/zack/projects/Birdfeeder
```

From that folder:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
python server/bird_server.py
```

At the expected Pi path, videos are saved to:

```text
/home/zack/projects/Birdfeeder/bird_videos
```

The folder can also be overridden when needed:

```bash
BIRD_VIDEO_FOLDER=/home/zack/projects/Birdfeeder/bird_videos python server/bird_server.py
```

## Windows Development Run

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
python .\server\bird_server.py
```

## Endpoints

`GET /health`

Returns a simple health response.

`POST /upload`

Receives raw concatenated JPEG frames from the ESP32.

`POST /stream_upload`

Legacy endpoint kept for compatibility with earlier firmware.

## Output

Videos are saved to `bird_videos/` by default. The raw `.mjpeg` fallback is kept only when conversion fails.
