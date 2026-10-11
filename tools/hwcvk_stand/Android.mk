#
# Copyright (C) 2026 Artem Bambalov
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# A measuring stand for the GPU executor library, loaded by path the way
# the composer loads it. Build it by name (m hwcvk_stand hwcvk.tegra).

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := hwcvk_stand
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := hwcvk_stand.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../..
LOCAL_SHARED_LIBRARIES := libui libutils liblog libcutils libdl
LOCAL_CFLAGS := -Wall -Werror -Wno-unused-parameter
include $(BUILD_EXECUTABLE)
