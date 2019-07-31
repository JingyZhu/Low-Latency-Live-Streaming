#!/bin/bash

# Puling side of live streaming
# IP is the ipv4 address of push side
# Parameters (fps. resolution) should be consistent with the pushing side

IP=192.168.1.103
height=1280
width=720
fps=60

nc $IP 9000 | ./vaapi_decode - - | mplayer -benchmark - -demuxer rawvideo -rawvideo w=${height}:h=${width}:fps=${fps}:format=nv12
