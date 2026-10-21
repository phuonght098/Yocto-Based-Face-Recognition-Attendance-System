#define _GNU_SOURCE
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_PATH "/etc/wifi-camera.conf"
#define FRAME_TIMEOUT_NS (5ULL * GST_SECOND)
#define SAMPLE_WAIT_NS (500 * GST_MSECOND)
#define RETRY_DELAY_SEC 3

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = '\0';
    return s;
}

static char *load_rtsp_url(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open config file %s\n", path);
        return NULL;
    }

    char *line = NULL;
    size_t cap = 0;
    char *url = NULL;

    while (getline(&line, &cap, f) >= 0) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;
        if (strncmp(p, "RTSP_URL=", 9) == 0) {
            p = trim(p + 9);
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                char *q = strrchr(p, quote);
                if (q) *q = '\0';
            }
            url = strdup(p);
            break;
        }
    }

    free(line);
    fclose(f);

    if (!url || strncmp(url, "rtsp://", 7) != 0) {
        fprintf(stderr, "ERROR: RTSP_URL missing or not an rtsp:// URL in %s\n", path);
        free(url);
        return NULL;
    }
    return url;
}

static void on_rtsp_pad_added(GstElement *element, GstPad *new_pad, gpointer data)
{
    (void)element;
    GstElement *depay_element = GST_ELEMENT(data);
    GstPad *sink_pad = gst_element_get_static_pad(depay_element, "sink");
    if (gst_pad_is_linked(sink_pad)) {
        gst_object_unref(sink_pad);
        return;
    }

    GstCaps *caps = gst_pad_get_current_caps(new_pad);
    if (!caps) caps = gst_pad_query_caps(new_pad, NULL);
    gboolean is_h264_video = FALSE;
    if (caps && gst_caps_get_size(caps) > 0) {
        const GstStructure *st = gst_caps_get_structure(caps, 0);
        const gchar *media = gst_structure_get_string(st, "media");
        const gchar *encoding = gst_structure_get_string(st, "encoding-name");
        is_h264_video = media && encoding && g_str_equal(media, "video") &&
                        g_ascii_strcasecmp(encoding, "H264") == 0;
    }
    if (is_h264_video) {
        GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
        if (ret != GST_PAD_LINK_OK)
            g_printerr("ERROR: cannot link RTSP H.264 pad (code %d)\\n", ret);
    }
    if (caps) gst_caps_unref(caps);
    gst_object_unref(sink_pad);
}

static void on_decoder_pad_added(GstElement *element, GstPad *new_pad, gpointer data)
{
    (void)element;
    GstElement *convert_element = GST_ELEMENT(data);
    GstPad *sink_pad = gst_element_get_static_pad(convert_element, "sink");
    if (!gst_pad_is_linked(sink_pad)) {
        GstCaps *caps = gst_pad_get_current_caps(new_pad);
        if (!caps) caps = gst_pad_query_caps(new_pad, NULL);
        gboolean is_video = FALSE;
        if (caps && gst_caps_get_size(caps) > 0) {
            const GstStructure *st = gst_caps_get_structure(caps, 0);
            is_video = g_str_has_prefix(gst_structure_get_name(st), "video/");
        }
        if (is_video) {
            GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
            if (ret != GST_PAD_LINK_OK)
                g_printerr("ERROR: cannot link decoded video pad (code %d)\\n", ret);
        }
        if (caps) gst_caps_unref(caps);
    }
    gst_object_unref(sink_pad);
}

static gboolean run_one_session(const char *url)
{
    GstElement *pipeline = NULL, *src = NULL, *depay = NULL, *parse = NULL;
    GstElement *decoder = NULL, *convert = NULL, *capsfilter = NULL, *sink = NULL;
    GstBus *bus = NULL;
    gboolean ok = FALSE;
    guint64 frames = 0;
    guint64 last_frame_ns = gst_util_get_timestamp();
    guint64 last_stats_ns = last_frame_ns;
    GstClockTime last_pts = GST_CLOCK_TIME_NONE;

    pipeline = gst_pipeline_new("wifi-camera-pipeline");
    src = gst_element_factory_make("rtspsrc", "source");
    depay = gst_element_factory_make("rtph264depay", "depayloader");
    parse = gst_element_factory_make("h264parse", "h264-parser");
    decoder = gst_element_factory_make("decodebin", "decoder");
    convert = gst_element_factory_make("videoconvert", "converter");
    capsfilter = gst_element_factory_make("capsfilter", "rgb-caps");
    sink = gst_element_factory_make("appsink", "frame-sink");

    if (!pipeline || !src || !depay || !parse || !decoder || !convert ||
        !capsfilter || !sink) {
        fprintf(stderr, "ERROR: required GStreamer element unavailable. "
                        "Check RTSP, RTP/H.264, h264parse, decoder, videoconvert and appsink plugins.\n");
        goto cleanup;
    }

    g_object_set(src,
                 "location", url,
                 "latency", 200,
                 "protocols", GST_RTSP_LOWER_TRANS_TCP,
                 "timeout", (guint64)5000000,
                 "tcp-timeout", (guint64)5000000,
                 NULL);

    GstCaps *rgb_caps = gst_caps_from_string("video/x-raw,format=RGB");
    g_object_set(capsfilter, "caps", rgb_caps, NULL);
    gst_caps_unref(rgb_caps);

    g_object_set(sink,
                 "emit-signals", FALSE,
                 "sync", FALSE,
                 "max-buffers", 1,
                 "drop", TRUE,
                 "wait-on-eos", FALSE,
                 NULL);

    gst_bin_add_many(GST_BIN(pipeline), src, depay, parse, decoder,
                     convert, capsfilter, sink, NULL);

    if (!gst_element_link_many(depay, parse, decoder, NULL)) {
        fprintf(stderr, "ERROR: cannot link depayloader/parser/decoder\n");
        goto cleanup;
    }
    if (!gst_element_link_many(convert, capsfilter, sink, NULL)) {
        fprintf(stderr, "ERROR: cannot link converter/capsfilter/appsink\n");
        goto cleanup;
    }

    /* rtspsrc exposes RTP pads dynamically; link only H.264 video. */
    g_signal_connect(src, "pad-added", G_CALLBACK(on_rtsp_pad_added), depay);
    /* decodebin exposes decoded video pads dynamically. */
    g_signal_connect(decoder, "pad-added", G_CALLBACK(on_decoder_pad_added), convert);

    bus = gst_element_get_bus(pipeline);
    fprintf(stdout, "INFO: starting RTSP session (credentials hidden)\n");
    GstStateChangeReturn state_ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "ERROR: pipeline could not enter PLAYING state\n");
        goto cleanup;
    }

    while (!stop_requested) {
        GstMessage *msg = gst_bus_pop_filtered(
            bus, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (msg) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError *err = NULL;
                gchar *debug = NULL;
                gst_message_parse_error(msg, &err, &debug);
                fprintf(stderr, "ERROR: GStreamer: %s\n",
                        err ? err->message : "unknown error");
                if (debug) fprintf(stderr, "DEBUG: %s\n", debug);
                if (err) g_error_free(err);
                g_free(debug);
            } else {
                fprintf(stderr, "WARN: video stream reached EOS\n");
            }
            gst_message_unref(msg);
            break;
        }

        GstSample *sample = gst_app_sink_try_pull_sample(
            GST_APP_SINK(sink), SAMPLE_WAIT_NS);
        if (sample) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstCaps *caps = gst_sample_get_caps(sample);
            GstVideoInfo info;
            gst_video_info_init(&info);
            if (caps && gst_video_info_from_caps(&info, caps)) {
                frames++;
                last_frame_ns = gst_util_get_timestamp();
                if (buffer) last_pts = GST_BUFFER_PTS(buffer);

                /* Placeholder for later face-recognition processing.
                   RGB data is in the GstBuffer; map it when consuming frames. */
                guint64 now = gst_util_get_timestamp();
                if (now - last_stats_ns >= GST_SECOND) {
                    fprintf(stdout,
                            "INFO: frames=%llu, resolution=%ux%u, last_pts=%s\n",
                            (unsigned long long)frames,
                            GST_VIDEO_INFO_WIDTH(&info),
                            GST_VIDEO_INFO_HEIGHT(&info),
                            GST_CLOCK_TIME_IS_VALID(last_pts) ? "valid" : "n/a");
                    fflush(stdout);
                    last_stats_ns = now;
                }
            }
            gst_sample_unref(sample);
        } else {
            guint64 now = gst_util_get_timestamp();
            if (now - last_frame_ns > FRAME_TIMEOUT_NS) {
                fprintf(stderr, "WARN: no decoded frame for 5 seconds; restarting pipeline\n");
                break;
            }
        }
    }

    if (stop_requested) ok = TRUE;
    else if (frames > 0) ok = TRUE;

cleanup:
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus) gst_object_unref(bus);
    if (pipeline) gst_object_unref(pipeline);
    return ok;
}

int main(int argc, char **argv)
{
    const char *config_path = (argc > 1) ? argv[1] : CONFIG_PATH;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    gst_init(&argc, &argv);

    char *url = load_rtsp_url(config_path);
    if (!url) return EXIT_FAILURE;

    fprintf(stdout, "INFO: Wi-Fi camera client starting; config=%s\n", config_path);
    while (!stop_requested) {
        run_one_session(url);
        if (stop_requested) break;
        fprintf(stderr, "WARN: session ended; retrying in %d seconds\n", RETRY_DELAY_SEC);
        for (int i = 0; i < RETRY_DELAY_SEC && !stop_requested; i++)
            sleep(1);
    }

    /* Do not print the URL: it contains credentials. */
    memset(url, 0, strlen(url));
    free(url);
    fprintf(stdout, "INFO: stopped\n");
    return EXIT_SUCCESS;
}
