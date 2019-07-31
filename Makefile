LIVE_LIBS=	libavdevice		\
			libavformat		\
			libavfilter		\
			libavcodec		\
			libswresample	\
			libswscale		\
			libavutil		\

CFLAGS := $(shell pkg-config --cflags $(LIVE_LIBS)) $(CFLAGS)
LDLIBS := $(shell pkg-config --libs $(LIVE_LIBS)) $(LDLIBS)

ALL= 	vaapi_encode		\
		vaapi_decode		\
		sc_vaapi_encode		\
		capture_screen		\

all: $(ALL)

clean:
	$(RM) $(ALL)