import sys
import os
# The module required to bridge C/C++ and Python (Python Binding)
import gi
# You must specify the version before actually importing the module
gi.require_version('Gst', '1.0')
# We import GStreamer (Gst) and the event loop (GLib).
from gi.repository import Gst, GLib
# NVIDIA DeepStream library for metadata access
import pyds


# Function to create pipeline elements and set their properties
def create_pipeline_element(factory_name, name, properties={}):
    print(f"The following element will be created: {name}")
    element = Gst.ElementFactory.make(factory_name, name)
    if not element:
        sys.stderr.write(f"ERROR: Failed to create element {factory_name}\n")
        sys.exit(1)
    for key_propertie, value_propertie in properties.items():
        element.set_property(key_propertie, value_propertie)
    return element


# Function to modify metadata on each frame
def osd_metada_modifier(connection_point, information, user_data):
    # Extract the data packet from information
    gstream_buffer = information.get_buffer()
    # Check if the buffer is empty
    if not gstream_buffer:
        sys.stderr.write("Failed to obtain GstreamBuffer!\n")
        return Gst.PadProbeReturn.OK

    # Intercept the metadata (inference results)
    # Batch(Lot) -> Frame(Cadru) -> Object(Obiect)
    batch_metadata = pyds.gst_buffer_nvds_batch_meta(hash(gstream_buffer))
    # List of frames from the batch
    frame_list = batch_metadata.frame_meta_list

    # Iterate through the frame list until we reach the end
    while frame_list is not None:
        try:
            '''At the memory address frame_list.data there is a
               NvDsFrameMeta structure; we cast it so we can read
               frame number, resolution, etc.'''
            frame_metadata = pyds.NvDsFrameMeta.cast(frame_list.data)
        except StopIteration:
            break

        # Reset the counter to know how many persons are in the frame
        person_count = 0
        # List of objects in the frame
        object_list = frame_metadata.obj_meta_list

        # Iterate through the list of detected objects in the frame
        while object_list is not None:
            try:
                '''At the memory address object_list.data there is a
                   NvDsObjectMeta structure; we cast it so we can read
                   the class, coordinates, etc.'''
                object_metadata = pyds.NvDsObjectMeta.cast(object_list.data)
            except StopIteration:
                break

            # --- Filter only for persons (Class 0) ---
            if object_metadata.class_id == 0:
                person_count += 1

                # Text displayed on screen
                object_metadata.text_params.display_text = f"Persoana {object_metadata.object_id}"

                # Optional: bounding box border color
                object_metadata.rect_params.border_color.set(0.0, 1.0, 0.0, 1.0)

            try:
                object_list = object_list.next
            except StopIteration:
                break

        # Print to console the number of detected persons
        print(f"Persone detectare {person_count}")

        try:
            pass
            frame_list = frame_list.next
        except StopIteration:
            break


# Main function — the entry point of the program
def main():
    Gst.init(None)

    # 1. CONFIG FILE DIRECTORY
    current_dir = os.path.dirname(os.path.abspath(__file__))
    config_path = os.path.join(current_dir, "../configs/yolo11s_infer.txt")

    if not os.path.exists(config_path):
        sys.stderr.write(f"The configuration file could not be found!\n")
        sys.exit(1)

    # 2. CREATE GSTREAMER PIPELINE
    pipeline = Gst.Pipeline()

    # 3. CREATE PIPELINE ELEMENTS
    # Camera source
    # Plugin for CSI cameras on Jetson: "nvarguscamerasrc"
    # Hardware: ISP -> Image Signal Processor
    source = create_pipeline_element(
        "nvarguscamerasrc",
        "camera-source")

    # Rotate camera 180 degrees
    # We perform the 180-degree rotation here
    # Hardware: VIC -> Video Image Compositor
    flipper = create_pipeline_element(
        "nvvideoconvert",
        "camera-flipper",
        {'flip-method': 2})

    # Muxer
    # Manages the stream from multiple cameras
    '''If it cannot receive the desired resolution it will upscale,
       so all images are unified at the desired resolution;
       if a camera has an issue the system is set to forward
       frames after 4 seconds.
    '''
    # Saves images in NVMM (Nvidia Memory Map) for fast access
    # Hardware: GPU memory controller and VIC
    stream_muxer = create_pipeline_element(
        "nvstreammux",
        "Stream-muxer",
        {'width': 1280,
         'height': 720,
         'batch-size': 1,
         'batched-push-timeout': 4000000})

    # Inference model (YOLOv11s)
    # Hardware: GPU
    primary_inference_model = create_pipeline_element(
        "nvinfer",
        "primary-inference",
        {'config-file-path': config_path})

    # Converter from NV12 -> RGBA
    '''The muxer and inference model work in NV12, which is
       memory-efficient, but the OSD requires RGBA
       in order to draw overlays on the video.'''
    # Hardware: VIC
    convertor = create_pipeline_element(
        "nvvideoconvert",
        "convertorNV12")

    # OSD
    # Reads YOLO metadata and draws overlays on the video
    # Hardware: GPU
    osd = create_pipeline_element(
        "nvdsosd",
        "onscreendisplay")

    # Sink — video output
    '''We retrieve the final image (including OSD drawings) from
       video memory and display it.'''
    '''sync: -> attempts to synchronize display with the internal clock
       to render exactly 30 FPS; can slow things down and may cause
       dropped frames to keep up — recommended: False
       '''
    '''qos: -> signals to the source/decoder when the sink cannot keep up;
       recommended: False
       '''
    # Hardware: GPU (display controller)
    sink = create_pipeline_element(
        "nveglglessink",
        "video-output",
        {
            'sync': False,
            'qos': False
        })

    # 4. ADD ELEMENTS TO PIPELINE
    print("Elementele vor fi adaugate in pipeline!\n")
    pipeline.add(source)
    pipeline.add(flipper)
    pipeline.add(stream_muxer)
    pipeline.add(primary_inference_model)
    pipeline.add(convertor)
    pipeline.add(osd)
    pipeline.add(sink)

    # 5. LINK ELEMENTS
    print("Linkare la elemente!\n")

    # Link: Source -> Flipper -> Muxer
    source_connection_point_output = source.get_static_pad("src")
    flipper_connection_point_input = flipper.get_static_pad("sink")
    source_connection_point_output.link(flipper_connection_point_input)
    flipper_connection_point_output = flipper.get_static_pad("src")
    # Note: the muxer has all pads closed by default; we request pad 0 to be opened
    mux_connection_point_input = stream_muxer.request_pad_simple("sink_0")
    flipper_connection_point_output.link(mux_connection_point_input)

    '''From the muxer output we can use the simple link method:
       if el1 has a single output pad and it is compatible with el2,
       they can be linked directly.
    '''
    # Link: Muxer -> YOLO -> VideoConverter -> OSD -> Sink
    stream_muxer.link(primary_inference_model)
    primary_inference_model.link(convertor)
    convertor.link(osd)
    osd.link(sink)

    # 6. ADD PROBE (connection point with C/C++)
    osd_connection_point_input = osd.get_static_pad("sink")
    if not osd_connection_point_input:
        sys.stderr.write("Error obtaining the OSD connection point!\n")
    else:
        osd_connection_point_input.add_probe(Gst.PadProbeType.BUFFER, osd_metada_modifier, 0)

    # 7. START LOOP
    # Create the event loop
    loop = GLib.MainLoop()
    # Start the loop
    pipeline.set_state(Gst.State.PLAYING)

    try:
        print("\nAPPS STARTED")
        loop.run()
    except KeyboardInterrupt:
        print("\nStop requested by the user!")
    except Exception as e:
        print(f"Eroare: {e}")
    finally:
        # Clean up everything:
        # stop the camera, free all RAM/VRAM, destroy the pipeline
        pipeline.set_state(Gst.State.NULL)


if __name__ == "__main__":
    sys.exit(main())