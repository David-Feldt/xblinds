#!/bin/bash

# Default values
SKETCH_NAME="blinds_controller/blinds_controller"
# SKETCH_NAME="driver/driver"

BOARD="esp8266:esp8266:d1_mini"
BAUDRATE=115200

# Function to find USB serial port
find_serial_port() {
    # Look for USB serial devices
    PORT=$(ls /dev/cu.usbserial-* 2>/dev/null | head -n 1)
    
    if [ -z "$PORT" ]; then
        echo "No USB serial port found"
        exit 1
    fi
    
    echo "Found serial port: $PORT"
}

# Function to show usage
show_usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -b, --build     Build the sketch"
    echo "  -u, --upload    Upload the sketch"
    echo "  -m, --monitor   Monitor serial output"
    echo "  -a, --all       Build, upload and monitor (all in one)"
    echo "  -s, --sketch    Specify sketch name (default: $SKETCH_NAME)"
    echo "  -h, --help      Show this help message"
    echo ""
    echo "Example:"
    echo "  $0 -b -u -m    # Build, upload and monitor"
    echo "  $0 -a          # Build, upload and monitor in one command"
    echo "  $0 -s my_sketch -b  # Build specific sketch"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -b|--build)
            BUILD=true
            shift
            ;;
        -u|--upload)
            UPLOAD=true
            shift
            ;;
        -m|--monitor)
            MONITOR=true
            shift
            ;;
        -a|--all)
            BUILD=true
            UPLOAD=true
            MONITOR=true
            shift
            ;;
        -s|--sketch)
            SKETCH_NAME="$2"
            shift 2
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
done

# If no options provided, show usage
if [ -z "$BUILD" ] && [ -z "$UPLOAD" ] && [ -z "$MONITOR" ]; then
    show_usage
    exit 1
fi

# Build the sketch
if [ "$BUILD" = true ]; then
    echo "Building $SKETCH_NAME..."
    arduino-cli compile --fqbn $BOARD $SKETCH_NAME.ino
    if [ $? -ne 0 ]; then
        echo "Build failed"
        exit 1
    fi
fi

# Upload the sketch
if [ "$UPLOAD" = true ]; then
    find_serial_port
    echo "Uploading $SKETCH_NAME to $PORT..."
    arduino-cli upload -p $PORT --fqbn $BOARD $SKETCH_NAME.ino
    if [ $? -ne 0 ]; then
        echo "Upload failed"
        exit 1
    fi
fi

# Monitor serial output
if [ "$MONITOR" = true ]; then
    find_serial_port
    echo "Monitoring $PORT at $BAUDRATE baud..."
    arduino-cli monitor -p $PORT -c baudrate=$BAUDRATE
fi 