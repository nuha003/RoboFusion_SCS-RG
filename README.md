# Smart Campus Safety & Response Grid (SCS-RG)

**Smart Campus Safety & Response Grid (SCS-RG)** is an AI-powered IoT system designed for real-time campus hazard detection, intelligent risk assessment, and automated emergency response.

---

## 🚀 Features

- **Real-Time Sensor Ingestion**: High-throughput REST & WebSocket endpoints accepting zone telemetry (Gas, PIR, Temperature, Humidity).
- **Intelligent Risk Fusion Engine**: State classification, sensor anomaly detection, warm-up filtering, and dynamic threat calculation.
- **Race-Condition Safe Alerts**: Conflict handling, session token authorization, and atomic incident management.
- **Resilient Architecture**: Automated state reconstruction from DB on startup, preventing data loss across restarts.
- **Interactive Dashboard**: Modern web UI displaying real-time campus layout, zone risk status, and incident logs.
- **ESP32 Node Firmware**: Arduino C++ firmware (`iot_lab_zone_node.ino`) for microcontrollers equipped with MQ-series gas and PIR motion sensors.

---

## 🛠️ Technology Stack

- **Backend**: Python 3.10+, FastAPI, Uvicorn, SQLAlchemy 2.0+, Pydantic v2, WebSockets
- **Database**: SQLite
- **Frontend**: HTML5, JavaScript, Vanilla CSS (located in `dashboard/`)
- **Firmware**: ESP32 / Arduino C++ (`iot_lab_zone_node.ino`)

---

## 📂 Directory Structure

```
├── dashboard/              # Frontend web application assets
├── check_zones.py          # Utility script to inspect zone status
├── database.py             # Database connection & session setup
├── iot_lab_zone_node.ino   # ESP32 firmware source code
├── main.py                 # FastAPI backend application & API routes
├── models.py               # SQLAlchemy models & schema definitions
├── requirements.txt        # Python package dependencies
├── risk_engine.py          # Risk classification & threat evaluation logic
├── seed.py                 # Initial data seeder script
├── seed_zones.py           # Zone database seeder script
└── simulated_zones.py      # Telemetry simulator for testing
```

---

## ⚡ Quick Start

### 1. Clone the Repository
```bash
git clone https://github.com/0001shahriar-source/scs-rg.git
cd scs-rg
```

### 2. Set Up Virtual Environment & Dependencies
```bash
python -m venv venv

# On Windows:
.\venv\Scripts\activate

# On Linux/macOS:
source venv/bin/activate

pip install -r requirements.txt
```

### 3. Seed Database & Run Server
```bash
python seed_zones.py
uvicorn main:app --reload --port 8000
```

Access the API documentation at `http://127.0.0.1:8000/docs`.

---

## 📄 License
Distributed under the MIT License. See `LICENSE` for more information.
