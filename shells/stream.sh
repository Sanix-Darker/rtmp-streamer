BITRATE="2500k" # Bitrate of the output video
FPS="30" # FPS video output
QUAL="medium" # FFMPEG quality preset
SOURCE="/dev/video0" # Radio Station
KEY="your_strean_key" # Stream name/key
SIZE="1920x1080"
FRAMERATE="2"
RTMPLINK="rtmp://127.0.0.1:1935/live/boumboum"
    ffmpeg -re -loop 1 \
    	-framerate "$FRAMERATE" \
    	-i "$SOURCE" \
    	-c:a aac \
    	-s "$SIZE" \
    	-ab 128k \
    	-b:v "$BITRATE" \
    	-threads 6 \
    	-qscale 3 \
    	-preset veryfast \
    	-vcodec libx264 \
    	-pix_fmt yuv420p \
    	-maxrate 2048k \
    	-bufsize 2048k \
    	-framerate 30 \
    	-g 60 \
    	-f flv \
    	"$RTMPLINK"
