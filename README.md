# RTMP-STREAMER

A C++ implementation of a RTMP server.

## How to start

Nothing have to build, if we are in an C/C++ environment environment, just hit :

```shell
sh ./shells/build.sh
```

To Start RTMP-STREAMER, just hit this command :

```shell
sh ./shells/start.sh
```

But if you want to build and start at the same time, just hit :

```shell
sh ./shells/build_and_start.sh
```

MICRON is composed in 2 main server, the rtmp server for the streaming and the HTTP-FLV server, available on:

- rtmp://127.0.0.1:1935/application/sessionid for the stream.
- http://127.0.0.1:8080/application/sessionid.flv for the flv serve.


## To test streams with ffmpeg

- Run the camera stream :

```shell
ffmpeg -i /dev/video0 -framerate 1 -video_size 720x404 -vcodec libx264 -maxrate 768k -bufsize 8080k -vf "format=yuv420p" -g 60 -f flv rtmp://127.0.0.1:1935/live/boumboum
```

- Run the audio stream :

```shell
ffmpeg -ar 8000 -f alsa -i hw:0 -acodec mp3 -b:a 128k -f rtp rtp://127.0.0.1:1935/live/boumboum:audio
```

- To stream a complete media file:

```shell
ffmpeg -re -i ~/INPUT_FILE -vcodec libx264 -profile:v main -preset:v medium -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -b:v 2500k -maxrate 2500k -bufsize 2500k -filter:v scale="trunc(oha/2)2:720" -sws_flags lanczos+accurate_rnd -acodec libfdk_aac -b:a 96k -ar 48000 -ac 2 -f flv rtmp://live.twitch.tv/app/STREAM_KEY
```

## Author

- Sanix-darker
