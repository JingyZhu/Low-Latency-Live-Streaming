#!/bin/bash

# Pushing side of live streaming
# Parameters (fps. resolution) should be consistent with the pulling side

height=1280
width=720
fps=60

./sc_vaapi_encode ${height} ${width} ${fps} | nc -lp 9000
