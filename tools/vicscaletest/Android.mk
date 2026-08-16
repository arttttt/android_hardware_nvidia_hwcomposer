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

# A measuring stand, not a module anything depends on. Build it by name
# (mmm hardware/nvidia/hwcomposer/tools/vicscaletest), push it to the
# device, run it from a shell; the vendor libraries it talks to are found
# by dlopen at run time.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := vicscaletest
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := vicscaletest.cpp
LOCAL_SHARED_LIBRARIES := libui libutils liblog libdl
LOCAL_CFLAGS := -Wall -Werror
include $(BUILD_EXECUTABLE)
