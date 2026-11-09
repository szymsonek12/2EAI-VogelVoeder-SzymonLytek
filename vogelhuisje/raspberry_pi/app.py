from flask import Flask, render_template, Response, jsonify
import requests
import socket
import json
import threading
import sqlite3
import time
from datetime import datetime

app = Flask(__name__)

# ==========================================
# 🔽 INSTELLINGEN VOOR DE ESP32
# ==========================================
ESP32_IP = "10.42.0.68" # <-- Pas dit aan als je ESP32 een ander IP heeft
STREAM_URL = f"http://{ESP32_IP}/stream"

latest_sensor_data = {"weight": 0.0, "scale_ready": False, "temp": None, "humidity": None}

# ==========================================
# DATABASE FUNCTIES
# ==========================================
DB_FILE = "sensor_data.db"
last_saved_time = 0 

def init_db():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute('''CREATE TABLE IF NOT EXISTS metingen
                 (id INTEGER PRIMARY KEY AUTOINCREMENT,
                  tijdstip TEXT,
                  gewicht REAL,
                  temperatuur REAL,
                  vochtigheid REAL)''')
    conn.commit()
    conn.close()

def save_to_db(data):
    global last_saved_time
    if time.time() - last_saved_time > 10: # Sla 1x per 10 sec op
        if data.get("temp") is not None and data.get("humidity") is not None:
            conn = sqlite3.connect(DB_FILE)
            c = conn.cursor()
            nu = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            c.execute("INSERT INTO metingen (tijdstip, gewicht, temperatuur, vochtigheid) VALUES (?, ?, ?, ?)",
                      (nu, data.get("weight", 0), data.get("temp", 0), data.get("humidity", 0)))
            conn.commit()
            conn.close()
            last_saved_time = time.time()

init_db()

# ==========================================
# UDP LISTENER
# ==========================================
def udp_listener():
    global latest_sensor_data
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 1234))
    print("UDP Listener gestart op poort 1234...")
    while True:
        data, addr = sock.recvfrom(1024)
        try:
            latest_sensor_data = json.loads(data.decode('utf-8'))
            save_to_db(latest_sensor_data)
        except Exception as e:
            print(f"Fout bij parsen UDP data: {e}")

threading.Thread(target=udp_listener, daemon=True).start()

# ==========================================
# VIDEO STREAM
# ==========================================
def generate_frames():
    try:
        stream = requests.get(STREAM_URL, stream=True, timeout=5)
        for chunk in stream.iter_content(chunk_size=1024):
            if chunk:
                yield chunk
    except Exception as e:
        print(f"Fout bij video stream: {e}")

@app.route('/video_feed')
def video_feed():
    return Response(generate_frames(), mimetype='multipart/x-mixed-replace; boundary=123456789000000000000987654321')

# ==========================================
# API ENDPOINTS
# ==========================================
@app.route('/api/data')
def get_data():
    return jsonify(latest_sensor_data)

@app.route('/api/history')
def get_history():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("SELECT tijdstip, gewicht, temperatuur, vochtigheid FROM metingen ORDER BY id DESC LIMIT 50")
    rows = c.fetchall()
    conn.close()
    rows.reverse()
    history = [{"tijdstip": r[0], "gewicht": r[1], "temperatuur": r[2], "vochtigheid": r[3]} for r in rows]
    return jsonify(history)

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/geschiedenis')
def geschiedenis():
    return render_template('history.html')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, threaded=True)