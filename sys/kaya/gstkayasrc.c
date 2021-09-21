/* GStreamer
 * Copyright (c) 2018 outside US, United States Government, Joshua M. Doe <oss@nvl.army.mil>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstkayasrc
 *
 * The kayasrc element is a source for KAYA framegrabbers.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v kayasrc ! videoconvert ! autovideosink
 * ]|
 * Shows video from the default KAYA framegrabber
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>

#include "gstkayasrc.h"
#include "genicampixelformat.h"

#ifdef HAVE_ORC
#include <orc/orc.h>
#else
#define orc_memcpy memcpy
#endif

GST_DEBUG_CATEGORY_STATIC (gst_kayasrc_debug);
#define GST_CAT_DEFAULT gst_kayasrc_debug

/* prototypes */
static void gst_kayasrc_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_kayasrc_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_kayasrc_dispose (GObject * object);
static void gst_kayasrc_finalize (GObject * object);

static gboolean gst_kayasrc_start (GstBaseSrc * src);
static gboolean gst_kayasrc_stop (GstBaseSrc * src);
static GstCaps *gst_kayasrc_get_caps (GstBaseSrc * src, GstCaps * filter);
static gboolean gst_kayasrc_set_caps (GstBaseSrc * src, GstCaps * caps);
static gboolean gst_kayasrc_unlock (GstBaseSrc * src);
static gboolean gst_kayasrc_unlock_stop (GstBaseSrc * src);

static GstFlowReturn gst_kayasrc_create (GstPushSrc * src, GstBuffer ** buf);

static void gst_kayasrc_stream_buffer_callback (STREAM_BUFFER_HANDLE
    buffer_handle, void *context);
static void gst_kayasrc_stream_callback (void *context,
    STREAM_HANDLE stream_handle);


enum
{
  PROP_0,
  PROP_INTERFACE_INDEX,
  PROP_DEVICE_INDEX,
  PROP_NUM_CAPTURE_BUFFERS,
  PROP_TIMEOUT,
  PROP_PROJECT_FILE,
  PROP_XML_FILE,
  PROP_EXPOSURE_TIME,
  PROP_EXECUTE_COMMAND
};

#define DEFAULT_PROP_INTERFACE_INDEX 0
#define DEFAULT_PROP_DEVICE_INDEX 0
#define DEFAULT_PROP_NUM_CAPTURE_BUFFERS 3
#define DEFAULT_PROP_TIMEOUT 1000
#define DEFAULT_PROP_PROJECT_FILE NULL
#define DEFAULT_PROP_XML_FILE NULL
#define DEFAULT_PROP_EXPOSURE_TIME 0
#define DEFAULT_PROP_EXECUTE_COMMAND NULL

/* pad templates */

static GstStaticPadTemplate gst_kayasrc_src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE
        ("{ GRAY8, GRAY16_LE, GRAY16_BE, BGRA, UYVY }") ";"
        GST_GENICAM_PIXEL_FORMAT_MAKE_BAYER8 ("{ bggr, grbg, rggb, gbrg }") ";"
        GST_GENICAM_PIXEL_FORMAT_MAKE_BAYER16
        ("{ bggr16, grbg16, rggb16, gbrg16 }", "1234")
    )
    );


#if GST_CHECK_VERSION(1,14,0)
static GstStaticCaps unix_reference = GST_STATIC_CAPS ("timestamp/x-unix");
#endif


/* class initialization */

G_DEFINE_TYPE (GstKayaSrc, gst_kayasrc, GST_TYPE_PUSH_SRC);

static void
gst_kayasrc_class_init (GstKayaSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);
  int i;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "kayasrc", 0,
      "Kaya CoaXPress video source");

  gobject_class->set_property = gst_kayasrc_set_property;
  gobject_class->get_property = gst_kayasrc_get_property;
  gobject_class->dispose = gst_kayasrc_dispose;
  gobject_class->finalize = gst_kayasrc_finalize;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_kayasrc_src_template));

  gst_element_class_set_static_metadata (gstelement_class,
      "KAYA Video Source", "Source/Video",
      "KAYA framegrabber video source", "Joshua M. Doe <oss@nvl.army.mil>");

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_kayasrc_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_kayasrc_stop);
  gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_kayasrc_get_caps);
  gstbasesrc_class->set_caps = GST_DEBUG_FUNCPTR (gst_kayasrc_set_caps);
  gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_kayasrc_unlock);
  gstbasesrc_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_kayasrc_unlock_stop);

  gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_kayasrc_create);

  /* Install GObject properties */
  g_object_class_install_property (gobject_class, PROP_INTERFACE_INDEX,
      g_param_spec_uint ("interface-index", "Interface index",
          "Interface index number (zero-based)",
          0, KAYA_SRC_MAX_FG_HANDLES, DEFAULT_PROP_INTERFACE_INDEX,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (gobject_class, PROP_DEVICE_INDEX,
      g_param_spec_uint ("device-index", "Device index",
          "Device index number, zero-based",
          0, G_MAXUINT, DEFAULT_PROP_DEVICE_INDEX,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (gobject_class, PROP_NUM_CAPTURE_BUFFERS,
      g_param_spec_uint ("num-capture-buffers", "Number of capture buffers",
          "Number of capture buffers", 1, G_MAXUINT,
          DEFAULT_PROP_NUM_CAPTURE_BUFFERS,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (klass),
      PROP_TIMEOUT, g_param_spec_int ("timeout",
          "Timeout (ms)",
          "Timeout in ms (0 to use default)", 0, G_MAXINT,
          DEFAULT_PROP_TIMEOUT, G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, PROP_PROJECT_FILE,
      g_param_spec_string ("project-file", "Project file",
          "Filepath of a project file to configure frame grabber",
          DEFAULT_PROP_PROJECT_FILE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (gobject_class, PROP_XML_FILE,
      g_param_spec_string ("xml-file", "XML file",
          "Filepath of a XML file to use with camera, or NULL to use camera's native XML",
          DEFAULT_PROP_XML_FILE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (G_OBJECT_CLASS (klass),
      PROP_EXPOSURE_TIME, g_param_spec_float ("exposure-time",
          "Exposure time (us)",
          "Sets the exposure time in microseconds",
          0, G_MAXFLOAT, DEFAULT_PROP_EXPOSURE_TIME,
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, PROP_EXECUTE_COMMAND,
      g_param_spec_string ("execute-command", "Command to execute",
          "Name of a command to execute after opening the camera (e.g., UserSetLoadReg)",
          DEFAULT_PROP_EXECUTE_COMMAND,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));

  for (i = 0; i < KAYA_SRC_MAX_FG_HANDLES; i++) {
    klass->fg_data[i].fg_handle = INVALID_FGHANDLE;
    klass->fg_data[i].ref_count = 0;
    memset (klass->fg_data[i].cam_handles, 0, sizeof (CAMHANDLE) * 4);
    klass->fg_data[i].num_cams = 0;
    g_mutex_init (&klass->fg_data[i].fg_mutex);
  }
}

static void
gst_kayasrc_cleanup (GstKayaSrc * src)
{
  GST_LOG_OBJECT (src, "cleanup");

  src->frame_size = 0;
  src->frame_count = 0;
  src->dropped_frames = 0;
  src->stop_requested = FALSE;
  src->acquisition_started = FALSE;
  src->kaya_base = GST_CLOCK_TIME_NONE;

  if (src->caps) {
    gst_caps_unref (src->caps);
    src->caps = NULL;
  }

  if (src->stream_handle != INVALID_STREAMHANDLE) {
    KYFG_StreamBufferCallbackUnregister (src->stream_handle,
        gst_kayasrc_stream_buffer_callback);
    // FIXME: we seem to get exceptions later on if we call this
    //KYFG_StreamDelete (src->stream_handle);
    src->stream_handle = INVALID_STREAMHANDLE;
  }

  if (src->cam_handle != INVALID_CAMHANDLE) {
    KYFG_CameraCallbackUnregister (src->cam_handle,
        gst_kayasrc_stream_callback);
    KYFG_CameraStop (src->cam_handle);
    KYFG_CameraClose (src->cam_handle);
    src->cam_handle = INVALID_CAMHANDLE;
  }

  if (src->fg_data) {
    g_mutex_lock (&src->fg_data->fg_mutex);
    GST_DEBUG_OBJECT (src, "Framegrabber open with refcount=%d",
        src->fg_data->ref_count);
    src->fg_data->ref_count--;
    if (src->fg_data->ref_count == 0) {
      GST_DEBUG_OBJECT (src, "Framegrabber ref dropped to 0, closing");
      KYFG_Close (src->fg_data->fg_handle);
      src->fg_data->fg_handle = INVALID_FGHANDLE;
    }
    g_mutex_unlock (&src->fg_data->fg_mutex);
    src->fg_data = NULL;
  }
}

static void
gst_kayasrc_init (GstKayaSrc * src)
{
  /* set source as live (no preroll) */
  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);

  /* override default of BYTES to operate in time mode */
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);

  /* initialize member variables */
  src->interface_index = DEFAULT_PROP_INTERFACE_INDEX;
  src->num_capture_buffers = DEFAULT_PROP_NUM_CAPTURE_BUFFERS;
  src->timeout = DEFAULT_PROP_TIMEOUT;
  src->project_file = DEFAULT_PROP_PROJECT_FILE;
  src->xml_file = DEFAULT_PROP_PROJECT_FILE;
  src->exposure_time = DEFAULT_PROP_EXPOSURE_TIME;
  src->execute_command = DEFAULT_PROP_EXECUTE_COMMAND;

  src->queue = g_async_queue_new ();
  src->caps = NULL;

  src->fg_data = NULL;
  src->cam_handle = INVALID_CAMHANDLE;
  src->stream_handle = INVALID_STREAMHANDLE;
  src->buffer_handles = NULL;

  src->kaya_base = GST_CLOCK_TIME_NONE;
}

static void
gst_kayasrc_get_exposure_time (GstKayaSrc * src)
{
  if (src->cam_handle != INVALID_CAMHANDLE) {
    src->exposure_time =
        KYFG_GetCameraValueFloat (src->cam_handle, "ExposureTime");
  }
}

static void
gst_kayasrc_set_exposure_time (GstKayaSrc * src)
{
  if (src->cam_handle != INVALID_CAMHANDLE && src->exposure_time > 0) {
    FGSTATUS ret = KYFG_SetCameraValueFloat (src->cam_handle, "ExposureTime",
        src->exposure_time);
    if (ret != FGSTATUS_OK) {
      GST_WARNING_OBJECT (src, "Exposure time %.3f invalid",
          src->exposure_time);
    }
  }
}

void
gst_kayasrc_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstKayaSrc *src;

  src = GST_KAYA_SRC (object);

  switch (property_id) {
    case PROP_INTERFACE_INDEX:
      src->interface_index = g_value_get_uint (value);
      break;
    case PROP_DEVICE_INDEX:
      src->device_index = g_value_get_uint (value);
      break;
    case PROP_NUM_CAPTURE_BUFFERS:
      src->num_capture_buffers = g_value_get_uint (value);
      break;
    case PROP_TIMEOUT:
      src->timeout = g_value_get_int (value);
      break;
    case PROP_PROJECT_FILE:
      g_free (src->project_file);
      src->project_file = g_value_dup_string (value);
      break;
    case PROP_XML_FILE:
      g_free (src->xml_file);
      src->xml_file = g_value_dup_string (value);
      break;
    case PROP_EXPOSURE_TIME:
      src->exposure_time = g_value_get_float (value);
      gst_kayasrc_set_exposure_time (src);
      break;
    case PROP_EXECUTE_COMMAND:
      g_free (src->execute_command);
      src->execute_command = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_kayasrc_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstKayaSrc *src;

  g_return_if_fail (GST_IS_KAYA_SRC (object));
  src = GST_KAYA_SRC (object);

  switch (property_id) {
    case PROP_INTERFACE_INDEX:
      g_value_set_uint (value, src->interface_index);
      break;
    case PROP_DEVICE_INDEX:
      g_value_set_uint (value, src->device_index);
      break;
    case PROP_NUM_CAPTURE_BUFFERS:
      g_value_set_uint (value, src->num_capture_buffers);
      break;
    case PROP_TIMEOUT:
      g_value_set_int (value, src->timeout);
      break;
    case PROP_PROJECT_FILE:
      g_value_set_string (value, src->project_file);
      break;
    case PROP_XML_FILE:
      g_value_set_string (value, src->xml_file);
      break;
    case PROP_EXPOSURE_TIME:
      gst_kayasrc_get_exposure_time (src);
      g_value_set_float (value, src->exposure_time);
      break;
    case PROP_EXECUTE_COMMAND:
      g_value_set_string (value, src->execute_command);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_kayasrc_dispose (GObject * object)
{
  GstKayaSrc *src;

  g_return_if_fail (GST_IS_KAYA_SRC (object));
  src = GST_KAYA_SRC (object);

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_kayasrc_parent_class)->dispose (object);
}

void
gst_kayasrc_finalize (GObject * object)
{
  GstKayaSrc *src;

  g_return_if_fail (GST_IS_KAYA_SRC (object));
  src = GST_KAYA_SRC (object);

  /* clean up object here */

  if (src->caps) {
    gst_caps_unref (src->caps);
    src->caps = NULL;
  }

  G_OBJECT_CLASS (gst_kayasrc_parent_class)->finalize (object);
}

static gboolean
gst_kayasrc_start (GstBaseSrc * bsrc)
{
  GstKayaSrc *src = GST_KAYA_SRC (bsrc);
  GstKayaSrcClass *srcclass = GST_KAYA_SRC_GET_CLASS (src);
  FGSTATUS ret;
  uint32_t i, num_ifaces;
  guint32 width, height;
  gchar camera_pixel_format[256], grabber_pixel_format[256];
  guint32 str_size;
  KY_DEVICE_INFO devinfo;
  size_t frame_alignment;
  GstKayaSrcFramegrabber *fg_data;

  GST_DEBUG_OBJECT (src, "start");

  gst_kayasrc_cleanup (src);

  /* find and list all KAYA interfaces */
  num_ifaces = KYFG_Scan (NULL, 0);
  if (num_ifaces == 0) {
    GST_ELEMENT_ERROR (src, LIBRARY, FAILED, ("No KAYA interfaces found"),
        (NULL));
    goto error;
  }
  for (i = 0; i < num_ifaces; ++i) {
    ret = KY_DeviceInfo (i, &devinfo);
    GST_DEBUG_OBJECT (src,
        "Found KAYA interface '%s', index=%d, bus=%d, slot=%d, fcn=%d, PID=%d, isVirtual=%s",
        devinfo.szDeviceDisplayName, i, devinfo.nBus, devinfo.nSlot,
        devinfo.nFunction, devinfo.DevicePID,
        (devinfo.isVirtual ? "Yes" : "No"));
  }

  g_assert (src->interface_index >= 0
      && src->interface_index < KAYA_SRC_MAX_FG_HANDLES);
  if (src->interface_index >= num_ifaces) {
    GST_ELEMENT_ERROR (src, LIBRARY, FAILED,
        ("Interface index provided (%d) out of bounds [0,%d]",
            src->interface_index, num_ifaces - 1), (NULL));
    goto error;
  }

  /* lock mutex until we have a camera opened */
  fg_data = &(srcclass->fg_data[src->interface_index]);
  g_mutex_lock (&fg_data->fg_mutex);

  /* open framegrabber if it isn't already opened */
  if (srcclass->fg_data[src->interface_index].ref_count > 0) {
    GST_DEBUG_OBJECT (src,
        "Framegrabber interface already opened in this process, reusing");
    if (src->project_file && strlen (src->project_file) > 0) {
      GST_ELEMENT_WARNING (src, RESOURCE, SETTINGS,
          ("Project file specified, but framegrabber is already opened, so it won't be used."),
          (NULL));
    }
  } else {
    g_assert (fg_data->ref_count == 0);

    /* project files are optional */
    if (src->project_file && strlen (src->project_file) > 0) {
      if (!g_file_test (src->project_file, G_FILE_TEST_EXISTS)) {
        g_mutex_unlock (&fg_data->fg_mutex);
        GST_ELEMENT_ERROR (src, RESOURCE, NOT_FOUND,
            ("Project file specified does not exist: %s", src->project_file),
            (NULL));
        goto error;
      }

      GST_DEBUG_OBJECT (src,
          "About to open interface at index %d with project file '%s'",
          src->interface_index, src->project_file);
      fg_data->fg_handle =
          KYFG_OpenEx (src->interface_index, src->project_file);
    } else {
      GST_DEBUG_OBJECT (src, "About to open interface at index %d",
          src->interface_index);
      fg_data->fg_handle = KYFG_Open (src->interface_index);
    }

    if (fg_data->fg_handle == INVALID_FGHANDLE) {
      g_mutex_unlock (&fg_data->fg_mutex);
      GST_ELEMENT_ERROR (src, LIBRARY, FAILED,
          ("Failed to open interface at index %d", src->interface_index),
          (NULL));
      goto error;
    }

    /* find and list all cameras */
    ret =
        KYFG_CameraScan (fg_data->fg_handle, fg_data->cam_handles,
        &fg_data->num_cams);
    GST_DEBUG_OBJECT (src, "Found %d cameras connected", fg_data->num_cams);
    if (fg_data->num_cams == 0) {
      g_mutex_unlock (&fg_data->fg_mutex);
      GST_ELEMENT_ERROR (src, LIBRARY, FAILED,
          ("Failed to detect any cameras on interface"), (NULL));
      goto error;
    }
    for (i = 0; i < fg_data->num_cams; ++i) {
      KYFGCAMERA_INFO caminfo;
      ret = KYFG_CameraInfo (fg_data->cam_handles[i], &caminfo);
      GST_DEBUG_OBJECT (src,
          "Found camera '%s', index=%d, %s, %s %s, %s, ver=%s",
          caminfo.deviceUserID, i, caminfo.deviceID, caminfo.deviceVendorName,
          caminfo.deviceModelName, caminfo.deviceManufacturerInfo,
          caminfo.deviceVersion);
    }
  }

  if (src->device_index >= fg_data->num_cams) {
    g_mutex_unlock (&fg_data->fg_mutex);
    GST_ELEMENT_ERROR (src, LIBRARY, FAILED,
        ("Camera device index provided out of bounds"), (NULL));
    goto error;
  }
  GST_DEBUG_OBJECT (src, "About to open camera at index %d", src->device_index);
  if (src->xml_file && strlen (src->xml_file) > 0) {
    if (!g_file_test (src->xml_file, G_FILE_TEST_EXISTS)) {
      g_mutex_unlock (&fg_data->fg_mutex);
      GST_ELEMENT_ERROR (src, RESOURCE, NOT_FOUND,
          ("XML file specified does not exist: %s", src->xml_file), (NULL));
      goto error;
    }
  }
  ret =
      KYFG_CameraOpen2 (fg_data->cam_handles[src->device_index], src->xml_file);
  if (ret != FGSTATUS_OK) {
    g_mutex_unlock (&fg_data->fg_mutex);
    GST_ELEMENT_ERROR (src, LIBRARY, FAILED,
        ("Failed to open camera at index %d", src->device_index), (NULL));
    goto error;
  }
  src->cam_handle = fg_data->cam_handles[src->device_index];

  /* increase refcount since we now have a camera open */
  src->fg_data = fg_data;
  src->fg_data->ref_count++;
  g_mutex_unlock (&src->fg_data->fg_mutex);

  /* if execute-command property is set, run it now */
  if (src->execute_command && src->execute_command[0] != 0) {
    KYFG_CameraExecuteCommand (src->cam_handle, src->execute_command);
  }

  gst_kayasrc_set_exposure_time (src);

  ret =
      KYFG_CameraCallbackRegister (src->cam_handle, gst_kayasrc_stream_callback,
      src);
  if (ret != FGSTATUS_OK) {
    GST_ELEMENT_ERROR (src, LIBRARY, FAILED,
        ("Failed to register stream callback function"), (NULL));
    goto error;
  }

  ret = KYFG_StreamCreate (src->cam_handle, &src->stream_handle, 0);
  if (ret != FGSTATUS_OK || src->stream_handle == INVALID_STREAMHANDLE) {
    GST_ELEMENT_ERROR (src, LIBRARY, FAILED,
        ("Failed to create stream"), (NULL));
    goto error;
  }

  ret =
      KYFG_StreamBufferCallbackRegister (src->stream_handle,
      gst_kayasrc_stream_buffer_callback, src);
  if (ret != FGSTATUS_OK) {
    GST_ELEMENT_ERROR (src, LIBRARY, FAILED,
        ("Failed to register stream buffer callback function"), (NULL));
    goto error;
  }

  KYFG_StreamGetInfo (src->stream_handle, KY_STREAM_INFO_PAYLOAD_SIZE,
      &src->frame_size, NULL, NULL);
  KYFG_StreamGetInfo (src->stream_handle, KY_STREAM_INFO_BUF_ALIGNMENT,
      &frame_alignment, NULL, NULL);

  if (src->buffer_handles) {
    g_free (src->buffer_handles);
  }
  src->buffer_handles = g_new (STREAM_BUFFER_HANDLE, src->num_capture_buffers);
  for (i = 0; i < src->num_capture_buffers; ++i) {
    void *frame = _aligned_malloc (src->frame_size, frame_alignment);
    ret =
        KYFG_BufferAnnounce (src->stream_handle, frame, src->frame_size, NULL,
        &src->buffer_handles[i]);
  }

  ret =
      KYFG_BufferQueueAll (src->stream_handle, KY_ACQ_QUEUE_UNQUEUED,
      KY_ACQ_QUEUE_INPUT);

  width = (guint32) KYFG_GetCameraValueInt (src->cam_handle, "Width");
  height = (guint32) KYFG_GetCameraValueInt (src->cam_handle, "Height");
  GST_DEBUG_OBJECT (src, "Camera resolution is %dx%d", width, height);

  str_size = sizeof (grabber_pixel_format);
  ret = KYFG_GetGrabberValueStringCopy (src->cam_handle, "PixelFormat",
      grabber_pixel_format, &str_size);
  str_size = sizeof (camera_pixel_format);
  ret = KYFG_GetCameraValueStringCopy (src->cam_handle, "PixelFormat",
      camera_pixel_format, &str_size);
  GST_DEBUG_OBJECT (src, "PixelFormat of camera is %s, grabber is %s",
      camera_pixel_format, grabber_pixel_format);

  /* check if PixelFormat is Normal, in which case we use camera value */
  if (g_strcmp0 (grabber_pixel_format, "Normal") == 0) {
    g_strlcpy (grabber_pixel_format, camera_pixel_format,
        sizeof (grabber_pixel_format));
  }

  /* create caps */
  if (src->caps) {
    gst_caps_unref (src->caps);
    src->caps = NULL;
  }

  src->caps =
      gst_genicam_pixel_format_caps_from_pixel_format (grabber_pixel_format,
      G_BYTE_ORDER, width, height, 30, 1, 1, 1);

  if (src->caps == NULL) {
    GST_ELEMENT_ERROR (src, STREAM, WRONG_TYPE,
        ("Unknown or unsupported pixel format '%s'.", grabber_pixel_format),
        (NULL));
    return FALSE;
  }

  src->stop_requested = FALSE;
  src->dropped_frames = 0;

  return TRUE;

error:
  gst_kayasrc_cleanup (src);

  return FALSE;
}

static gboolean
gst_kayasrc_stop (GstBaseSrc * bsrc)
{
  GstKayaSrc *src = GST_KAYA_SRC (bsrc);

  GST_DEBUG_OBJECT (src, "stop");

  gst_kayasrc_cleanup (src);

  return TRUE;
}

static GstCaps *
gst_kayasrc_get_caps (GstBaseSrc * bsrc, GstCaps * filter)
{
  GstKayaSrc *src = GST_KAYA_SRC (bsrc);
  GstCaps *caps;

  if (src->stream_handle == INVALID_STREAMHANDLE) {
    caps = gst_pad_get_pad_template_caps (GST_BASE_SRC_PAD (src));
  } else {
    caps = gst_caps_copy (src->caps);
  }

  GST_DEBUG_OBJECT (src, "The caps before filtering are %" GST_PTR_FORMAT,
      caps);

  if (filter && caps) {
    GstCaps *tmp = gst_caps_intersect (caps, filter);
    gst_caps_unref (caps);
    caps = tmp;
  }

  GST_DEBUG_OBJECT (src, "The caps after filtering are %" GST_PTR_FORMAT, caps);

  return caps;
}

static gboolean
gst_kayasrc_set_caps (GstBaseSrc * bsrc, GstCaps * caps)
{
  GstKayaSrc *src = GST_KAYA_SRC (bsrc);
  GstVideoInfo vinfo;
  GstStructure *s = gst_caps_get_structure (caps, 0);

  GST_DEBUG_OBJECT (src, "The caps being set are %" GST_PTR_FORMAT, caps);

  gst_video_info_from_caps (&vinfo, caps);

  if (GST_VIDEO_INFO_FORMAT (&vinfo) == GST_VIDEO_FORMAT_UNKNOWN) {
    goto unsupported_caps;
  }

  return TRUE;

unsupported_caps:
  GST_ERROR_OBJECT (src, "Unsupported caps: %" GST_PTR_FORMAT, caps);
  return FALSE;
}

static gboolean
gst_kayasrc_unlock (GstBaseSrc * bsrc)
{
  GstKayaSrc *src = GST_KAYA_SRC (bsrc);

  GST_LOG_OBJECT (src, "unlock");

  src->stop_requested = TRUE;

  return TRUE;
}

static gboolean
gst_kayasrc_unlock_stop (GstBaseSrc * bsrc)
{
  GstKayaSrc *src = GST_KAYA_SRC (bsrc);

  GST_LOG_OBJECT (src, "unlock_stop");

  src->stop_requested = FALSE;

  return TRUE;
}

typedef struct
{
  GstKayaSrc *src;
  STREAM_BUFFER_HANDLE buf_handle;
  guint32 buf_id;
} VideoFrame;

static void
buffer_release (void *data)
{
  VideoFrame *frame = (VideoFrame *) data;
  GST_TRACE_OBJECT (frame->src, "Releasing buffer id=%d", frame->buf_id);
  KYFG_BufferToQueue (frame->buf_handle, KY_ACQ_QUEUE_INPUT);
  g_free (frame);
}

static void
gst_kayasrc_stream_buffer_callback (STREAM_BUFFER_HANDLE buffer_handle,
    void *context)
{
  GstKayaSrc *src = GST_KAYA_SRC (context);
  GstBuffer *buf;
  unsigned char *data;
  guint32 buf_id;
  GstClockTime timestamp;
  VideoFrame *vf;
  GstClock *clock;

  KYFG_BufferGetInfo (buffer_handle, KY_STREAM_BUFFER_INFO_TIMESTAMP,
      &timestamp, NULL, NULL);
  KYFG_BufferGetInfo (buffer_handle, KY_STREAM_BUFFER_INFO_BASE, &data, NULL,
      NULL);
  KYFG_BufferGetInfo (buffer_handle, KY_STREAM_BUFFER_INFO_ID, &buf_id, NULL,
      NULL);

  GST_TRACE_OBJECT (src, "Got buffer id=%d, total_num=%d", buf_id,
      src->frame_count);

  vf = g_new0 (VideoFrame, 1);
  vf->src = src;
  vf->buf_handle = buffer_handle;
  vf->buf_id = buf_id;
  buf =
      gst_buffer_new_wrapped_full ((GstMemoryFlags) GST_MEMORY_FLAG_NO_SHARE,
      (gpointer) data, src->frame_size, 0, src->frame_size, vf,
      (GDestroyNotify) buffer_release);

  GST_BUFFER_OFFSET (buf) = src->frame_count;
  src->frame_count++;

  //if (src->kaya_base == GST_CLOCK_TIME_NONE) {
  // assume delay between these two calls is negligible
  src->kaya_base = KYFG_GetGrabberValueInt (src->cam_handle, "Timestamp");
  src->unix_base = g_get_real_time () * 1000;
  //}
#if GST_CHECK_VERSION(1,14,0)
  {
    GstClockTime unix_ts = src->unix_base + (timestamp - src->kaya_base);
    gst_buffer_add_reference_timestamp_meta (buf,
        gst_static_caps_get (&unix_reference), unix_ts, GST_CLOCK_TIME_NONE);
    GST_LOG_OBJECT (src, "Buffer #%d, adding unix timestamp: %llu",
        GST_BUFFER_OFFSET (buf), unix_ts);
    /*{
       GDateTime *frame_time, *tmpdt;
       tmpdt = g_date_time_new_from_unix_utc (0);
       frame_time = g_date_time_add_seconds(tmpdt, (gdouble)unix_ts / GST_SECOND);
       g_date_time_unref (tmpdt);
       GST_LOG ("Unix timestamp added is: %s.%d", g_date_time_format (frame_time, "%Y-%m-%d %H:%M:%S"), g_date_time_get_microsecond(frame_time));
       g_date_time_unref (frame_time);
       } */
  }
#endif

  clock = gst_element_get_clock (GST_ELEMENT (src));
  GST_BUFFER_TIMESTAMP (buf) =
      GST_CLOCK_DIFF (gst_element_get_base_time (GST_ELEMENT (src)),
      gst_clock_get_time (clock));
  gst_object_unref (clock);

  g_async_queue_push (src->queue, buf);
}

static void
gst_kayasrc_stream_callback (void *context, STREAM_HANDLE stream_handle)
{
  GstKayaSrc *src = GST_KAYA_SRC (context);
}

static GstFlowReturn
gst_kayasrc_create (GstPushSrc * psrc, GstBuffer ** buf)
{
  GstKayaSrc *src = GST_KAYA_SRC (psrc);
  gint64 dropped_frames = 0;
  static FILE *temperature_file = NULL;
  static gint64 temp_log_last_time = 0;
  GST_LOG_OBJECT (src, "create");

  if (g_getenv ("GST_KAYA_FPGA_TEMP_LOG")) {
    if (temperature_file == NULL) {
      const char *envvar = g_getenv ("GST_KAYA_FPGA_TEMP_LOG");
      gboolean write_header;
      gchar *log_filename;
      if (atoi (envvar) == 1) {
        GDateTime *dt = g_date_time_new_now_local ();
        log_filename =
            g_date_time_format (dt, "kaya_fgpa_temp_%Y%m%d_%H%M%S.csv");
        g_date_time_unref (dt);
      } else {
        log_filename = g_strdup (envvar);
      }

      write_header = !g_file_test (log_filename, G_FILE_TEST_EXISTS);

      GST_DEBUG_OBJECT (src, "Opening FPGA temp log file (%s)", log_filename);
      temperature_file = fopen (log_filename, "a");
      if (!temperature_file) {
        GST_ERROR_OBJECT (src, "Failed to open log file");
        return GST_FLOW_ERROR;
      }
      g_free (log_filename);

      if (write_header) {
        fprintf (temperature_file, "IsoTime, UnixTime, KayaFpgaTemp\n");
      }
    }

    if (temperature_file && g_get_real_time () - temp_log_last_time >= 1000000) {
      GDateTime *dt = g_date_time_new_now_local ();
      gchar *time_str = g_date_time_format (dt, "%Y-%m-%dT%H:%M:%S, %s");
      int fg_temp = KYFG_GetGrabberValueInt (src->fg_data->fg_handle,
          "DeviceTemperature");
      GST_DEBUG_OBJECT (src, "FPGA temp: %d", fg_temp);
      fprintf (temperature_file, "%s, %d\n", time_str, fg_temp);
      fflush (temperature_file);
      g_date_time_unref (dt);
      g_free (time_str);
      temp_log_last_time = g_get_real_time ();
    }
  }

  if (!src->acquisition_started) {
    FGSTATUS ret;
    GST_DEBUG_OBJECT (src, "starting acquisition");
    ret = KYFG_CameraStart (src->cam_handle, src->stream_handle, 0);
    if (ret != FGSTATUS_OK) {
      GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
          ("Failed to start camera acquisition"), (NULL));
      goto error;
    }
    src->acquisition_started = TRUE;
  }

  *buf =
      GST_BUFFER (g_async_queue_timeout_pop (src->queue, src->timeout * 1000));
  if (!*buf) {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
        ("Failed to get buffer in %d ms", src->timeout), (NULL));
    goto error;
  }

  dropped_frames =
      KYFG_GetGrabberValueInt (src->cam_handle, "DropFrameCounter");
  if (dropped_frames > src->dropped_frames) {
    GstStructure *info_msg;
    gint64 just_dropped = dropped_frames - src->dropped_frames;
    src->dropped_frames = dropped_frames;

    GST_WARNING_OBJECT (src, "Just dropped %d frames (%d total)", just_dropped,
        src->dropped_frames);

    info_msg = gst_structure_new ("dropped-frame-info",
        "num-dropped-frames", G_TYPE_INT, just_dropped,
        "total-dropped-frames", G_TYPE_INT, src->dropped_frames,
        "timestamp", GST_TYPE_CLOCK_TIME, GST_BUFFER_TIMESTAMP (buf), NULL);
    gst_element_post_message (GST_ELEMENT (src),
        gst_message_new_element (GST_OBJECT (src), info_msg));
    src->dropped_frames = dropped_frames;
  }

  if (src->stop_requested) {
    if (*buf != NULL) {
      gst_buffer_unref (*buf);
      *buf = NULL;
    }
    return GST_FLOW_FLUSHING;
  }

  return GST_FLOW_OK;

error:
  return GST_FLOW_ERROR;
}
