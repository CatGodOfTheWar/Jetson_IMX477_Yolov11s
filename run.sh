#!/bin/bash

# Clear the cache (optional, useful for debugging at the beginning)
rm -rf ~/.cache/gstreamer-1.0/

# Enter the Python folder and run
echo "Starting DeepStream application with YOLOv11..."
python3 python_src/test.py