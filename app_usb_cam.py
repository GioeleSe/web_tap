from contextlib import asynccontextmanager
import subprocess
import socket
import time
from gpiozero import LED
from fastapi import FastAPI
from fastapi.responses import StreamingResponse, FileResponse
from fastapi.staticfiles import StaticFiles

blue_led = LED(26)
white_led = LED(13)
motor_A = LED(17)
motor_B = LED(27)

@asynccontextmanager
async def lifespan(app: FastAPI):
    # Startup logic
    launch_gstreamer()
    # wait_for_gstreamer()  # Uncomment if you want it to block until ready
    yield
    # Shutdown logic (optional, if you need to clean up resources later)

app = FastAPI(lifespan=lifespan)
app.mount("/static", StaticFiles(directory="static"), name="static")

def launch_gstreamer():
    pipeline = (
        "v4l2src device=/dev/video0 ! "
        "image/jpeg,width=1280,height=720,framerate=30/1 ! "  
        "jpegparse ! "            
        "multipartmux boundary=frame ! "  
        "tcpserversink host=127.0.0.1 port=5001"  
    )
    subprocess.Popen(["gst-launch-1.0"] + pipeline.split())

def stream_from_gstreamer():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect(('127.0.0.1', 5001))
        while True:
            data = s.recv(65536)  
            if not data:
                break
            yield data

def wait_for_gstreamer(host='127.0.0.1', port=5001, retries=10, delay=1):
    for i in range(retries):
        try:
            s = socket.socket()
            s.connect((host, port))
            s.close()
            print("GStreamer ready!")
            return True
        except ConnectionRefusedError:
            print(f"Waiting for GStreamer... ({i+1}/{retries})")
            time.sleep(delay)
    raise RuntimeError("GStreamer did not start in time")

@app.get('/video_feed')
def video_feed():
    return StreamingResponse(
        stream_from_gstreamer(),
        media_type='multipart/x-mixed-replace; boundary=frame'
    )

@app.get('/gpio/led/toggle/{color}')
def led_toggle(color: str):
    if color == "blue":
        blue_led.toggle()
    elif color == "white":
        white_led.toggle()

@app.get('/gpio/motor/toggle')
def motor_toggle():
    motor_A.toggle()
    motor_B.toggle()

@app.get('/gpio/motor/stop')
def motor_stop():
    motor_A.off()
    motor_B.off()
    
@app.get('/gpio/motor/start/{direction}')
def motor_start(direction: str):
    if direction == "cw":
        motor_A.on()
        motor_B.off()
    elif direction == "ccw":
        motor_A.off()
        motor_B.on()

@app.get('/')
def index():
    return FileResponse('templates/index.html')