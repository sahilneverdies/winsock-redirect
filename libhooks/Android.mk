LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := hooker
LOCAL_SRC_FILES := hooker.c
LOCAL_LDLIBS := -llog
include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := libhooks
LOCAL_SRC_FILES := libhooks.c
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)
