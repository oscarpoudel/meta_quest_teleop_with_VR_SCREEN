LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

include ../../../ovr_sdk/cflags.mk

LOCAL_MODULE			:= vrinputstandard

LOCAL_C_INCLUDES 		:= 	$(LOCAL_PATH)/../../../../ovr_sdk/SampleCommon/Src \
							$(LOCAL_PATH)/../../../../ovr_sdk/SampleFramework/Src \
							$(LOCAL_PATH)/../../../../ovr_sdk/VrApi/Include \
							$(LOCAL_PATH)/../../../../ovr_sdk/1stParty/OVR/Include \
							$(LOCAL_PATH)/../../../../ovr_sdk/1stParty/utilities/include \
							$(LOCAL_PATH)/../../../../ovr_sdk/3rdParty/stb/src \

LOCAL_SRC_FILES			:= 	../../../Src/main.cpp \
							../../../Src/OculusTeleop.cpp \
							../../../Src/Buttons.cpp \
							../../../Src/TcpJpegReceiver.cpp \
							../../../Src/CameraPanel.cpp \


# include default libraries
LOCAL_LDLIBS 			:= -llog -landroid -lGLESv3 -lEGL -lz
LOCAL_STATIC_LIBRARIES 	:= sampleframework
LOCAL_SHARED_LIBRARIES	:= vrapi

include $(BUILD_SHARED_LIBRARY)

$(call import-module,SampleFramework/Projects/Android/jni)
$(call import-module,VrApi/Projects/AndroidPrebuilt/jni)
