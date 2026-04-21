# 1. Basic configuration
APP := ds-tracking-app
SRCS := c++_src/App.c++
TARGET_DEVICE := $(shell uname -m)


# 2. Paths to SDKs (Check whether your CUDA version is 11.4 or 12.2)
CUDA_VER := 12.2
DS_PATH := /opt/nvidia/deepstream/deepstream
CUDA_PATH := /usr/local/cuda-$(CUDA_VER)


# In case you do not know the exact version, we use the generic path
ifeq ($(wildcard $(CUDA_PATH)),)
    CUDA_PATH := /usr/local/cuda
endif


# 3. GStreamer and GLib components
PKGS := gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0 glib-2.0


# 4. Compilation flags (Headers)
CXXFLAGS := -O3 -std=c++17
CFLAGS := -I$(DS_PATH)/sources/includes \
          -I$(CUDA_PATH)/include \
          $(shell pkg-config --cflags $(PKGS))


# 5. Linking flags (Libraries)
LIBS := $(shell pkg-config --libs $(PKGS))
LIBS += -L$(DS_PATH)/lib \
        -lnvdsgst_meta -lnvds_meta -lnvdsgst_helper \
        -lstdc++fs \
        -Wl,-rpath,$(DS_PATH)/lib


# 6. Build rules
all: $(APP)


$(APP): $(SRCS)
    g++ $(CXXFLAGS) $(CFLAGS) -o $(APP) $(SRCS) $(LIBS)


clean:
    rm -f $(APP)


.PHONY: all clean