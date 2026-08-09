/*
 * Copyright (C) 2022 The Android Open Source Project
 * Copyright (C) 2026 Artem Bambalov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Adapted from drm-hwcomposer's drm/DrmConnector.h.
 *
 * What is on the far end of a display's output: which timings it runs, how
 * large it is, whether the user plugged it in, and what it can be told to do
 * about colour and content protection.
 *
 * Everything a display asks a connector is here, under the names it asks
 * them by, so that upstream's code reads unchanged. What is not here is how
 * any of it is found out -- that is the whole of their file and none of this
 * one.
 *
 * Most of the questions are about a settable attribute of the hardware, and
 * every one of those is answered the same way by default: there is no such
 * attribute. That is not a stub. A display's colour space and its content
 * protection are attributes of a digital link to a display that was plugged
 * in, and a panel soldered to a board has no link and was not plugged in.
 * Upstream already reads an absent attribute as "this display cannot do
 * that" and turns the feature off, so answering truthfully is enough -- the
 * code that would have used it simply does not run.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "display/Device.h"
#include "display/DrmColorspace.h"
#include "display/DrmMode.h"
#include "display/DrmProperty.h"
#include "display/DrmUnique.h"
#include "display/PipelineBinding.h"

namespace android::drm_hwcomposer {

/* One attribute that is not there, to hand back where a backend has none.
 *
 * A property cannot be copied, so every such answer has to be a reference to
 * something that outlives the call, and there is no reason for that to be
 * more than one object: they are all equally absent. */
inline const DrmProperty &AbsentProperty() {
  static const DrmProperty kAbsent;
  return kAbsent;
}

class Connector : public PipelineBindable<Connector> {
 public:
  virtual ~Connector() = default;

  virtual uint32_t GetId() const = 0;
  virtual std::string GetName() const = 0;

  /* Which display device this one hangs off, and which of that device's
   * displays it is. Together they make the port a display reports to the
   * framework, which has to stay the same across restarts -- so both are
   * positions rather than identifiers handed out at run time. */
  virtual Device &GetDev() const = 0;
  virtual uint32_t GetIndexInResArray() const = 0;

  /* One of the DRM_MODE_CONNECTOR_* values. It says how a display is
   * attached, which the framework and the logs both want to know. */
  virtual uint32_t GetConnectorType() const = 0;

  virtual bool IsInternal() const = 0;
  virtual bool IsExternal() const = 0;

  /* Whether this display is reached through another one. A stream shared
   * down one cable between several monitors; nothing else. */
  virtual bool IsMst() const {
    return false;
  }

  /* Whether anything is on the other end. A display that is part of the
   * machine is always connected, which is the default. */
  virtual bool IsConnected() const {
    return true;
  }

  /* Whether content protection has been negotiated and is in force. There is
   * nothing to negotiate with over a link that is not a link, so the default
   * is that it is not. */
  virtual bool IsContentProtectionEnabled() const {
    return false;
  }

  virtual const std::vector<DrmMode> &GetModes() const = 0;

  /* Physical size of the visible area in millimetres. Zero where it is not
   * known, which consumers read as "no information". */
  virtual uint32_t GetMmWidth() const = 0;
  virtual uint32_t GetMmHeight() const = 0;

  /* What the display says about itself. Only a display that was plugged in
   * has anything to say, so the default is nothing. */
  virtual DrmModePropertyBlobUnique GetEdidBlob() {
    return {nullptr, [](drmModePropertyBlobRes * /*it*/) {}};
  }

  virtual std::optional<PanelOrientation> GetPanelOrientation() {
    return std::nullopt;
  }

  /* The settable attributes. See the note at the top of the file on why
   * absent is a real answer rather than a missing one. */

  virtual const DrmProperty &GetLinkStatusProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetColorspaceProperty() const {
    return AbsentProperty();
  }

  virtual uint64_t GetColorspacePropertyValue(DrmColorspace /*c*/) {
    return 0;
  }

  virtual const DrmProperty &GetContentTypeProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetContentProtectionProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetMinBpcProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetHdrOutputMetadataProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetHdcpContentTypeProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetWritebackFbIdProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetWritebackOutFenceProperty() const {
    return AbsentProperty();
  }

  virtual const DrmProperty &GetPanelOrientationProperty() const {
    return AbsentProperty();
  }
};

}  // namespace android::drm_hwcomposer
