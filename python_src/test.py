import subprocess
import time
import sys
import os
# The module required to bridge C/C++ and Python (Python Binding)
import gi 
# You must specify the version before actually importing the module
gi.require_version('Gst','1.0');
# We import GStreamer (Gst) and the event loop (GLib)
from gi.repository import Gst, GLib
# NVIDIA DeepStream library for metadata access
import pyds
import serial


# SERIAL CONFIGURATION 
try:
    uart = serial.Serial(
    port="/dev/ttyTHS1",
    baudrate=9600,
    bytesize=serial.EIGHTBITS,
    parity=serial.PARITY_NONE,
    stopbits=serial.STOPBITS_ONE,
    timeout=1)
except Exception as e:
    print(f"SERIAL ERROR: {e}")
    uart = None


# Function to create pipeline components and set their properties
def create_pipeline_element(factory_name, name, properties={}):
    print(f"The following element will be created: {name}")
    element = Gst.ElementFactory.make(factory_name, name)
    if not element:
        sys.stderr.write(f"ERROR: Failed to create element {factory_name}\n")
        sys.exit(1)
    for key_propertie, value_propertie in properties.items():
        element.set_property(key_propertie, value_propertie)
    return element


# Function for modifying metadata on each frame
def osd_metada_modifier(connection_point, information, user_data):
    # Extract the data packet from information 
    gstream_buffer = information.get_buffer()
    # Check if the buffer is empty 
    if not gstream_buffer:
        sys.stderr.write("Failed to obtain GstreamBuffer!\n")
        return Gst.PadProbeReturn.OK
    
    # Intercept metadata (inference results)
    # Batch(Lot) -> Frame(Cadru) -> Object(Obiect)
    batch_metadata = pyds.gst_buffer_get_nvds_batch_meta(hash(gstream_buffer))
    # List of frames from the batch 
    frame_list = batch_metadata.frame_meta_list
    
    # Iterate through the list of frames until the end 
    while frame_list is not None:
        try:
            '''At the memory address frame_list.data there is a structure
               NvDsFrameMeta; we cast it so we can read values such as
               frame number, resolution, etc.'''
            frame_metadata = pyds.NvDsFrameMeta.cast(frame_list.data)
        except StopIteration:
            break
        
        detected_persons = []
        
        # Reset the counter to know how many persons are in the frame
        person_count = 0
        # List of objects in the frame
        object_list = frame_metadata.obj_meta_list
        
        # Iterate through the list of detected objects in the frame
        while object_list is not None:
            try:
                '''At the memory address object_list.data there is a structure
                   NvDsObjectMeta; we cast it so we can read the class,
                   coordinates'''
                object_metadata = pyds.NvDsObjectMeta.cast(object_list.data)
            except StopIteration:
                break
            
            # --- Filter only persons (Class 0) ---
            if object_metadata.class_id == 0:
                person_count += 1
                
                # Text displayed on screen 
                object_metadata.text_params.display_text = f"ID: {object_metadata.object_id} Persoana"
                object_metadata.text_params.font_params.font_size = 28 
                object_metadata.text_params.font_params.font_name = "Serif"
                object_metadata.text_params.font_params.font_color.set(1.0, 1.0, 1.0, 1.0)
                
                # Bounding box color 
                object_metadata.rect_params.border_color.set(0.0, 1.0, 0.0, 1.0)
                detected_persons.append(object_metadata)
            
            try: 
                object_list = object_list.next
            except StopIteration:
                break
            
        if len(detected_persons) > 0:
            send_UART(detected_persons)
        
        # Print to console the number of detected persons
        # print(f"Persone detectare {person_count}")
        
        try:
            pass
            frame_list = frame_list.next
        except StopIteration:
            break
    return Gst.PadProbeReturn.OK


# Function to restart the camera process
def restart_argus_daemon():
    print("[INFO] Restarting nvargus-daemon to unlock the camera...")
    try:
        subprocess.run(["sudo", "systemctl", "restart", "nvargus-daemon"], check=True)
        time.sleep(3) 
        print("[INFO] Daemon restarted successfully.")
        
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] Failed to restart the daemon. Error code: {e.returncode}")
        sys.exit(1)
    except Exception as e:
        print(f"[ERROR] An unexpected problem occurred: {e}")


 # Function to transmit data to Arduino through UART


last_uart_time = 0


def send_UART(objects_list):
    global last_uart_time


    if uart is None or not uart.is_open:
        return


    now = time.time()
    if now - last_uart_time < 0.1:
        return
    last_uart_time = now


    if not objects_list:
        return


    target = max(
        objects_list,
        key=lambda o: o.rect_params.width * o.rect_params.height
    )


    rect = target.rect_params
    center_x = rect.left + rect.width / 2
    center_y = rect.top + rect.height / 2


    frame_center_x = 1920 / 2
    frame_center_y = 1080 / 2


    deadzone_x = 150
    deadzone_y = 100


    command = None


    if center_x > frame_center_x + deadzone_x:
        command = 1   # RIGHT
    elif center_x < frame_center_x - deadzone_x:
        command = 0   # LEFT
    elif center_y < frame_center_y - deadzone_y:
        command = 2   # UP
    elif center_y > frame_center_y + deadzone_y:
        command = 3   # DOWN


    if command is not None:
        uart.write(bytes([command]))
        #print(f"[UART] Sent byte: {command}")



            
# Main function, the entry point of our program
def main():
    Gst.init(None)
    
    # 1.DIRECTORY CONFIGS
    # Primary model config file
    current_dir =  os.path.dirname(os.path.abspath(__file__))
    config_path = os.path.join(current_dir, "../configs/yolo11s_infer.txt")
    
    # Library file and config file for tracker
    tracker_lib_file = os.path.join(current_dir, "/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so")
    tracker_config_file = os.path.join(current_dir, "../configs/config_tracker_NvDeepSORT.yml")
    
    if not os.path.exists(config_path):
        sys.stderr.write(f"YOLOv11s configuration file could not be found!\n")
        sys.exit(1)
        
    if not os.path.exists(tracker_lib_file):
        sys.stderr.write(f"Tracker library could not be found!\n")
        sys.exit(1)
        
    if not os.path.exists(tracker_config_file):
        sys.stderr.write(f"Tracker configuration file could not be found!\n")
        sys.exit(1)    
        
    # 2. CREATE GSTREAMER PIPELINE
    pipeline = Gst.Pipeline()
    
    # 3. CREATE PIPELINE ELEMENTS
    # Camera source 
    # Plugin for CSI cameras on Jetson "nvarguscamerasrc"
    # Hardware: ISP -> Image Signal Processor
    source = create_pipeline_element(
        "nvarguscamerasrc",
        "camera-source")
    
    # Set camera capabilities 
    # This forces the camera to negotiate memory correctly with DeepStream
    # We force 1080p resolution and NV12 format (the native format of the camera and DeepStream)
    caps_filter = create_pipeline_element("capsfilter", "camera-caps")
    caps_filter.set_property(
        "caps", 
        Gst.Caps.from_string("video/x-raw(memory:NVMM), width=1920, height=1080, format=NV12, framerate=30/1"))
    
    # Rotate the camera by 180 degrees 
    # Here we perform the 180-degree conversion 
    # Hardware: VIC -> Video Image Compositort
    flipper = create_pipeline_element(
        "nvvideoconvert",
        "camera-flipper",
        {'flip-method': 2})
    
    # Muxer
    # It manages the stream from multiple cameras 
    '''If it cannot receive the desired resolution, it will upscale,
       so that all images are uniform at the desired resolution;
       if a problem occurs with a camera, the system is set to wait 4 seconds 
       before forwarding the images
    '''
    # It will store the images in NVMM (Nvidia Memory Map) for fast access 
    # Hardware: GPU memory controller and VIC
    stream_muxer = create_pipeline_element(
        "nvstreammux",
        "Stream-muxer",
        {'width': 1920,
         'height': 1080,
         'batch-size': 1,
         'batched-push-timeout': 4000000})
    
    # Inference model (Yolov11s)
    # Hardware: GPU
    primary_inference_model = create_pipeline_element(
        "nvinfer",
        "primary-inference",
        {'config-file-path': config_path})
    
    # Tracker
    # Hardware: GPU and CPU
    tracker = create_pipeline_element("nvtracker", "tracker")
        
    # Set Tracker properties
    tracker.set_property('ll-lib-file', tracker_lib_file)
    tracker.set_property('ll-config-file', tracker_config_file)
    tracker.set_property('tracker-width', 640)  
    tracker.set_property('tracker-height', 384)
    
    # Converter from NV12 -> RGBA
    '''The muxer and the inference model work in NV12, which is
       memory-efficient, but the OSD needs RGBA
       in order to draw over the video'''
    # Hardware: VIC
    convertor = create_pipeline_element(
        "nvvideoconvert",
        "convertorNV12")
    
    # OSD 
    # Reads metadata from Yolo and draws over the video
    # Hardware: GPU
    osd = create_pipeline_element(
        "nvdsosd",
        "onscreendisplay")
    
    # Sink - Video output
    '''We take the final image (including the OSD drawings) from video memory
       and display it'''
    '''sync: -> tries to synchronize the display with the internal clock to render exactly 30FPS,\
       it can reduce speed and "drop frames" may appear in order to keep up, recommended "False"
       '''
    '''qos: -> signals to the source/decoder when it cannot keep up with the sink, recommended "False"
       '''
    # Hardware: GPU(display controller)
    sink = create_pipeline_element(
        "nveglglessink",
        "video-output",
        {
            'sync': False,
            'qos': False
        }) 
    
    # 4. ADD TO PIPELINE
    print("The elements will be added to the pipeline!\n")
    pipeline.add(source)
    pipeline.add(caps_filter)
    pipeline.add(flipper)
    pipeline.add(stream_muxer)
    pipeline.add(primary_inference_model)
    pipeline.add(tracker)
    pipeline.add(convertor)
    pipeline.add(osd)
    pipeline.add(sink)
    
    # 5. LINK ELEMENTS 
    print("Linking elements!\n")
    
    # Link: Source -> CapsFilter -> Flipper -> Muxer -> Yolo -> Tracker -> VideoConvertor -> OSD -> Sink
    source.link(caps_filter) 
    caps_filter.link(flipper) 
    flipper_connection_point_output = flipper.get_static_pad("src")
    mux_connection_point_input = stream_muxer.request_pad_simple("sink_0")
    if not flipper_connection_point_output or not mux_connection_point_input:
        sys.stderr.write("Error obtaining pads for the Muxer \n")
        sys.exit(1)
    flipper_connection_point_output.link(mux_connection_point_input)
    stream_muxer.link(primary_inference_model)
    primary_inference_model.link(tracker)
    tracker.link(convertor)
    convertor.link(osd)
    osd.link(sink)
    
    # 6. ADD PROBE (connection point with C/C++)
    osd_connection_point_input = osd.get_static_pad("sink")
    if not osd_connection_point_input:
        sys.stderr.write("Error obtaining the connection point with OSD\n")
    else:
        osd_connection_point_input.add_probe(Gst.PadProbeType.BUFFER, osd_metada_modifier, 0)


    # 7. START LOOP
    # Create the event loop
    loop = GLib.MainLoop()
    # Start the loop 
    pipeline.set_state(Gst.State.PLAYING)
    
    try:
        print("\nAPPLICATION STARTED")
        loop.run()
    except KeyboardInterrupt:
        print("\nStop requested by the user!")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        # Clean everything up
        # Stop the camera
        # Free all RAM/VRAM
        # Destroy the Pipeline
        pipeline.set_state(Gst.State.NULL)
    
if __name__ == "__main__":
    restart_argus_daemon()
    sys.exit(main())