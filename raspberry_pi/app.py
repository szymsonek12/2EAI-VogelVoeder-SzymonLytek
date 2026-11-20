from flask import Flask, render_template, Response, jsonify, request
import requests
import json
import sqlite3
import time
import socket
from datetime import datetime

app = Flask(__name__)

# ==========================================
# 🔽 INSTELLINGEN VOOR DE ESP32
# ==========================================
ESP32_IP = "10.42.0.68"  # <-- Pas dit aan naar het IP van jouw ESP32!
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
    # Sla 1x per 10 seconden op om de database niet te overspoelen
    if time.time() - last_saved_time > 10:
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
# API ENDPOINTS
# ==========================================
@app.route('/api/sensor_data', methods=['POST'])
def receive_sensor_data():
    global latest_sensor_data
    try:
        data = request.json
        if data:
            latest_sensor_data = data
            save_to_db(data)
            return jsonify({"status": "success"}), 200
        return jsonify({"status": "error", "message": "No data received"}), 400
    except Exception as e:
        print(f"Fout bij ontvangen data: {e}")
        return jsonify({"status": "error", "message": str(e)}), 500

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

# ==========================================
# SERVO COMMANDO VIA UDP (Snel en blokkeringsvrij)
# ==========================================
@app.route('/api/servo', methods=['POST'])
def control_servo():
    data = request.json
    print(f"🔽 [DEBUG] Ontvangen van browser: {data}")
    
    try:
        # Maak een UDP socket voor snelle communicatie
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        
        if "angle" in data:
            msg = f"ANGLE:{data['angle']}"
            sock.sendto(msg.encode(), (ESP32_IP, 4210))
            print(f"🟢 [UDP] Verzonden: {msg}")
            
        elif "auto" in data:
            msg = f"MODE:{data['auto']}"
            sock.sendto(msg.encode(), (ESP32_IP, 4210))
            print(f"🟢 [UDP] Verzonden: {msg}")
            
        elif "threshold" in data:
            msg = f"THRESH:{data['threshold']}"
            sock.sendto(msg.encode(), (ESP32_IP, 4210))
            print(f"🟢 [UDP] Verzonden: {msg}")
            
        sock.close()
        return jsonify({"status": "success"}), 200
    except Exception as e:
        print(f"❌ [DEBUG] Fout: {e}")
        return jsonify({"status": "error", "message": str(e)}), 500

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
# PAGINA ROUTES
# ==========================================
@app.route('/')
def index():
    return render_template('index.html')

@app.route('/geschiedenis')
def geschiedenis():
    return render_template('history.html')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, threaded=True)
