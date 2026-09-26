from contextlib import asynccontextmanager
import subprocess
import socket
import threading, queue
import io, time, requests
from PIL import Image
from gpiozero import LED
import asyncio
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import StreamingResponse, FileResponse
from fastapi.staticfiles import StaticFiles

# --- Configuration ---
ESP32_HTTP_URL = "http://10.42.0.50/capture"

frame_queue = queue.Queue(maxsize=4)
worker_running = True

blue_led = None 
white_led = None 
motor_A = None 
motor_B = None 

def frame_capture_worker():
    global worker_running
    while worker_running:
        try:
            response = requests.get(ESP32_HTTP_URL, timeout=0.5)
            if response.status_code==200:
                img = Image.open(io.BytesIO(response.content))        
                
                buf = io.BytesIO()
                img.save(buf, format="JPEG", quality=100, optimize=False)
                frame=buf.getvalue()

                if frame_queue.full():
                    try:
                        frame_queue.get_nowait()
                    except queue.Empty:
                        pass
                frame_queue.put(frame)
            else:
                time.sleep(0.02)
        except Exception:
            time.sleep(0.1)

@asynccontextmanager
async def lifespan(app: FastAPI):
    global blue_led, white_led, motor_A, motor_B
    blue_led = LED(26)
    white_led = LED(13)
    motor_A = LED(17)
    motor_B = LED(27)


    worker_running = True
    t = threading.Thread(target=frame_capture_worker, daemon=True)
    t.start()

    yield

    worker_running = False
    if blue_led: blue_led.close()
    if white_led: white_led.close()
    if motor_A: motor_A.close()
    if motor_B: motor_B.close()    

app = FastAPI(lifespan=lifespan)
app.mount("/static", StaticFiles(directory="static"), name="static")

def generate_frames():
    while True:
        try:
            frame = frame_queue.get(timeout=0.5)
            yield (b'--frame\r\n'
                b'Content-Type: image/jpeg\r\n\r\n'+ frame + b'\r\n')
        except queue.Empty:
            time.sleep(0.05)

# def generate_frames():
#     with requests.Session() as session:
#         while True:
#             try:
#                 response = session.get(ESP32_HTTP_URL, timeout=1.5, stream=True)
#                 if response.status_code == 200:
#                     bmp_data = response.content

#                     img = Image.open(io.BytesIO(bmp_data))
#                     buf = io.BytesIO()
#                     img.save(buf, format="JPEG", quality=30)
#                     frame = buf.getvalue()

#                     yield (b'--frame\r\n'
#                             b'Content-Type: image/jpeg\r\n\r\n'+ frame + b'\r\n')
#                 else:
#                     time.sleep(0.1)
#             except (requests.exceptions.RequestException, Exception) as e:
#                 time.sleep(0.5)

@app.websocket("/ws/video")
async def video_websocket(websocket: WebSocket):
    await websocket.accept()
    try:
        while True:
            if not frame_queue.empty():
                frame = frame_queue.get_nowait()
                await websocket.send_bytes(frame)
            else:
                await asyncio.sleep(0.01)
    except(WebSocketDisconnect, Exception):
        pass

@app.get('/video_feed')
def video_feed():
    return StreamingResponse(
        generate_frames(),
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

if __name__ == '__main__':
    import uvicorn
    uvicorn.run("app:app", host="0.0.0.0", port=8000, reload=False)