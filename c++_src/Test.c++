//***********************************************************************
#include <iostream>
#include <vector>
#include <string>
#include <cstdlib>
#include <map>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <csignal> 
#include <gst/gst.h>
#include "nvdsmeta.h"
#include "gstnvdsmeta.h"
#include <stdint.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
//***********************************************************************
//* DEFINES
#define MAX_DISPLAY_LEN 128
#define FRAME_RATE 60
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
//***********************************************************************
//* BAUD_PORT & BAUD_RATE
constexpr std::string_view UART_PORT = "/dev/ttyTHS1";
constexpr speed_t UART_BAUD = B115200;
//***********************************************************************
//* COMMANDS TO FC
constexpr uint8_t CMD_LEFT = 0x00;  
constexpr uint8_t CMD_RIGHT = 0x01;  
constexpr uint8_t CMD_UP = 0x02;  
constexpr uint8_t CMD_DOWN = 0x03;  
constexpr uint8_t CMD_NO_COMMAND = 0xFF;  
//***********************************************************************
// Global loop handle required for clean shutdown via SIGINT (Ctrl+C)
// Declared static to limit visibility to this translation unit
// Must be global because signal handlers cannot capture local variables
static GMainLoop* g_loop = nullptr;
//***********************************************************************
class UartCommunication {
    private:
        int fd;
        std::chrono::steady_clock::time_point last_uart_time;

        bool write_byte(uint8_t command) { 
            ssize_t sent = write(fd, &command, 1);
            if (sent < 0) return false;
            tcdrain(fd);
            return true;
        }

    public:
        UartCommunication(): 
            fd(-1), 
            last_uart_time(std::chrono::steady_clock::now()) {}

        bool init_serial (std::string_view port = UART_PORT, speed_t baud = UART_BAUD) {
            // Just in case if you call the function for a second time
            if (fd != -1) 
                return true;
            /*  
                FLAGS
                O_WRONLY => Write only
                O_NOCTTY => Treat the port as a simple file
                O_NDELAY => We don't block the program
            */
            fd = open(port.data(), O_WRONLY | O_NOCTTY | O_NDELAY);
            if (fd == -1) 
                return false;

            struct termios options;
            // Get the settings
            tcgetattr(fd, &options);
            // Set baud rate
            cfsetospeed(&options, baud);
            /*
                PARENB => No parity bit
                CSTOP => One stop bit, not 2
                CSIZE => Delete bits size value 
                CRTSCTS => No hardware control
            */
            options.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
            /*
                CS8 => 8 bits per character
                CLOCAL => We don't have a modem , ignore it
                CREAD => Open just in case for a ACK signal 
            */
            options.c_cflag |= CS8 | CLOCAL | CREAD;
            /*
                OPOST => Stops the OS to modify the data before sent 
            */
            options.c_oflag &= ~OPOST;
            /*
                Recieve settings , but we put the 0 just in case to not block the program by accident 
            */
            options.c_cc[VMIN] = 0;
            options.c_cc[VTIME] = 0;
            return tcsetattr(fd, TCSANOW, &options) == 0;
        }

        void send_uart(const std::vector<NvDsObjectMeta*>& objects_list) {
            // Guard: if serial port is not open, skip transmission
            if (fd == -1) 
                return;

            // Rate limiting: transmit at most once every 100ms (10Hz)
            // Prevents flooding the serial bus with redundant commands
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - last_uart_time).count();
            if (elapsed < 0.1) 
                return;
            last_uart_time = now;

            // Guard: nothing to track if no persons detected in this frame
            if (objects_list.empty()) 
                return;

            // Select the closest person by finding the largest bounding box area
            // Larger area = closer to camera = highest priority target
            NvDsObjectMeta* target = *std::max_element(
                objects_list.begin(),
                objects_list.end(),
                [](NvDsObjectMeta* a, NvDsObjectMeta* b) {
                    return (a->rect_params.width * a->rect_params.height) < 
                        (b->rect_params.width * b->rect_params.height);
                }
            );

            // Calculate the center point of the target bounding box
            float center_x = target->rect_params.left + target->rect_params.width  / 2.0f;
            float center_y = target->rect_params.top  + target->rect_params.height / 2.0f;

            // Reference point: center of the 1920x1080 frame
            float frame_center_x = 1920.0f / 2.0f;
            float frame_center_y = 1080.0f / 2.0f;

            // Deadzone: tolerance area around frame center
            // Prevents jitter when target is approximately centered
            // X: ±150px horizontal, Y: ±100px vertical
            float deadzone_x = 150.0f;
            float deadzone_y = 100.0f;

            // 255 = no command (target is inside deadzone)
            uint8_t command = CMD_NO_COMMAND;

            // Determine tracking direction based on target position
            // Priority: horizontal movement checked before vertical
            if      (center_x > frame_center_x + deadzone_x) command = CMD_RIGHT;  
            else if (center_x < frame_center_x - deadzone_x) command = CMD_LEFT;  
            else if (center_y < frame_center_y - deadzone_y) command = CMD_UP;  
            else if (center_y > frame_center_y + deadzone_y) command = CMD_DOWN;  


            if (command != CMD_NO_COMMAND)
                write_byte(command);  
        }

        ~UartCommunication() {
            if (fd != -1) {
                tcdrain(fd);
                close(fd);
            }
        }
};
//***********************************************************************
class SpiCommunication {
    //! Future Build
};

//***********************************************************************
class PipelineManager {
    private:
        // Owns the UART serial port used to send tracking commands
        UartCommunication uart;

        // Restarts the nvargus-daemon service to release the CSI camera
        // Required after a crash or improper shutdown to avoid "Device Busy" errors
        void restart_argus_daemon() {
            std::cout << "[INFO] Restarting nvargus-daemon to unlock the camera!" << std::endl;
            int ret_value = system("sudo systemctl restart nvargus-daemon");
            if(ret_value != 0) {
                std::cout << "[ERROR] We couldn't restart the daemon! Error code: " << ret_value << std::endl;
                exit(1);
            }
            // Wait for the daemon to fully restart before accessing the camera
            sleep(3);
            std::cout << "[INFO] Daemon restart succesfull!" << std::endl;
        }

    public:
        // Entry point: restarts the camera daemon before pipeline initialization
        void start() {
            restart_argus_daemon();
        }
        
        // Initializes the UART serial port using the global UART_PORT and UART_BAUD constants
        bool init() {
            if (!uart.init_serial()) {
                std::cerr << "[ERROR] UART initialization failed!" << std::endl;
                return false;
            }
            return true;
        }

        // Creates and configures a GStreamer pipeline element
        /*
            factory_name => GStreamer plugin name (e.g. "nvinfer", "nvtracker")
            name => unique instance name within the pipeline
            properties => optional key-value pairs set via g_object_set()
        */
        GstElement* create_pipeline_element (
            const std::string& factory_name, 
            const std::string& name,
            const std::map<std::string, std::string>& properties = {}
        ) {
            std::cout << "[INFO] We will create the element: " << name << std::endl;
            GstElement* element = gst_element_factory_make(factory_name.c_str(), name.c_str());
            if (!element) {
                std::cerr << "[ERROR] Element couldn't be created : " << factory_name << std::endl;
                exit(1);
            }
            // Apply all key-value properties to the element
            for (const auto& [key, value] : properties) {
                g_object_set(element, key.c_str(), value.c_str(), NULL);
            }
            return element;
        }

        // GStreamer pad probe callback, intercepts every buffer passing through the OSD sink pad
        // Called once per frame by the GStreamer pipeline thread
        /*
            connection_point => the pad this probe is attached to
            information => contains the GstBuffer with frame + DeepStream metadata
            user_data => pointer to PipelineManager instance (passed via &manager in main)
        */
        static GstPadProbeReturn osd_data_modifier (
            GstPad* connection_point, 
            GstPadProbeInfo* information, 
            gpointer user_data
        ) {   
            // Recover PipelineManager instance to access uart
            PipelineManager* self = static_cast<PipelineManager*>(user_data);

            // Extract the raw GstBuffer from the probe info
            GstBuffer* gstream_buffer = GST_PAD_PROBE_INFO_BUFFER(information);
            if(!gstream_buffer) {
                std::cerr << "[ERROR] We couldn't obtain the GstreamBuffer!" << std::endl;
                return GST_PAD_PROBE_OK;
            }

            // Extract DeepStream batch metadata attached to the buffer
            // Contains all frame and object metadata produced by nvinfer and nvtracker
            NvDsBatchMeta* batch_metadata = gst_buffer_get_nvds_batch_meta(gstream_buffer);
            if(!batch_metadata) {
                std::cerr << "[ERROR] We couldn't obtain batch metadata!" << std::endl;
                return GST_PAD_PROBE_OK;
            }

            // Reused across frames to avoid per-frame memory allocation
            std::vector<NvDsObjectMeta*> detected_persons;
            // Reserve space for up to 50 persons per frame
            // Adjust based on your maximum expected detections
            detected_persons.reserve(50);

            // Iterate over all frames in the batch (batch-size=1 => single frame per loop) 
            NvDsFrameMetaList* frame_list = batch_metadata->frame_meta_list;
            while(frame_list != nullptr) {
                NvDsFrameMeta* frame_metadata = (NvDsFrameMeta*)(frame_list->data);
                if(!frame_metadata) 
                    break;

                // Clear without deallocating — reuse reserved memory for next frame
                detected_persons.clear(); 
                
                // Iterate over all detected objects in this frame
                NvDsObjectMetaList* object_list = frame_metadata->obj_meta_list;
                while(object_list != nullptr) {
                    NvDsObjectMeta* object_metadata = (NvDsObjectMeta*)(object_list->data);
                    if (!object_metadata) 
                        break;
                    
                    // Class 0 = Person (defined in YOLO label file)
                    if(object_metadata->class_id == 0) {

                        // Set overlay label: tracker ID + class name
                        snprintf(
                            object_metadata->text_params.display_text,
                            MAX_DISPLAY_LEN,
                            "ID: %lu Person",
                            object_metadata->object_id
                        );

                        // Font settings 
                        object_metadata->text_params.font_params.font_size = 28;
                        object_metadata->text_params.font_params.font_name = (char*)"Serif";
                        // White text: RGBA(1.0, 1.0, 1.0, 1.0)
                        object_metadata->text_params.font_params.font_color = {1.0, 1.0, 1.0, 1.0};
                        // Green bounding box: RGBA(0.0, 1.0, 0.0, 1.0)
                        object_metadata->rect_params.border_color = {0.0, 1.0, 0.0, 1.0};
                        object_metadata->rect_params.border_width = 2;

                        detected_persons.push_back(object_metadata);
                    }
                    object_list = object_list->next;
                }
                // Send tracking command via UART for the closest detected person
                if (!detected_persons.empty()) {
                    self->uart.send_uart(detected_persons);
                }
                frame_list = frame_list->next;
            }
            return GST_PAD_PROBE_OK;
        }
};
//***********************************************************************
int main(int argc, char* argv[]) {

    // Initialize GStreamer runtime — must be called before any GStreamer function
    // Parses GStreamer-specific command-line arguments (e.g. --gst-debug)
    gst_init(&argc, &argv);

    /*
        Create the PipelineManager object which owns:
        - UartCommunication (serial port control)
        - GStreamer pipeline elements
        - OSD metadata callback
    */
    PipelineManager manager;

    // Restart nvargus-daemon to release the CSI camera if it was left open
    // by a previous crash or improper shutdown
    manager.start();

    // Initialize UART serial port on /dev/ttyTHS1 at 115200 baud
    // Used to send tracking commands (LEFT/RIGHT/UP/DOWN) to the external device
    if (!manager.init()) {
        return 1;
    }

    //* 1.DIRECTOR CONFIGS
    std::filesystem::path exe_dir = 
        std::filesystem::canonical("/proc/self/exe").parent_path();

    //Primary model config file
    std::string config_path = (exe_dir / "configs/yolo11s_infer.txt").string();
    // Library file and config file for tracker
    std::string tracker_lib = "/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so";
    std::string tracker_config_path = (exe_dir / "configs/config_tracker_NvDeepSORT.yml").string();

    // Verify paths if they exists
    if (!std::filesystem::exists(config_path)) {
        std::cerr << "[ERROR] YOLO config not found: " << config_path << std::endl;
        return 1;
    }
    if (!std::filesystem::exists(tracker_lib)) {
        std::cerr << "[ERROR] Tracker library not found!" << std::endl;
        return 1;
    }
    if (!std::filesystem::exists(tracker_config_path)) {
        std::cerr << "[ERROR] Tracker config not found!" << std::endl;
        return 1;
    }

    //* 2.CREATE PIPELINE GSTREAM
    GstElement* pipeline = gst_pipeline_new(
        "main-pipeline"
    );

    //* 3. CREATE PIPELINE ELEMENTS
    // Camera Source
    // Plugin for CSI cameras on Jetson: "nvarguscamerasrc"
    // Hardware: ISP -> Image Signal Processor
    GstElement* source = manager.create_pipeline_element(
        "nvarguscamerasrc", "camera-source"
    );

    g_object_set(G_OBJECT(source),
        "awblock", FALSE,         
        "wbmode", 1,          
        "saturation", 1.0f,         
        "exposuretimerange", "13000 16666666", 
        "gainrange", "1 4",     
        "ispdigitalgainrange", "1 2",     
        NULL
    );

    // Set camera capabilities
    // This forces the camera to correctly negotiate memory with DeepStream
    // Forcing 1080p resolution and NV12 format (native to both camera and DeepStream)
    GstElement* caps_filter = manager.create_pipeline_element(
        "capsfilter", "camera-caps"
    );
    GstCaps* caps = gst_caps_from_string(
        "video/x-raw(memory:NVMM), width=1920, height=1080, format=NV12, framerate=" STR(FRAME_RATE) "/1"
    );
    g_object_set(caps_filter, "caps", caps, NULL);
    gst_caps_unref(caps);

    // Rotate camera by 180 degrees
    // Performing 180-degree conversion here
    // Hardware: VIC -> Video Image Compositor
    GstElement* flipper = manager.create_pipeline_element(
        "nvvideoconvert",
        "camera-flipper"
    );
    g_object_set(G_OBJECT(
        flipper), 
        "flip-method",
        2,
        NULL
    );

    // Muxer
    // Manages streams from multiple cameras
    /* 
        If it cannot receive the desired resolution, it will perform upscaling
        to ensure all images are uniform at the target resolution.
        If a camera issue occurs, the system is configured with a 4-second
        timeout to continue forwarding the remaining images.
    */
    // Saves images in NVMM (Nvidia Memory Map) for fast access
    // Hardware: GPU Memory Controller and VIC (Video Image Compositor)
    GstElement* stream_muxer = manager.create_pipeline_element(
        "nvstreammux",
        "Stream-muxer"
    );
    g_object_set(G_OBJECT(stream_muxer), 
        "width", 1920, 
        "height", 1080, 
        "batch-size", 1, 
        "batched-push-timeout", 4000000, 
        NULL
    );

    // Inference Model (Yolov11s)
    // Hardware: GPU
    GstElement* primary_inference_model = manager.create_pipeline_element(
        "nvinfer", "primary-inference",
        {{"config-file-path", config_path}}
    );

    // Tracker
    // Hardware: GPU & CPU
    GstElement* tracker = manager.create_pipeline_element(
        "nvtracker", "tracker"
    );
    // Tracker properties setup
    g_object_set(
        tracker,
        "ll-lib-file",    
        tracker_lib.c_str(),
        NULL
    );
    g_object_set(tracker, 
        "ll-config-file", 
        tracker_config_path.c_str(), 
        NULL
    );
    g_object_set(tracker, 
        "tracker-width",  
        640,                          
        NULL
    );
    g_object_set(tracker, 
        "tracker-height", 
        384,                          
        NULL
    );

    g_object_set(tracker, 
        "user-meta-pool-size", 
        256, 
        NULL
    );

    // NV12 to RGBA Converter
    /* 
        The Muxer and Inference model operate in NV12 for memory efficiency,
        but the OSD (On-Screen Display) requires RGBA to draw overlays on
        top of the video.
    */
    // Hardware: VIC (Video Image Compositor)
    GstElement* convertor = manager.create_pipeline_element(
        "nvvideoconvert", "convertorNV12"
    );

    // OSD (On-Screen Display)
    // Reads metadata from YOLO and draws overlays on the video
    // Hardware: GPU
    GstElement* osd = manager.create_pipeline_element(
        "nvdsosd", "onscreendisplay"
    );

    // Sink - Video Output
    /* 
        We take the final image (including OSD overlays) from video memory
        and display it.
    */
    /* 
        sync: -> Attempts to synchronize display with the internal clock to maintain
        exactly 30FPS; it may reduce speed and cause "dropped frames" to keep up.
        Recommendation: "False".
    */
    /* 
        qos (Quality of Service): -> Signals the source/decoder when the sink 
        cannot keep pace. Recommendation: "False".
    */
    // Hardware: GPU (Display Controller)
    // GstElement* sink = manager.create_pipeline_element(
    //     "nveglglessink", 
    //     "video-output"
    // );
    // g_object_set(G_OBJECT(sink), 
    //     "sync", FALSE, 
    //     "qos",  FALSE, 
    //     NULL
    // );

    // Converter required after OSD to prepare the image for hardware encoding
    GstElement* convertor2 = manager.create_pipeline_element(
        "nvvideoconvert", "convertor-post-osd"
    );

    GstElement* caps_filter_cpu = manager.create_pipeline_element(
        "capsfilter", "caps-cpu"
    );
    GstCaps* caps_cpu = gst_caps_from_string(
        "video/x-raw, format=I420");

    g_object_set(caps_filter_cpu,
        "caps",
        caps_cpu,
        NULL
    );

    gst_caps_unref(caps_cpu);

    // Hardware H.264 Encoder (Uses Jetson's NVENC accelerator)
    GstElement* encoder = manager.create_pipeline_element("x264enc", "h264-encoder");
    
    // !!! MEMORY SETTINGS !!!
    g_object_set(G_OBJECT(encoder), "bitrate", 2000, NULL); 
    gst_util_set_object_arg(G_OBJECT(encoder), "speed-preset", "ultrafast");
    gst_util_set_object_arg(G_OBJECT(encoder), "tune", "zerolatency");

    // Parses the H.264 stream for proper packaging
    GstElement* parser = manager.create_pipeline_element("h264parse", "h264-parser");

    // Payloads the H.264 stream for network transmission (RTP Protocol)
    GstElement* payloader = manager.create_pipeline_element("rtph264pay", "rtp-payloader");

    g_object_set(G_OBJECT(payloader), 
        "config-interval", (guint)1, // Trimite setările la fiecare secundă
        "pt", (guint)96,             // Payload Type 96 (trebuie să corespundă cu fișierul SDP)
        NULL
    );

    // Sends the payloaded data over the network via UDP
    GstElement* sink = manager.create_pipeline_element("udpsink", "udp-output");
    
    // Setăm cu CAST-URI EXPLICITE pentru port, sync și async
    g_object_set(G_OBJECT(sink), 
        "host", "192.168.0.162", // TARGET LAPTOP IP
        "port", (gint)5000,      // gint = Integer
        "sync", (gboolean)FALSE, // gboolean = Boolean
        "async", (gboolean)FALSE, 
        NULL
    );

    //* 4. ADDING TO PIPELINE
    std::cout << "[INFO] Elements will be added to the pipeline" << std::endl;
    gst_bin_add_many(
        GST_BIN(pipeline),
        source, 
        caps_filter, 
        flipper, 
        stream_muxer,
        primary_inference_model, 
        tracker, 
        convertor, 
        osd, 
        convertor2,
        caps_filter_cpu,
        encoder, 
        parser, 
        payloader, 
        sink,
        NULL
    );

    //* 5. LINKING ELEMENTS
    std::cout << "[INFO] Linking elements!" << std::endl;
    // Link: Source -> CapsFilter -> Flipper -> Muxer -> Yolo -> Tracker -> VideoConvertor -> OSD -> Sink
    gst_element_link(source, caps_filter);
    gst_element_link(caps_filter, flipper);
    GstPad* flipper_output = gst_element_get_static_pad(
        flipper, "src"
    );
    GstPad* muxer_input = gst_element_request_pad_simple(
        stream_muxer, "sink_0"
    );
    if (!flipper_output || !muxer_input) {
        std::cerr << "[ERROR] Error getting muxer pads!" << std::endl;
        return 1;
    }
    gst_pad_link(flipper_output, muxer_input);
    gst_object_unref(flipper_output);
    gst_object_unref(muxer_input);
    gst_element_link(stream_muxer, primary_inference_model);
    gst_element_link(primary_inference_model, tracker);
    gst_element_link(tracker, convertor);
    gst_element_link(convertor, osd);
    gst_element_link(osd, convertor2);
    gst_element_link(convertor2, caps_filter_cpu);
    gst_element_link(caps_filter_cpu, encoder);
    gst_element_link(encoder, parser);
    gst_element_link(parser, payloader);
    gst_element_link(payloader, sink);

    //* 6. ADDING PROBES
    GstPad* osd_input = gst_element_get_static_pad(osd, "sink");
    if (!osd_input) {
        std::cerr << "[ERROR] Error adding probe to osd!" << std::endl;
        return 1;
    }
    gst_pad_add_probe(
        osd_input,
        GST_PAD_PROBE_TYPE_BUFFER,
        PipelineManager::osd_data_modifier,
        &manager,
        NULL
    );
    gst_object_unref(osd_input);

    //* 7. STARTING THE LOOP
    // Initialize the GLib Main Loop to manage event execution and signal monitoring
    g_loop = g_main_loop_new(NULL, FALSE);

    // Set up a Signal Handler for SIGINT (Ctrl+C) to ensure a controlled shutdown
    // This allows the program to release hardware resources before exiting
    signal(SIGINT, [](int) {
        std::cout << "\n[INFO] Ctrl+C detected, stopping..." << std::endl;
        if (g_loop) g_main_loop_quit(g_loop);
    });

    // Transition the pipeline to the PLAYING state to begin data flow
    // This activates the hardware engines: ISP (Camera), GPU (Inference), and VIC (Conversion)
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    std::cout << "\n[INFO] APP STARTED" << std::endl;
    // Start the main loop (blocking call)
    // The application will remain in this loop until g_main_loop_quit() is triggered
    g_main_loop_run(g_loop);

    //* 8. CLEANING
    std::cout << "\n[INFO] STOP!" << std::endl;
    // Set pipeline state to NULL to stop hardware processing and close device handles
    gst_element_set_state(pipeline, GST_STATE_NULL);
    // Resource Deallocation: Clear pipeline and loop objects from memory
    // Critical on Jetson to prevent memory leaks and "Device Busy" errors on next run
    gst_object_unref(pipeline);
    g_main_loop_unref(g_loop);
    std::cout << "[INFO] Cleanup complete. Application exited." << std::endl;

    return 0;
}
