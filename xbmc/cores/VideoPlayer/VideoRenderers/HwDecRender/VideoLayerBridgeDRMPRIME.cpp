/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoLayerBridgeDRMPRIME.h"

#include "cores/VideoPlayer/Buffers/VideoBufferDRMPRIME.h"
#include "utils/EDIDUtils.h"
#include "utils/log.h"
#include "windowing/gbm/drm/DRMAtomic.h"

#include <utility>

using namespace KODI::WINDOWING::GBM;
using namespace DRMPRIME;

CVideoLayerBridgeDRMPRIME::CVideoLayerBridgeDRMPRIME(std::shared_ptr<CDRMAtomic> drm)
  : m_DRM(std::move(drm))
{
}

CVideoLayerBridgeDRMPRIME::~CVideoLayerBridgeDRMPRIME()
{
  Release(m_prev_buffer);
  Release(m_buffer);
}

void CVideoLayerBridgeDRMPRIME::Disable()
{
  // disable video plane
  auto plane = m_DRM->GetVideoPlane();
  if (plane)
  {
    m_DRM->AddProperty(plane, "FB_ID", 0);
    m_DRM->AddProperty(plane, "CRTC_ID", 0);
  }

  auto connector = m_DRM->GetConnector();

  bool result;
  uint64_t value;
  std::tie(result, value) = connector->GetPropertyValue("Colorspace", "Default");
  if (result)
  {
    CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - setting connector colorspace to Default", __FUNCTION__);
    m_DRM->AddProperty(connector, "Colorspace", value);
  }

  // disable HDR metadata
  if (connector->SupportsProperty("HDR_OUTPUT_METADATA"))
  {
    m_DRM->AddProperty(connector, "HDR_OUTPUT_METADATA", 0);
    m_DRM->SetActive(true);

    if (m_hdr_blob_id)
      drmModeDestroyPropertyBlob(m_DRM->GetFileDescriptor(), m_hdr_blob_id);
    m_hdr_blob_id = 0;
  }
}

void CVideoLayerBridgeDRMPRIME::Acquire(CVideoBufferDRMPRIME* buffer)
{
  // release the buffer that is no longer presented on screen
  Release(m_prev_buffer);

  // release the buffer currently being presented next call
  m_prev_buffer = m_buffer;

  // reference count the buffer that is going to be presented on screen
  m_buffer = buffer;
  m_buffer->Acquire();
}

void CVideoLayerBridgeDRMPRIME::Release(CVideoBufferDRMPRIME* buffer)
{
  if (!buffer)
    return;

  Unmap(buffer);
  buffer->Release();
}

bool CVideoLayerBridgeDRMPRIME::Map(CVideoBufferDRMPRIME* buffer)
{
  if (buffer->m_fb_id)
    return true;

  if (!buffer->AcquireDescriptor())
  {
    CLog::Log(LOGERROR, "CVideoLayerBridgeDRMPRIME::{} - failed to acquire descriptor",
              __FUNCTION__);
    return false;
  }

  AVDRMFrameDescriptor* descriptor = buffer->GetDescriptor();
  uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0}, flags = 0;
  uint64_t modifier[4] = {0};
  int ret;

  // convert Prime FD to GEM handle
  for (int object = 0; object < descriptor->nb_objects; object++)
  {
    ret = drmPrimeFDToHandle(m_DRM->GetFileDescriptor(), descriptor->objects[object].fd,
                             &buffer->m_handles[object]);
    if (ret < 0)
    {
      CLog::Log(LOGERROR,
                "CVideoLayerBridgeDRMPRIME::{} - failed to convert prime fd {} to gem handle {}, "
                "ret = {}",
                __FUNCTION__, descriptor->objects[object].fd, buffer->m_handles[object], ret);
      return false;
    }
  }

  for (int layer = 0; layer < descriptor->nb_layers; layer++)
  {
    AVDRMLayerDescriptor* layerDesc = &descriptor->layers[layer];
    for (int plane = 0; plane < layerDesc->nb_planes; plane++)
    {
      AVDRMPlaneDescriptor* planeDesc = &layerDesc->planes[plane];
      AVDRMObjectDescriptor* objectDesc = &descriptor->objects[planeDesc->object_index];

      // fix index for cases where there are multiple layers but one plane
      if (descriptor->nb_layers > 1 && layerDesc->nb_planes == 1)
        plane = layer;

      handles[plane] = buffer->m_handles[planeDesc->object_index];
      pitches[plane] = planeDesc->pitch;
      offsets[plane] = planeDesc->offset;
      modifier[plane] = objectDesc->format_modifier;
    }
  }

  uint32_t format = descriptor->layers[0].format;

  if (descriptor->nb_layers == 2)
  {
    if (descriptor->layers[0].format == DRM_FORMAT_R8 &&
        descriptor->layers[1].format == DRM_FORMAT_GR88)
      format = DRM_FORMAT_NV12;

    if (descriptor->layers[0].format == DRM_FORMAT_R16 &&
        descriptor->layers[1].format == DRM_FORMAT_GR1616)
      format = DRM_FORMAT_P010;
  }

  if (descriptor->nb_layers == 3)
  {
    if (descriptor->layers[0].format == DRM_FORMAT_R8 &&
        descriptor->layers[1].format == DRM_FORMAT_R8 &&
        descriptor->layers[2].format == DRM_FORMAT_R8)
      format = DRM_FORMAT_YUV420;

    // YUV420P10 isn't supported by any hardware that I've seen
  }

  if (modifier[0] && modifier[0] != DRM_FORMAT_MOD_INVALID)
    flags = DRM_MODE_FB_MODIFIERS;

  // add the video frame FB
  ret = drmModeAddFB2WithModifiers(m_DRM->GetFileDescriptor(), buffer->GetWidth(),
                                   buffer->GetHeight(), format, handles, pitches, offsets, modifier,
                                   &buffer->m_fb_id, flags);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CVideoLayerBridgeDRMPRIME::{} - failed to add fb {}, ret = {}",
              __FUNCTION__, buffer->m_fb_id, ret);
    return false;
  }

  Acquire(buffer);
  return true;
}

void CVideoLayerBridgeDRMPRIME::Unmap(CVideoBufferDRMPRIME* buffer)
{
  if (buffer->m_fb_id)
  {
    drmModeRmFB(m_DRM->GetFileDescriptor(), buffer->m_fb_id);
    buffer->m_fb_id = 0;
  }

  for (int i = 0; i < AV_DRM_MAX_PLANES; i++)
  {
    if (buffer->m_handles[i])
    {
      struct drm_gem_close gem_close = {.handle = buffer->m_handles[i]};
      drmIoctl(m_DRM->GetFileDescriptor(), DRM_IOCTL_GEM_CLOSE, &gem_close);
      buffer->m_handles[i] = 0;
    }
  }

  buffer->ReleaseDescriptor();
}

void CVideoLayerBridgeDRMPRIME::Configure(CVideoBufferDRMPRIME* buffer)
{
  const VideoPicture& picture = buffer->GetPicture();

  auto plane = m_DRM->GetVideoPlane();
  if (!plane)
    plane = m_DRM->GetGuiPlane();

  bool result;
  uint64_t value;
  std::tie(result, value) = plane->GetPropertyValue("COLOR_ENCODING", GetColorEncoding(picture));
  if (result)
    m_DRM->AddProperty(plane, "COLOR_ENCODING", value);

  std::tie(result, value) = plane->GetPropertyValue("COLOR_RANGE", GetColorRange(picture));
  if (result)
    m_DRM->AddProperty(plane, "COLOR_RANGE", value);

  auto connector = m_DRM->GetConnector();

  std::vector<uint8_t> raw;
  std::tie(result, raw) = connector->GetEDID();

  KODI::UTILS::CEDIDUtils edid;
  if (result)
    edid.SetEDID(raw);

  std::tie(result, value) =  connector->GetPropertyValue("Colorspace", GetColorimetry(picture));
  if (result)
  {
    if (edid.SupportsColorimetry(GetColorimetry(picture)))
    {
      CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - setting connector colorspace to {}", __FUNCTION__,
              GetColorimetry(picture));
      m_DRM->AddProperty(connector, "Colorspace", value);
      m_DRM->SetActive(true);
    }
  }

  if (connector->SupportsProperty("HDR_OUTPUT_METADATA"))
  {
    m_hdr_metadata.metadata_type = HDMI_STATIC_METADATA_TYPE1;

    m_hdr_metadata.hdmi_metadata_type1.metadata_type = HDMI_STATIC_METADATA_TYPE1;

    uint8_t eotf = GetEOTF(picture);

    if (edid.SupportsEOTF(eotf))
    {
      m_hdr_metadata.hdmi_metadata_type1.eotf = eotf;

      if (m_hdr_blob_id)
        drmModeDestroyPropertyBlob(m_DRM->GetFileDescriptor(), m_hdr_blob_id);
      m_hdr_blob_id = 0;

      const AVMasteringDisplayMetadata* mdmd = GetMasteringDisplayMetadata(picture);
      if (mdmd && mdmd->has_primaries)
      {
        // Convert to unsigned 16-bit values in units of 0.00002,
        // where 0x0000 represents zero and 0xC350 represents 1.0000
        for (int i = 0; i < 3; i++)
        {
          m_hdr_metadata.hdmi_metadata_type1.display_primaries[i].x =
              std::round(av_q2d(mdmd->display_primaries[i][0]) * 50000.0);
          m_hdr_metadata.hdmi_metadata_type1.display_primaries[i].y =
              std::round(av_q2d(mdmd->display_primaries[i][1]) * 50000.0);

          CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - display_primaries[{}].x: {}",
                    __FUNCTION__, i, m_hdr_metadata.hdmi_metadata_type1.display_primaries[i].x);
          CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - display_primaries[{}].y: {}",
                    __FUNCTION__, i, m_hdr_metadata.hdmi_metadata_type1.display_primaries[i].y);
        }

        m_hdr_metadata.hdmi_metadata_type1.white_point.x =
            std::round(av_q2d(mdmd->white_point[0]) * 50000.0);
        m_hdr_metadata.hdmi_metadata_type1.white_point.y =
            std::round(av_q2d(mdmd->white_point[1]) * 50000.0);

        CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - white_point.x: {}", __FUNCTION__,
                  m_hdr_metadata.hdmi_metadata_type1.white_point.x);
        CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - white_point.y: {}", __FUNCTION__,
                  m_hdr_metadata.hdmi_metadata_type1.white_point.y);
      }

      if (mdmd && mdmd->has_luminance)
      {
        // Convert to unsigned 16-bit value in units of 1 cd/m2,
        // where 0x0001 represents 1 cd/m2 and 0xFFFF represents 65535 cd/m2
        m_hdr_metadata.hdmi_metadata_type1.max_display_mastering_luminance =
            std::round(av_q2d(mdmd->max_luminance));

        // Convert to unsigned 16-bit value in units of 0.0001 cd/m2,
        // where 0x0001 represents 0.0001 cd/m2 and 0xFFFF represents 6.5535 cd/m2
        m_hdr_metadata.hdmi_metadata_type1.min_display_mastering_luminance =
            std::round(av_q2d(mdmd->min_luminance) * 10000.0);

        CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - max_display_mastering_luminance: {}",
                  __FUNCTION__, m_hdr_metadata.hdmi_metadata_type1.max_display_mastering_luminance);
        CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - min_display_mastering_luminance: {}",
                  __FUNCTION__, m_hdr_metadata.hdmi_metadata_type1.min_display_mastering_luminance);
      }

      const AVContentLightMetadata* clmd = GetContentLightMetadata(picture);
      if (clmd)
      {
        m_hdr_metadata.hdmi_metadata_type1.max_cll = clmd->MaxCLL;
        m_hdr_metadata.hdmi_metadata_type1.max_fall = clmd->MaxFALL;

        CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - max_cll: {}", __FUNCTION__,
                  m_hdr_metadata.hdmi_metadata_type1.max_cll);
        CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - max_fall: {}", __FUNCTION__,
                  m_hdr_metadata.hdmi_metadata_type1.max_fall);
      }

      drmModeCreatePropertyBlob(m_DRM->GetFileDescriptor(), &m_hdr_metadata, sizeof(m_hdr_metadata),
                                &m_hdr_blob_id);


      m_DRM->AddProperty(connector, "HDR_OUTPUT_METADATA", m_hdr_blob_id);
    }
  }

  m_DRM->SetActive(true);
}

void CVideoLayerBridgeDRMPRIME::SetVideoPlane(CVideoBufferDRMPRIME* buffer, const CRect& destRect)
{
  if (!Map(buffer))
  {
    Unmap(buffer);
    return;
  }

  auto plane = m_DRM->GetVideoPlane();
  m_DRM->AddProperty(plane, "FB_ID", buffer->m_fb_id);
  m_DRM->AddProperty(plane, "CRTC_ID", m_DRM->GetCrtc()->GetCrtcId());
  m_DRM->AddProperty(plane, "SRC_X", 0);
  m_DRM->AddProperty(plane, "SRC_Y", 0);
  m_DRM->AddProperty(plane, "SRC_W", buffer->GetWidth() << 16);
  m_DRM->AddProperty(plane, "SRC_H", buffer->GetHeight() << 16);
  m_DRM->AddProperty(plane, "CRTC_X", static_cast<int32_t>(destRect.x1) & ~1);
  m_DRM->AddProperty(plane, "CRTC_Y", static_cast<int32_t>(destRect.y1) & ~1);
  m_DRM->AddProperty(plane, "CRTC_W", (static_cast<uint32_t>(destRect.Width()) + 1) & ~1);
  m_DRM->AddProperty(plane, "CRTC_H", (static_cast<uint32_t>(destRect.Height()) + 1) & ~1);
}

void CVideoLayerBridgeDRMPRIME::UpdateVideoPlane()
{
  if (!m_buffer || !m_buffer->m_fb_id)
    return;

  // release the buffer that is no longer presented on screen
  Release(m_prev_buffer);
  m_prev_buffer = nullptr;

  auto plane = m_DRM->GetVideoPlane();
  m_DRM->AddProperty(plane, "FB_ID", m_buffer->m_fb_id);
  m_DRM->AddProperty(plane, "CRTC_ID", m_DRM->GetCrtc()->GetCrtcId());
}
