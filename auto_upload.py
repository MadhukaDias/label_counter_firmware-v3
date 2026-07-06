Import("env")
import socket

try:
    import serial.tools.list_ports
    has_serial = True
except ImportError:
    has_serial = False

ota_ip = env.GetProjectOption("custom_ota_port", "192.168.1.82")

def has_usb_serial():
    if not has_serial: return False
    for p in serial.tools.list_ports.comports():
        # Any port with a Vendor ID is typically a USB device
        if p.vid is not None:
            return True
    return False

def check_ota():
    try:
        # Check port 80 (Web Server) since it uses TCP and we know it's always running on the device.
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(1.0)
        result = sock.connect_ex((ota_ip, 80))
        sock.close()
        return result == 0
    except:
        return False

print("\n--- Auto Upload Protocol Detector ---")

if has_usb_serial():
    print("[OK] USB Serial device detected. Priority: Serial.")
    env.Replace(UPLOAD_PROTOCOL="esptool")
    if "UPLOAD_PORT" in env:
        del env["UPLOAD_PORT"]
else:
    print("[INFO] No USB Serial device found. Trying OTA...")
    if check_ota():
        print(f"[OK] OTA device found at {ota_ip}. Using OTA over Wi-Fi.")
        env.Replace(UPLOAD_PROTOCOL="espota")
        env.Replace(UPLOAD_PORT=ota_ip)
    else:
        print(f"[FAIL] Neither USB Serial nor OTA device ({ota_ip}) found!")
        print("[FAIL] Upload will fail. Please connect the device.")
        env.Replace(UPLOAD_PROTOCOL="esptool")
        if "UPLOAD_PORT" in env:
            del env["UPLOAD_PORT"]

print("-------------------------------------\n")
