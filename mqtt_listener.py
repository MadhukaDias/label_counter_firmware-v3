import paho.mqtt.client as mqtt
import json

MQTT_BROKER = "broker.emqx.io"
MQTT_PORT = 1883
MQTT_TOPIC = "labelcounter/+/data"

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print(f"[SUCCESS] Connected to MQTT Broker ({MQTT_BROKER})")
        print(f"[INFO] Subscribing to topic: {MQTT_TOPIC}...\n")
        client.subscribe(MQTT_TOPIC)
        print("Waiting for machine data. Try sewing a label or starting a calibration!\n")
        print("-" * 50)
    else:
        print(f"[ERROR] Failed to connect, return code {reason_code}")

def on_message(client, userdata, msg):
    payload = msg.payload.decode("utf-8")
    try:
        data = json.loads(payload)
        
        device_id = data.get("device", "Unknown")
        
        if "count" in data:
            count = data["count"]
            print(f"[ COUNT EVENT ] Device: {device_id} | Total Labels: {count}")
            
        elif "event" in data:
            event_type = data["event"]
            if event_type == "calibration_start":
                print(f"\n[ CALIBRATION ] Device: {device_id} started Calibration mode!")
            elif event_type == "calibration_done":
                status = data.get("status", "unknown").upper()
                print(f"[ CALIBRATION ] Device: {device_id} finished calibration. Result: {status}\n")
            else:
                print(f"[ SYSTEM EVENT ] {event_type}")
                
    except json.JSONDecodeError:
        print(f"[RAW MQTT] Topic: {msg.topic} | Payload: {payload}")

if __name__ == "__main__":
    print("Initializing MTG Machine MQTT Listener...")
    
    # Create an MQTT client instance (Compatible with Paho MQTT 2.x API)
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    
    # Assign callback functions
    client.on_connect = on_connect
    client.on_message = on_message
    
    # Connect to the broker
    try:
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
    except Exception as e:
        print(f"Error connecting to broker: {e}")
        exit(1)
        
    # Blocking loop to process network traffic
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\nListener stopped by user. Exiting...")
