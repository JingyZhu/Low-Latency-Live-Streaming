#!/bin/bash

# test.sh is used to test the latency for no encoding-transmission-decoding stages.
# It directly capture the screen, tranform the pix-fmt into nv12, and feed it into the mplayer

height=1280
width=720
fps=60

ffmpeg -f x11grab -framerate 60 -video_size ${height}*${width} -i :0.0 -pix_fmt=nv12 -c:v copy -f rawvideo - | mplayer -benchmark - -demuxer rawvideo -rawvideo w=${height}:h=${width}:fps=${fps}:format=nv12
