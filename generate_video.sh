#!/bin/bash
# Video generation script for attractor cinematic visualizations

# Default parameters
FRAGMENTS=20
FRAMES_PER_FRAGMENT=300
OUTPUT="cinematic.mp4"
CRF=18
PRESET="fast"
FRAMERATE=60
PARTICLES=2000000
CONFIG_FILE=""
START_TYPE=""

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -n|--fragments)
            FRAGMENTS="$2"
            shift 2
            ;;
        -f|--frames)
            FRAMES_PER_FRAGMENT="$2"
            shift 2
            ;;
        -o|--output)
            OUTPUT="$2"
            shift 2
            ;;
        --crf)
            CRF="$2"
            shift 2
            ;;
        --preset)
            PRESET="$2"
            shift 2
            ;;
        --fps)
            FRAMERATE="$2"
            shift 2
            ;;
        -p|--particles)
            PARTICLES="$2"
            shift 2
            ;;
        -c|--config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        -s|--start-type)
            START_TYPE="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  -n, --fragments N         Number of attractor fragments (default: 20)"
            echo "  -f, --frames N            Frames per fragment (default: 300)"
            echo "  -p, --particles N         Number of particles (default: 2000000)"
            echo "  -c, --config FILE         Config file for zoom parameters (optional)"
            echo "  -s, --start-type N        Starting attractor: 0=Aizawa 1=Thomas 2=Lorenz 3=Halvorsen 4=Chen"
            echo "  -o, --output FILE         Output filename (default: cinematic.mp4)"
            echo "  --crf N                   Video quality 0-51, lower=better (default: 18)"
            echo "  --preset PRESET           Encoding preset: ultrafast, fast, medium, slow (default: fast)"
            echo "  --fps N                   Frame rate (default: 60)"
            echo "  -h, --help                Show this help"
            echo ""
            echo "Examples:"
            echo "  $0                                    # Generate with defaults"
            echo "  $0 -n 10 -f 60 -o test.mp4          # Quick test video"
            echo "  $0 -c attractor_config.example       # Use custom zoom config"
            echo "  $0 -n 30 --crf 15 --preset slow     # High quality, longer video"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use -h or --help for usage information"
            exit 1
            ;;
    esac
done

# Calculate total frames and duration
TOTAL_FRAMES=$((FRAGMENTS * FRAMES_PER_FRAGMENT))
DURATION=$((TOTAL_FRAMES / FRAMERATE))

echo "======================================"
echo "Attractor Cinematic Video Generator"
echo "======================================"
echo "Fragments:        $FRAGMENTS"
echo "Frames/fragment:  $FRAMES_PER_FRAGMENT"
echo "Particles:        $PARTICLES"
echo "Total frames:     $TOTAL_FRAMES"
echo "Frame rate:       ${FRAMERATE} fps"
echo "Duration:         ${DURATION} seconds"
echo "Output:           $OUTPUT"
echo "Quality (CRF):    $CRF"
echo "Preset:           $PRESET"
if [ -n "$CONFIG_FILE" ]; then
    echo "Config file:      $CONFIG_FILE"
fi
if [ -n "$START_TYPE" ]; then
    ATTRACTOR_NAMES=("Aizawa" "Thomas" "Lorenz" "Halvorsen" "Chen")
    echo "Start attractor:  ${ATTRACTOR_NAMES[$START_TYPE]} (type $START_TYPE)"
fi
echo "======================================"
echo ""

# Check if attractor_cinematic exists
if [ ! -f "./attractor_cinematic" ]; then
    echo "Error: attractor_cinematic binary not found"
    echo "Run: nvc -acc -fast -Minfo=accel -o attractor_cinematic attractor_cinematic.c -lm"
    exit 1
fi

# Check if ffmpeg exists
if ! command -v ffmpeg &> /dev/null; then
    echo "Error: ffmpeg not found. Please install ffmpeg."
    exit 1
fi

# Generate video
echo "Generating video..."
echo ""

# Build attractor command with optional config and start type
ATTRACTOR_CMD="./attractor_cinematic -n $FRAGMENTS -f $FRAMES_PER_FRAGMENT -p $PARTICLES"
if [ -n "$CONFIG_FILE" ]; then
    ATTRACTOR_CMD="$ATTRACTOR_CMD -c $CONFIG_FILE"
fi
if [ -n "$START_TYPE" ]; then
    ATTRACTOR_CMD="$ATTRACTOR_CMD -s $START_TYPE"
fi

$ATTRACTOR_CMD 2>/dev/null | \
    ffmpeg -f rawvideo -pixel_format rgb24 -video_size 1920x1080 \
    -framerate "$FRAMERATE" -i - \
    -c:v libx264 -preset "$PRESET" -crf "$CRF" \
    -g 300 -keyint_min 60 \
    -x264-params "scenecut=0:rc-lookahead=60" \
    -tune animation \
    -pix_fmt yuv420p -y "$OUTPUT" 2>&1 | \
    grep --line-buffered -E "(frame=|Duration:|video:)"

echo ""
echo "======================================"
echo "Video generation complete!"
echo "Output: $OUTPUT"
echo "======================================"
echo ""
echo "View with: ffplay $OUTPUT"
echo "        or: mpv $OUTPUT"
