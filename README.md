### Low-Latency-Live-Streaming

This is a project focusing on ultra low latency live video streaming. By ultilizing the intel hardware for encoding/decoding acceleration (**VAAPI**), our live streaming project can reduce the latency to **~40ms** in LAN network. 

#### Requirements

To build and run the code, you have to be prepared with the following requirements:

- Hardware: Intel GPU (dedicated GPU or iGPU)

- OS: Linux with Intel VAAPI.
  - You can run ```vainfo``` to check for Intel VAAPI availability.
- Dependencies: FFmpeg (We test and run on FFmpeg 4, but FFmpeg 3 should be fine), Mplayer >= 1.3

#### Build

To build the code, just run

```sh
make
```

#### Run

To run the live streaming:

- First run ```push.sh``` at the pushing side
- Then run ```pull.sh``` at the pulling side

You should then see the live streaming working :)

To modify the streaming configuration, modify the params in script. (Params in push and pull should be consistent)

- You can also run ```test.sh``` to test your screen capturing and playing availability.

#### About

This project is built by Team Fishermen for VE450 Major Design, at UMJI-SJTU.