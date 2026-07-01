Import("env")
import socket

ota_ip = env.GetProjectOption("custom_ota_port", "192.168.1.82")
ota_port = 3232

def check_ota():
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(1.0)
        result = sock.connect_ex((ota_ip, ota_port))
        sock.close()
        return result == 0
    except:
        return False

print("\n--- Auto Upload Protocol Detector ---")
if check_ota():
    print(f"✅ OTA device found at {ota_ip}. Using OTA over Wi-Fi.")
    env.Replace(UPLOAD_PROTOCOL="espota")
    env.Replace(UPLOAD_PORT=ota_ip)
else:
    print(f"❌ OTA device NOT found at {ota_ip}.")
    print("🔌 Falling back to USB Serial Cable upload.")
    env.Replace(UPLOAD_PROTOCOL="esptool")
    # Clears the upload port so esptool can auto-detect the COM port
    if "UPLOAD_PORT" in env:
        del env["UPLOAD_PORT"]
print("-------------------------------------\n")
